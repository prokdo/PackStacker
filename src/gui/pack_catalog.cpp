#include "gui/pack_catalog.hpp"

#include "core/metadata.hpp"
#include "core/model.hpp"
#include "core/pack_source.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QDirListing>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <qcontainerfwd.h>
#include <qhashfunctions.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qtypes.h>

#include <algorithm>
#include <expected>
#include <limits>
#include <stop_token>
#include <utility>
#include <vector>

namespace PackStacker
{
    namespace
    {
        QString formatText(PackFormat format)
        {
            return format.Minor == 0
                       ? QString::number(format.Major)
                       : QString::number(format.Major) + '.' + QString::number(format.Minor);
        }

        QString descriptionText(const QJsonValue& value, qsizetype limit)
        {
            auto pending = std::vector<std::pair<QJsonValue, qint32>>{{value, 0}};
            auto text = QString{};
            while (!pending.empty() && text.size() < limit)
            {
                const auto [part, depth] = std::move(pending.back());
                pending.pop_back();
                if (depth > Limits{}.MaxJsonDepth)
                {
                    continue;
                }
                if (part.isString())
                {
                    text += part.toString().left(limit - text.size());
                }
                else if (part.isArray())
                {
                    const auto array = part.toArray();
                    for (auto index = array.size(); index > 0; --index)
                    {
                        pending.emplace_back(array.at(index - 1), depth + 1);
                    }
                }
                else if (part.isObject())
                {
                    const auto object = part.toObject();
                    auto prefix = object.value("text").toString();
                    if (prefix.isEmpty() && object.contains("translate"))
                    {
                        prefix = object.value("translate").toString();
                    }
                    text += prefix.left(limit - text.size());
                    pending.emplace_back(object.value("extra"), depth + 1);
                }
            }
            return text;
        }

        void inspectIcon(CatalogEntry& row, const PackSource& source, const std::stop_token& stop)
        {
            if (!source.Entries().contains("pack.png"))
            {
                return;
            }
            auto icon = source.Read("pack.png", quint64{512} * 1024, stop);
            if (!icon)
            {
                row.Warning = QCoreApplication::translate("PackCatalog", "Значок: %1")
                                  .arg(QString::fromStdString(icon.error().Message));
                return;
            }
            auto iconBytes = QByteArray::fromStdString(*icon);
            auto input = QBuffer(&iconBytes);
            input.open(QIODevice::ReadOnly);
            auto reader = QImageReader(&input, "png");
            const auto size = reader.size();
            if (!size.isValid() || size.width() > 1024 || size.height() > 1024)
            {
                row.Warning = QCoreApplication::translate(
                    "PackCatalog", "Значок PNG должен быть не больше 1024×1024.");
                return;
            }
            const auto image = reader.read();
            if (image.isNull())
            {
                row.Warning = QCoreApplication::translate("PackCatalog", "Значок: %1")
                                  .arg(reader.errorString());
                return;
            }
            auto thumbnail = QByteArray{};
            auto output = QBuffer(&thumbnail);
            output.open(QIODevice::WriteOnly);
            if (!image.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                     .save(&output, "PNG"))
            {
                row.Warning =
                    QCoreApplication::translate("PackCatalog", "Не удалось подготовить значок.");
                return;
            }
            row.Icon = QStringLiteral("data:image/png;base64,") +
                       QString::fromLatin1(thumbnail.toBase64());
        }

        bool containsContent(const PackSource& source, std::string_view prefix)
        {
            const auto& entries = source.Entries();
            const auto entry = entries.lower_bound(prefix);
            return entry != entries.end() && entry->first.starts_with(prefix);
        }

        void describeContent(CatalogEntry& row, const PackSource& source, const Metadata& metadata)
        {
            row.Content = {
                .HasAssets = containsContent(source, "assets/"),
                .HasData = containsContent(source, "data/"),
            };
            row.Overlays.reserve(metadata.Overlays.size());
            for (const auto& overlay : metadata.Overlays)
            {
                row.Overlays.push_back({
                    .Formats = overlay.Formats,
                    .Content =
                        {
                            .HasAssets = containsContent(source, overlay.Directory + "/assets/"),
                            .HasData = containsContent(source, overlay.Directory + "/data/"),
                        },
                });
            }
        }

        CatalogEntry inspect(const QString& path, const std::stop_token& stop)
        {
            auto row = CatalogPlaceholder(path);
            if (!path.isValidUtf16() || path.contains(QChar(0)))
            {
                row.Error = QStringLiteral("Invalid pack path");
                return row;
            }
            auto source = PackSource::Open(path.toStdString(), {}, stop);
            if (!source)
            {
                row.Error = QString::fromStdString(source.error().Message);
                return row;
            }
            row.Storage = QFileInfo(path).isDir()
                              ? QCoreApplication::translate("PackCatalog", "ПАПКА")
                              : QStringLiteral("ZIP");
            auto metadata = ReadMetadata(**source, {}, stop);
            if (!metadata)
            {
                row.Error = QString::fromStdString(metadata.error().Message);
                return row;
            }
            row.Description.clear();
            auto bytes = (*source)->Read("pack.mcmeta", Limits{}.MaxJsonBytes, stop);
            if (!bytes)
            {
                row.Error = QString::fromStdString(bytes.error().Message);
                return row;
            }
            const auto pack = QJsonDocument::fromJson(QByteArray::fromStdString(*bytes))
                                  .object()
                                  .value("pack")
                                  .toObject();
            describeContent(row, **source, *metadata);
            row.OutputDescription =
                descriptionText(pack.value("description"), Limits{}.MaxJsonBytes);
            const auto text = row.OutputDescription.left(512);
            auto clean = QString{};
            for (auto index = qsizetype{0}; index < text.size(); ++index)
            {
                if (text.at(index) == QChar(0x00a7) && index + 1 < text.size())
                {
                    ++index;
                    continue;
                }
                clean += text.at(index);
            }
            row.Description = clean.simplified();
            const auto minimum = metadata->Formats.Minimum;
            const auto maximum = metadata->Formats.Maximum;
            if (!pack.contains("min_format") && !pack.contains("max_format"))
            {
                row.AutoFormat = QString::number(pack.value("pack_format").toInt());
            }
            else if (minimum == maximum)
            {
                row.AutoFormat = formatText(minimum);
            }
            row.Format =
                minimum == maximum || (minimum.Major == maximum.Major && minimum.Minor == 0 &&
                                       maximum.Minor == std::numeric_limits<qint32>::max())
                    ? formatText(minimum)
                    : formatText(minimum) + QStringLiteral(" – ") +
                          (maximum.Minor == std::numeric_limits<qint32>::max()
                               ? QString::number(maximum.Major) + ".x"
                               : formatText(maximum));
            row.Size = QLocale().formattedDataSize(static_cast<qint64>((*source)->TotalBytes()));
            inspectIcon(row, **source, stop);
            return row;
        }

        Result<QStringList> catalogPaths(const QString& directory, const std::stop_token& stop)
        {
            if (directory.isEmpty())
            {
                return QStringList{};
            }
            if (auto valid = CheckPhysicalPath(directory.toStdString()); !valid)
            {
                return std::unexpected(valid.error());
            }
            const auto root = QFileInfo(directory);
            if (!root.isDir() || !root.isReadable())
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::IO,
                    .Message = "Cannot read source directory",
                    .Pack = {},
                    .Path = directory.toStdString(),
                });
            }
            auto paths = QStringList{};
            auto visited = quint64{0};
            for (const auto& entry :
                 QDirListing(directory, QDirListing::IteratorFlag::ExcludeOther |
                                            QDirListing::IteratorFlag::IncludeHidden))
            {
                if (stop.stop_requested())
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::CANCELLED,
                        .Message = "Cancelled",
                        .Pack = {},
                        .Path = directory.toStdString(),
                    });
                }
                if (++visited > 10000)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::LIMIT_EXCEEDED,
                        .Message = "Source directory entry limit exceeded",
                        .Pack = {},
                        .Path = directory.toStdString(),
                    });
                }
                const auto info = entry.fileInfo();
                if (info.isHidden())
                {
                    continue;
                }
                if ((info.isDir() &&
                     QFileInfo(QDir(info.filePath()).filePath("pack.mcmeta")).exists()) ||
                    info.fileName().endsWith(".zip", Qt::CaseInsensitive))
                {
                    paths.append(QDir::cleanPath(info.absoluteFilePath()));
                    if (std::cmp_greater(paths.size(), Limits{}.MaxPacks))
                    {
                        return std::unexpected(Diagnostic{
                            .Code = ErrorCode::LIMIT_EXCEEDED,
                            .Message = "Catalog pack limit exceeded",
                            .Pack = {},
                            .Path = directory.toStdString(),
                        });
                    }
                }
            }
            std::ranges::sort(paths,
                              [](const auto& first, const auto& second)
                              {
                                  const auto order = first.compare(second, Qt::CaseInsensitive);
                                  return order == 0 ? first < second : order < 0;
                              });
            return paths;
        }
    }

    QString CatalogPathKey(const QString& path)
    {
#ifdef Q_OS_WIN
        return QDir::cleanPath(path).toCaseFolded();
#else
        return QDir::cleanPath(path);
#endif
    }

    CatalogEntry CatalogPlaceholder(const QString& path)
    {
        auto name = QFileInfo(path).fileName();
        if (name.endsWith(".zip", Qt::CaseInsensitive))
        {
            name.chop(4);
        }
        auto entry = CatalogEntry{};
        entry.Path = path;
        entry.Name = std::move(name);
        entry.Description =
            QCoreApplication::translate("PackCatalog", "Метаданные ещё не прочитаны");
        entry.Storage = QCoreApplication::translate("PackCatalog", "ПАК");
        return entry;
    }

    QVariantMap CatalogEntry::ToRow() const
    {
        return {
            {"path", Path},
            {"name", Name},
            {"description", Description},
            {"outputDescription", OutputDescription},
            {"storage", Storage},
            {"format", Format},
            {"autoFormat", AutoFormat},
            {"size", Size},
            {"icon", Icon},
            {"error", Error},
            {"warning", Warning},
        };
    }

    Result<CatalogSnapshot> ReadCatalog(const QString& directory, const QStringList& selected,
                                        const std::stop_token& stop)
    {
        if (std::cmp_greater(selected.size(), Limits{}.MaxPacks))
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::LIMIT_EXCEEDED,
                .Message = "Preview pack limit exceeded",
                .Pack = {},
                .Path = directory.toStdString(),
            });
        }
        const auto paths = catalogPaths(directory, stop);
        if (!paths)
        {
            return std::unexpected(paths.error());
        }
        auto result = CatalogSnapshot{};
        for (const auto& path : *paths + selected)
        {
            if (stop.stop_requested())
            {
                return std::unexpected(Diagnostic{
                    .Code = ErrorCode::CANCELLED,
                    .Message = "Cancelled",
                    .Pack = {},
                    .Path = path.toStdString(),
                });
            }
            const auto key = CatalogPathKey(path);
            if (!result.Details.contains(key))
            {
                result.Details.insert(key, inspect(path, stop));
            }
        }
        if (stop.stop_requested())
        {
            return std::unexpected(Diagnostic{
                .Code = ErrorCode::CANCELLED,
                .Message = "Cancelled",
                .Pack = {},
                .Path = directory.toStdString(),
            });
        }
        result.Available = *paths;
        return result;
    }
}
