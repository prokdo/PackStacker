#include "gui/pack_controller.hpp"

#include "core/model.hpp"
#include "core/pack.hpp"
#include "core/pack_source.hpp"
#include "core/rules.hpp"
#include "gui/pack_catalog.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <qcontainerfwd.h>
#include <qhashfunctions.h>
#include <qlist.h>
#include <qnamespace.h>
#include <qobjectdefs.h>
#include <qtmetamacros.h>
#include <qtypes.h>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace PackStacker
{
    namespace
    {
        std::optional<PackFormat> parseFormat(const QString& text)
        {
            const auto parts = text.split('.');
            if (parts.size() > 2)
            {
                return std::nullopt;
            }
            auto result = PackFormat{};
            for (auto index = qsizetype{0}; index < parts.size(); ++index)
            {
                if (parts.at(index).isEmpty())
                {
                    return std::nullopt;
                }
                for (const auto character : parts.at(index))
                {
                    if (character < QChar('0') || character > QChar('9'))
                    {
                        return std::nullopt;
                    }
                }
                auto converted = false;
                const auto number = parts.at(index).toInt(&converted);
                if (!converted)
                {
                    return std::nullopt;
                }
                (index == 0 ? result.Major : result.Minor) = number;
            }
            return result.Major >= 4 ? std::optional(result) : std::nullopt;
        }

        QString diagnosticText(const Diagnostic& diagnostic)
        {
            auto message = QString::fromStdString(diagnostic.Message);
            if (!diagnostic.Pack.empty())
            {
                message +=
                    PackController::tr("\nPack: %1").arg(QString::fromStdString(diagnostic.Pack));
            }
            if (!diagnostic.Path.empty())
            {
                message +=
                    PackController::tr("\nPath: %1").arg(QString::fromStdString(diagnostic.Path));
            }
            return message;
        }

        Result<std::vector<MergeRule>> coreRules(const QVariantList& values)
        {
            auto entries = std::vector<MergeRule>{};
            entries.reserve(static_cast<std::size_t>(values.size()));
            for (const auto& value : values)
            {
                const auto row = value.toMap();
                const auto match = row.value("match");
                const auto strategy = row.value("strategy");
                if (row.size() != 2 || match.typeId() != QMetaType::QString ||
                    strategy.typeId() != QMetaType::QString || !match.toString().isValidUtf16() ||
                    !strategy.toString().isValidUtf16())
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::INVALID_METADATA,
                        .Message = "Invalid merge rule",
                        .Pack = {},
                        .Path = {},
                    });
                }
                const auto name = strategy.toString().toStdString();
                const auto parsed = ParseStrategyName(name);
                if (!parsed)
                {
                    return std::unexpected(Diagnostic{
                        .Code = ErrorCode::INVALID_METADATA,
                        .Message = "Unknown merge strategy: " + name,
                        .Pack = {},
                        .Path = {},
                    });
                }
                entries.push_back({.Match = match.toString().toStdString(), .Strategy = *parsed});
            }
            if (auto valid = ValidateRules(entries); !valid)
            {
                return std::unexpected(valid.error());
            }
            return entries;
        }

        QVariantList ruleRows(const std::vector<MergeRule>& values)
        {
            auto rows = QVariantList{};
            for (const auto& rule : values)
            {
                rows.append(QVariantMap{
                    {"match", QString::fromStdString(rule.Match)},
                    {"strategy", QString::fromUtf8(StrategyName(rule.Strategy))},
                });
            }
            return rows;
        }

        struct JobResult
        {
            PackStacker::Result<void> Result;
            quint64 FileCount = 0;
        };
    }

    PackController::PackController(QObject* parent) : QObject(parent)
    {
    }

    PackController::~PackController()
    {
        stop.request_stop();
        scanStop.request_stop();
        if (worker)
        {
            worker->wait();
        }
        if (scanner)
        {
            scanner->wait();
        }
    }

    QStringList PackController::Packs() const
    {
        return packs;
    }
    bool PackController::Busy() const
    {
        return busy;
    }
    bool PackController::Cancelling() const
    {
        return (busy && stop.stop_requested()) || (scanning && scanStop.stop_requested());
    }
    bool PackController::Failed() const
    {
        return failed;
    }
    QString PackController::Status() const
    {
        return status;
    }

    QString PackController::ResultPath() const
    {
        return resultPath;
    }

    QString PackController::DefaultOutputPath(const QString& directory, qint32 outputFormat)
    {
        if (directory.isEmpty() || (outputFormat != 0 && outputFormat != 1))
        {
            return {};
        }
        return QDir(directory).filePath(outputFormat == 0 ? QStringLiteral("combined-pack.zip")
                                                          : QStringLiteral("combined-pack"));
    }

    QVariantMap PackController::ValidateInputs(const QString& target, const QString& output)
    {
        auto targetError = QString{};
        if (target.isEmpty())
        {
            targetError = tr("Укажите точный pack format.");
        }
        else if (!parseFormat(target))
        {
            targetError =
                tr("Формат: целое число от 4 или MAJOR.MINOR, компоненты не больше 2147483647.");
        }

        auto outputError = QString{};
        const auto destination = QFileInfo(output);
        if (output.isEmpty())
        {
            outputError = tr("Укажите путь нового результата.");
        }
        else if (!output.isValidUtf16() || output.contains(QChar(0)) ||
                 !destination.fileName().isValidUtf16() ||
                 !ValidateLogicalPath(destination.fileName().toStdString(), {}))
        {
            outputError = tr("Некорректное имя результата.");
        }
        else if (!destination.absoluteFilePath().isValidUtf16() ||
                 !CheckPhysicalPath(destination.absoluteFilePath().toStdString()))
        {
            outputError = tr("Путь не должен содержать символические ссылки или junction.");
        }
        else if (destination.exists())
        {
            outputError = tr("Этот путь уже существует. Выберите новый: перезапись запрещена.");
        }
        else if (const auto parent = QFileInfo(destination.absolutePath());
                 !parent.isDir() || !parent.isWritable())
        {
            outputError =
                tr("Родительский каталог должен существовать и быть доступен для записи.");
        }
        return {{"target", targetError}, {"output", outputError}};
    }

    QString PackController::PackTypeWarning(const QString& path, qint32 type,
                                            const QString& target) const
    {
        const auto entry = details.constFind(CatalogPathKey(path));
        if (entry == details.cend())
        {
            return {};
        }
        auto content = entry->Content;
        if (const auto format = parseFormat(target))
        {
            for (const auto& overlay : entry->Overlays)
            {
                if (overlay.Formats.Contains(*format))
                {
                    content.HasAssets |= overlay.Content.HasAssets;
                    content.HasData |= overlay.Content.HasData;
                }
            }
        }
        if (content.HasAssets && content.HasData)
        {
            return tr("Этот пак содержит и ресурсы, и данные. Смешанные паки не поддерживаются.");
        }
        if (type == 0 && content.HasData)
        {
            return tr("Это датапак. Выберите тип «Датапак».");
        }
        if (type == 1 && content.HasAssets)
        {
            return tr("Это ресурспак. Выберите тип «Ресурспак».");
        }
        return {};
    }

    bool PackController::OpenResultFolder()
    {
        if (resultPath.isEmpty())
        {
            return false;
        }
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(resultPath).absolutePath())))
        {
            return reject(tr("Не удалось открыть папку результата."));
        }
        return true;
    }

    QString PackController::SourceDirectory() const
    {
        return sourceDirectory;
    }
    bool PackController::Scanning() const
    {
        return scanning;
    }

    QVariantList PackController::AvailablePacks() const
    {
        auto available = QVariantList{};
        for (const auto& path : availablePaths())
        {
            available.append(catalogEntry(path).ToRow());
        }
        return available;
    }

    QStringList PackController::availablePaths() const
    {
        auto available = QStringList{};
        for (const auto& entry : library)
        {
            const auto key = CatalogPathKey(entry);
            auto selected = false;
            for (const auto& path : packs)
            {
                selected |= CatalogPathKey(path) == key;
            }
            if (!selected)
            {
                available.append(entry);
            }
        }
        return available;
    }

    QVariantList PackController::SelectedPacks() const
    {
        auto selected = QVariantList{};
        for (const auto& path : packs)
        {
            selected.append(catalogEntry(path).ToRow());
        }
        return selected;
    }

    CatalogEntry PackController::catalogEntry(const QString& path) const
    {
        const auto found = details.constFind(CatalogPathKey(path));
        return found == details.cend() ? CatalogPlaceholder(path) : *found;
    }

    bool PackController::SetSourceDirectory(const QUrl& url)
    {
        if (busy || scanning)
        {
            return false;
        }
        const auto path = LocalPath(url);
        if (path.isEmpty())
        {
            return reject(tr("Выберите локальный каталог с паками."));
        }
        return beginScan(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    }

    bool PackController::RefreshCatalog()
    {
        return beginScan(sourceDirectory);
    }

    bool PackController::beginScan(const QString& directory)
    {
        if (busy || scanning)
        {
            return false;
        }
        scanStop = std::stop_source{};
        const auto token = scanStop.get_token();
        const auto result = std::make_shared<Result<CatalogSnapshot>>();
        scanner.reset(QThread::create([directory, selected = packs, token, result]
                                      { *result = ReadCatalog(directory, selected, token); }));
        connect(
            scanner.get(), &QThread::finished, this,
            [this, directory, result]
            {
                scanner->wait();
                scanner.reset();
                scanning = false;
                failed = !*result && result->error().Code != ErrorCode::CANCELLED;
                if (*result)
                {
                    sourceDirectory = directory;
                    library = std::move(result->value().Available);
                    details = std::move(result->value().Details);
                    status = tr("Каталог обновлён. Выберите паки для добавления в стек.");
                    emit CatalogChanged();
                }
                else
                {
                    status = result->error().Code == ErrorCode::CANCELLED
                                 ? tr("Сканирование отменено.")
                                 : diagnosticText(result->error());
                }
                emit StateChanged();
                emit CatalogFinished(result->has_value());
            },
            Qt::QueuedConnection);
        scanning = true;
        failed = false;
        status = tr("Чтение каталога и метаданных…");
        emit StateChanged();
        scanner->start();
        return true;
    }

    bool PackController::AddAvailable(qint32 index)
    {
        if (busy || scanning)
        {
            return false;
        }
        const auto available = availablePaths();
        if (index < 0 || index >= available.size())
        {
            return false;
        }
        const auto& path = available.at(index);
        const auto entry = details.constFind(CatalogPathKey(path));
        if (entry != details.cend() && !entry->Error.isEmpty())
        {
            return reject(entry->Error);
        }
        return AddPack(QUrl::fromLocalFile(path));
    }

    void PackController::returnToCatalog(const QString& path)
    {
        const auto key = CatalogPathKey(path);
        for (const auto& entry : library)
        {
            if (CatalogPathKey(entry) == key)
            {
                return;
            }
        }
        library.append(path);
    }

    bool PackController::ClearPacks()
    {
        if (busy || scanning || packs.isEmpty())
        {
            return false;
        }
        for (const auto& path : packs)
        {
            returnToCatalog(path);
        }
        packs.clear();
        emit PacksChanged();
        emit CatalogChanged();
        return true;
    }

    bool PackController::reject(const QString& message)
    {
        status = message;
        failed = true;
        emit StateChanged();
        return false;
    }

    QString PackController::LocalPath(const QUrl& url)
    {
        if (!url.isValid() || !url.isLocalFile() || url.hasQuery() || url.hasFragment())
        {
            return {};
        }
        const auto path = url.toLocalFile();
        if (path.isEmpty() || !path.isValidUtf16() || path.contains(QChar(0)))
        {
            return {};
        }
        return QDir::cleanPath(path);
    }

    bool PackController::AddPack(const QUrl& url)
    {
        return AddPacks({url});
    }

    bool PackController::AddPacks(const QList<QUrl>& urls)
    {
        if (busy || scanning)
        {
            return false;
        }
        if (urls.isEmpty())
        {
            return reject(tr("Выберите хотя бы одну папку пака или ZIP."));
        }
        const auto limit = Limits{}.MaxPacks;
        const auto count = static_cast<quint64>(urls.size());
        if (count > limit || static_cast<quint64>(packs.size()) > limit - count)
        {
            return reject(tr("Превышен лимит паков. Ни один вход не добавлен."));
        }
#ifdef Q_OS_WIN
        const auto sensitivity = Qt::CaseInsensitive;
#else
        const auto sensitivity = Qt::CaseSensitive;
#endif
        auto updated = packs;
        for (const auto& url : urls)
        {
            const auto path = LocalPath(url);
            if (path.isEmpty())
            {
                return reject(tr(
                    "Входы должны быть локальными файловыми URL без query и fragment. Ни один вход "
                    "не добавлен."));
            }
            const auto absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
            if (updated.contains(absolute, sensitivity))
            {
                return reject(
                    tr("Повторяющийся вход: %1\nНи один вход не добавлен.").arg(absolute));
            }
            updated.append(absolute);
        }
        packs = std::move(updated);
        failed = false;
        status = tr("Добавлено паков: %1. Новые входы имеют меньший приоритет.").arg(urls.size());
        emit PacksChanged();
        emit CatalogChanged();
        emit StateChanged();
        return true;
    }

    bool PackController::RemovePack(qint32 index)
    {
        if (busy || scanning || index < 0 || index >= packs.size())
        {
            return false;
        }
        returnToCatalog(packs.at(index));
        packs.removeAt(index);
        emit PacksChanged();
        emit CatalogChanged();
        return true;
    }

    bool PackController::MovePack(qint32 sourceIndex, qint32 destinationIndex)
    {
        if (busy || scanning || sourceIndex < 0 || destinationIndex < 0 ||
            sourceIndex >= packs.size() || destinationIndex >= packs.size() ||
            sourceIndex == destinationIndex)
        {
            return false;
        }
        packs.move(sourceIndex, destinationIndex);
        emit PacksChanged();
        emit CatalogChanged();
        return true;
    }

    std::expected<std::vector<std::string>, QString> PackController::validatedPacks(
        qint32 type, const QString& target) const
    {
        auto paths = std::vector<std::string>{};
        paths.reserve(static_cast<std::size_t>(packs.size()));
        for (const auto& path : packs)
        {
            if (!path.isValidUtf16() || path.contains(QChar(0)))
            {
                return std::unexpected(tr("Некорректный путь к паку."));
            }
            const auto warning = PackTypeWarning(path, type, target);
            if (!warning.isEmpty())
            {
                return std::unexpected(
                    QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), warning));
            }
            paths.push_back(path.toStdString());
        }
        return paths;
    }

    bool PackController::Start(qint32 type, const QString& target, const QString& output,
                               qint32 outputFormat, const QString& description)
    {
        if (busy || scanning)
        {
            return false;
        }
        if (packs.isEmpty())
        {
            return reject(tr("Добавьте хотя бы один пак в стек."));
        }
        const auto format = parseFormat(target);
        if (!format)
        {
            return reject(
                tr("Целевой pack format должен иметь вид MAJOR или MAJOR.MINOR: компоненты int32, "
                   "MAJOR >= 4."));
        }
        if ((type != 0 && type != 1) || (outputFormat != 0 && outputFormat != 1))
        {
            return reject(tr("Выберите корректные тип пака и формат результата."));
        }
        if (output.isEmpty())
        {
            return reject(tr("Выберите новый выходной путь."));
        }
        if (!description.isValidUtf16())
        {
            return reject(diagnosticText({
                .Code = ErrorCode::INVALID_METADATA,
                .Message = "Invalid output metadata",
                .Pack = {},
                .Path = "pack.mcmeta",
            }));
        }
        if (!output.isValidUtf16() || output.contains(QChar(0)))
        {
            return reject(tr("Некорректное имя результата."));
        }
        auto corePacks = validatedPacks(type, target);
        if (!corePacks)
        {
            return reject(corePacks.error());
        }
        const auto request = ResolveRequest{
            .PacksHighToLow = std::move(*corePacks),
            .Type = type == 0 ? PackType::RESOURCE : PackType::DATA,
            .Target = *format,
            .Limits = {},
            .Rules = rules,
        };
        const auto exportRequest = ExportRequest{
            .Path = QFileInfo(output).absoluteFilePath().toStdString(),
            .Format = outputFormat == 0 ? OutputFormat::ZIP : OutputFormat::DIRECTORY,
            .Description = description.toStdString(),
            .Limits = {},
        };
        stop = std::stop_source{};
        const auto token = stop.get_token();
        const auto job = std::make_shared<JobResult>();
        worker.reset(QThread::create(
            [this, request, exportRequest, token, job]
            {
                auto resolved = ResolveStack(request, token);
                if (!resolved)
                {
                    job->Result = std::unexpected(resolved.error());
                    return;
                }
                job->FileCount = resolved->Files.size();
                QMetaObject::invokeMethod(
                    this,
                    [this]
                    {
                        if (!Cancelling())
                        {
                            status = tr("Экспорт…");
                            emit StateChanged();
                        }
                    },
                    Qt::QueuedConnection);
                job->Result = ExportPack(*resolved, exportRequest, token);
            }));
        connect(
            worker.get(), &QThread::finished, this,
            [this, job, output = QString::fromStdString(exportRequest.Path)]
            {
                worker->wait();
                worker.reset();
                busy = false;
                failed = !job->Result && job->Result.error().Code != ErrorCode::CANCELLED;
                if (job->Result)
                {
                    resultPath = output;
                    status = tr("Экспортировано файлов: %1 и pack.mcmeta. Результат: %2")
                                 .arg(job->FileCount)
                                 .arg(output);
                }
                else if (job->Result.error().Code == ErrorCode::CANCELLED)
                {
                    status = tr("Операция отменена. Результат не опубликован.");
                }
                else
                {
                    status = diagnosticText(job->Result.error());
                }
                emit StateChanged();
                emit Finished(job->Result.has_value());
            },
            Qt::QueuedConnection);
        busy = true;
        resultPath.clear();
        failed = false;
        status = tr("Разрешение стека…");
        emit StateChanged();
        worker->start();
        return true;
    }

    QVariantList PackController::Rules() const
    {
        return ruleRows(rules);
    }

    bool PackController::ApplyRules(const QVariantList& values)
    {
        if (busy || scanning)
        {
            return false;
        }
        auto parsed = coreRules(values);
        if (!parsed)
        {
            return reject(diagnosticText(parsed.error()));
        }
        rules = std::move(*parsed);
        failed = false;
        status = tr("Правила применены.");
        emit RulesChanged();
        emit StateChanged();
        return true;
    }

    QVariantMap PackController::ImportRules(const QUrl& url)
    {
        if (busy || scanning)
        {
            return {{"ok", false}};
        }
        const auto parsed = LoadRules(LocalPath(url).toStdString());
        if (!parsed)
        {
            reject(diagnosticText(parsed.error()));
            return {{"ok", false}, {"error", status}};
        }
        return {{"ok", true}, {"rules", ruleRows(*parsed)}};
    }

    bool PackController::ExportRules(const QUrl& url, const QVariantList& values)
    {
        if (busy || scanning)
        {
            return false;
        }
        const auto parsed = coreRules(values);
        if (!parsed)
        {
            return reject(diagnosticText(parsed.error()));
        }
        const auto saved = SaveRules(LocalPath(url).toStdString(), *parsed);
        if (!saved)
        {
            return reject(diagnosticText(saved.error()));
        }
        return true;
    }

    void PackController::Cancel()
    {
        const auto exportCancelled = busy && stop.request_stop();
        const auto scanCancelled = scanning && scanStop.request_stop();
        if (exportCancelled || scanCancelled)
        {
            status = tr("Запрошена отмена…");
            emit StateChanged();
        }
    }
}
