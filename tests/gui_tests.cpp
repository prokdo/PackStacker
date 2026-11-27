#include "core/model.hpp"
#include "gui/localization.hpp"
#include "gui/pack_catalog.hpp"
#include "gui/pack_controller.hpp"

#include <QBuffer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QLatin1StringView>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTimer>
#include <QtClassHelperMacros>
#include <qcolor.h>
#include <qcoreapplication.h>
#include <qfiledevice.h>
#include <qhashfunctions.h>
#include <qlist.h>
#include <qnamespace.h>
#include <qnumeric.h>
#include <qobject.h>
#include <qobjectdefs.h>
#include <qqml.h>
#include <qqmlengine.h>
#include <qrgb.h>
#include <qstringfwd.h>
#include <qtenvironmentvariables.h>
#include <qtestmouse.h>
#include <qtypes.h>
#include <qurl.h>

#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using PackStacker::CatalogPathKey;
    using PackStacker::CatalogPlaceholder;
    using PackStacker::ErrorCode;
    using PackStacker::InstallInterfaceTranslation;
    using PackStacker::InterfaceLanguage;
    using PackStacker::Limits;
    using PackStacker::PackController;
    using PackStacker::ReadCatalog;
    using PackStacker::Result;
    using Json = nlohmann::json;
    using Files = std::vector<std::pair<QString, QByteArray>>;

    void require(bool condition, const char* message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    template <typename T>
    T take(Result<T> result)
    {
        if (!result)
        {
            throw std::runtime_error(result.error().Message);
        }
        return std::move(*result);
    }

    template <typename T>
    void expectError(const Result<T>& result, ErrorCode code)
    {
        require(!result, "Expected an error");
        require(result.error().Code == code, "Unexpected error code");
    }

    QByteArray encode(const Json& value)
    {
        return QByteArray::fromStdString(value.dump());
    }

    QList<QQuickItem*> visualItems(QQuickItem* root)
    {
        auto items = QList<QQuickItem*>{root};
        for (auto index = qsizetype{0}; index < items.size(); ++index)
        {
            items.append(items.at(index)->childItems());
        }
        return items;
    }

    void write(const QString& path, const QByteArray& bytes)
    {
        require(QDir().mkpath(QFileInfo(path).absolutePath()), "Cannot create fixture directory");
        auto file = QFile(path);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Cannot open fixture file");
        require(file.write(bytes) == bytes.size(), "Cannot write fixture file");
    }

    QByteArray read(const QString& path)
    {
        auto file = QFile(path);
        require(file.open(QIODevice::ReadOnly), "Cannot read fixture file");
        const auto bytes = file.readAll();
        require(file.error() == QFileDevice::NoError, "Cannot read fixture bytes");
        return bytes;
    }

    bool waitFor(const PackController& controller)
    {
        require(controller.Busy(), "Expected a running operation");
        auto loop = QEventLoop{};
        auto timeout = QTimer{};
        timeout.setSingleShot(true);
        auto completed = false;
        auto succeeded = false;
        QObject::connect(&controller, &PackController::Finished, &loop,
                         [&](bool success)
                         {
                             completed = true;
                             succeeded = success;
                             loop.quit();
                         });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(15000);
        loop.exec();
        require(completed, "Controller timed out");
        require(!controller.Busy() && !controller.Cancelling(),
                "Controller did not return to idle");
        return succeeded;
    }

    bool waitForCatalog(const PackController& controller)
    {
        require(controller.Scanning(), "Expected a running catalog scan");
        auto loop = QEventLoop{};
        auto timeout = QTimer{};
        timeout.setSingleShot(true);
        auto completed = false;
        auto succeeded = false;
        QObject::connect(&controller, &PackController::CatalogFinished, &loop,
                         [&](bool success)
                         {
                             completed = true;
                             succeeded = success;
                             loop.quit();
                         });
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
        timeout.start(15000);
        loop.exec();
        require(completed && !controller.Scanning(), "Catalog scan timed out");
        return succeeded;
    }

    struct Fixture
    {
        QTemporaryDir Temporary;
        PackController Controller;

        Fixture()
        {
            require(Temporary.isValid(), "Cannot create temporary directory");
        }
        QString Path(const QString& name) const
        {
            return Temporary.filePath(name);
        }
        QString Pack(const QString& name, const Files& files = {}, qint32 format = 34) const
        {
            const auto root = Path(name);
            write(root + "/pack.mcmeta",
                  QByteArray::fromStdString(
                      Json{{"pack", {{"pack_format", format}, {"description", "Test"}}}}.dump()));
            for (const auto& [relative, bytes] : files)
            {
                write(root + '/' + relative, bytes);
            }
            return root;
        }
        void Add(const QString& path)
        {
            require(Controller.AddPack(QUrl::fromLocalFile(path)), "Cannot add input pack");
        }
        bool Start(const QString& output = "result", qint32 outputFormat = 1)
        {
            return Controller.Start(0, "34", Path(output), outputFormat, "Combined");
        }
        void Clean() const
        {
            require(
                QDir(Temporary.path())
                    .entryList({".packstacker-*"}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)
                    .isEmpty(),
                "Staging leaked");
        }
    };

    struct Interface
    {
        bool Warnings = false;
        QQmlApplicationEngine Engine;
        QObject* Root = nullptr;

        explicit Interface(PackController& controller)
        {
            QObject::connect(&Engine, &QQmlEngine::warnings, &Engine, [this] { Warnings = true; });
            Engine.setInitialProperties({{"controller", QVariant::fromValue(&controller)}});
            Engine.load(QUrl::fromLocalFile(QStringLiteral(PACKSTACKER_QML_SOURCE)));
            require(!Engine.rootObjects().isEmpty(), "QML did not load");
            Root = Engine.rootObjects().front();
            QCoreApplication::processEvents();
            if (controller.Scanning())
            {
                require(waitForCatalog(controller), "Initial catalog scan failed");
            }
            require(!Warnings, "QML load produced warnings");
        }
        QObject* Item(const char* name) const
        {
            auto* object = Root->findChild<QObject*>(QString::fromUtf8(name));
            if (object == nullptr)
            {
                const auto* window = qobject_cast<QQuickWindow*>(Root);
                for (auto* item : visualItems(window->contentItem()))
                {
                    if (item->objectName() == QString::fromUtf8(name))
                    {
                        object = item;
                        break;
                    }
                }
            }
            if (object == nullptr)
            {
                throw std::runtime_error(std::string("QML item not found: ") + name);
            }
            return object;
        }
        void Set(const char* name, const char* property, const QVariant& value) const
        {
            require(Item(name)->setProperty(property, value), "Cannot set QML property");
            if (QString::fromLatin1(property) == "text" &&
                (QString::fromLatin1(name) == "outputPath" ||
                 QString::fromLatin1(name) == "description"))
            {
                require(QMetaObject::invokeMethod(Item(name), "textEdited"),
                        "Cannot simulate editing a default field");
            }
        }
        void Click(const char* name) const
        {
            require(Item(name)->property("enabled").toBool(), "QML button is disabled");
            require(QMetaObject::invokeMethod(Item(name), "clicked"), "Cannot activate QML button");
        }
    };

    struct SystemAppearance
    {
        QPalette OriginalPalette = QGuiApplication::palette();

        SystemAppearance() = default;

        ~SystemAppearance()
        {
            QGuiApplication::styleHints()->unsetColorScheme();
            QGuiApplication::setPalette(OriginalPalette);
        }

        void SetDark(bool dark) const
        {
            QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark
                                                               : Qt::ColorScheme::Light);
            QCoreApplication::processEvents();
            const auto scheme = QGuiApplication::styleHints()->colorScheme();
            if (scheme != (dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light))
            {
                require(QGuiApplication::platformName() == "offscreen" &&
                            scheme == Qt::ColorScheme::Unknown,
                        "Platform did not apply the requested appearance");
                auto palette = OriginalPalette;
                palette.setColor(QPalette::Window, dark ? QColor("#181818") : QColor("#f4f6f8"));
                QGuiApplication::setPalette(palette);
                QCoreApplication::processEvents();
            }
        }

    private:
        Q_DISABLE_COPY_MOVE(SystemAppearance)
    };

    constexpr auto LANGUAGE = QLatin1StringView("assets/example/lang/en_us.json");
    constexpr auto OPAQUE = QLatin1StringView("assets/custom/path/file.bin");

    void testInterfaceLanguageFollowsSupportedUIPreferencesWithEnglishFallback()
    {
        require(InterfaceLanguage({"ru-RU"}) == "ru", "Russian region not recognized");
        require(InterfaceLanguage({"ru_UA"}) == "ru", "Russian locale not recognized");
        require(InterfaceLanguage({"en-GB", "ru-RU"}) == "en", "Preference order ignored");
        require(InterfaceLanguage({"de-DE", "ru-RU", "en-US"}) == "ru",
                "Supported fallback ignored");
        require(InterfaceLanguage({"de-DE"}) == "en" && InterfaceLanguage({}) == "en" &&
                    InterfaceLanguage({"C"}) == "en",
                "English fallback not selected");
    }

    void testEmbeddedTranslationsLocalizeQMLControllerAndBackgroundCatalogInBothLanguages()
    {
        for (const auto english : {true, false})
        {
            auto translator = QTranslator{};
            require(InstallInterfaceTranslation(translator, {english ? "en-US" : "ru-RU"}),
                    "Cannot install embedded translation");
            auto fixture = Fixture{};
            const auto interface = Interface(fixture.Controller);
            require(interface.Root->property("title").toString() == "PackStacker",
                    "Unexpected window title");
            interface.Click("aboutButton");
            require(interface.Item("aboutDialog")->property("visible").toBool() &&
                        interface.Item("aboutVersion")->property("text").toString() ==
                            QStringLiteral("PackStacker 1.2.3"),
                    "About dialog did not display the application version");
            interface.Set("aboutDialog", "visible", false);
            require(interface.Item("startExport")->property("text").toString() ==
                        (english ? QStringLiteral("Merge") : QStringLiteral("Объединить")),
                    "QML action not translated");
            require(
                interface.Item("outputPath")->property("placeholderText").toString() ==
                        (english
                             ? QStringLiteral("Default: left-side folder / combined-pack.zip")
                             : QStringLiteral("По умолчанию: каталог слева / combined-pack.zip")) &&
                    interface.Item("description")->property("placeholderText").toString() ==
                        (english ? QStringLiteral("Default: description of the top pack")
                                 : QStringLiteral("По умолчанию: описание верхнего пака")),
                "Default field placeholders are missing or not translated");
            interface.Set("outputFormat", "currentIndex", 1);
            require(!interface.Item("outputPath")
                            ->property("placeholderText")
                            .toString()
                            .contains(".zip") &&
                        !interface.Item("outputPath")
                             ->property("toolTip")
                             .toString()
                             .contains(".zip") &&
                        !interface.Item("description")->property("toolTip").toString().isEmpty(),
                    "Folder default hints still describe ZIP output or description hint is "
                    "missing");
            interface.Set("outputFormat", "currentIndex", 0);
            require(!fixture.Start() &&
                        fixture.Controller.Status() ==
                            (english ? QStringLiteral("Add at least one pack to the stack.")
                                     : QStringLiteral("Добавьте хотя бы один пак в стек.")),
                    "Controller validation not translated");
            const auto input = fixture.Pack("library/localized");
            require(fixture.Controller.SetSourceDirectory(
                        QUrl::fromLocalFile(fixture.Path("library"))) &&
                        waitForCatalog(fixture.Controller),
                    "Localized background scan failed");
            require(!interface.Item("outputPath")->property("text").toString().isEmpty() &&
                        !interface.Item("outputPath")->property("toolTip").toString().isEmpty() &&
                        interface.Item("outputPath")->property("hoverEnabled").toBool(),
                    "Default explanation is unavailable when the output path is populated");
            require(fixture.Controller.AvailablePacks().at(0).toMap().value("storage").toString() ==
                        (english ? QStringLiteral("FOLDER") : QStringLiteral("ПАПКА")),
                    "Background catalog not translated");
            require(CatalogPlaceholder(input).Description ==
                        (english ? QStringLiteral("Metadata not read yet")
                                 : QStringLiteral("Метаданные ещё не прочитаны")),
                    "Catalog placeholder not translated");
            require(!interface.Warnings, "Localized QML produced warnings");
        }
    }

    void testStackAdditionsUnicodeDuplicateAndRemoteURLRejection()
    {
        auto fixture = Fixture{};
        const auto first = fixture.Pack(QStringLiteral("Пак 世界"));
        const auto second = fixture.Pack("second");
        fixture.Add(first);
        fixture.Add(second);
        require(fixture.Controller.Packs() == QStringList{first, second}, "Stack order changed");
        require(!fixture.Controller.AddPack(
                    QUrl::fromLocalFile(first + "/../" + QFileInfo(first).fileName())),
                "Duplicate accepted");
        require(!fixture.Controller.AddPack(QUrl("https://example.com/pack.zip")),
                "Remote input accepted");
        require(PackStacker::PackController::LocalPath(QUrl::fromLocalFile(first)) == first,
                "Unicode URL changed");
        require(fixture.Controller.Packs().size() == 2, "Invalid input changed stack");
    }

    void testBatchAdditionsPreserveURLOrderAndNotifyOnce()
    {
        auto fixture = Fixture{};
        const auto existing = fixture.Path("existing");
        fixture.Add(existing);
        auto notifications = qint32{0};
        QObject::connect(&fixture.Controller, &PackController::PacksChanged, &fixture.Controller,
                         [&] { ++notifications; });
        const auto first = fixture.Path(QStringLiteral("Новый 世界 # 100%.zip"));
        const auto second = fixture.Path("second");
        require(
            fixture.Controller.AddPacks({QUrl::fromLocalFile(first), QUrl::fromLocalFile(second)}),
            "Valid batch rejected");
        require(fixture.Controller.Packs() == QStringList{existing, first, second} &&
                    notifications == 1,
                "Batch changed URL order or notified partially");
        require(!fixture.Controller.Failed(), "Successful batch retained an error");
    }

    void drag(const Interface& interface, const char* source, const char* destination)
    {
        auto* window = qobject_cast<QQuickWindow*>(interface.Root);
        require(!window->grabWindow().isNull(), "Cannot render drag targets");
        const auto center = [&interface](const char* name)
        {
            const auto* item = qobject_cast<QQuickItem*>(interface.Item(name));
            return item->mapToScene(item->boundingRect().center());
        };
        const auto start = center(source);
        const auto end = center(destination);
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, start.toPoint());
        for (auto step = 1; step <= 5; ++step)
        {
            QTest::mouseMove(window, (start + (end - start) * (step / 5.0)).toPoint());
        }
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, end.toPoint());
        QCoreApplication::processEvents();
    }

    void testDraggingMovesPacksBetweenPanelsAndReordersStack()
    {
        auto fixture = Fixture{};
        const auto first = fixture.Pack("first");
        const auto second = fixture.Pack("second");
        const auto interface = Interface(fixture.Controller);
        require(fixture.Controller.SetSourceDirectory(
                    QUrl::fromLocalFile(QFileInfo(first).absolutePath())) &&
                    waitForCatalog(fixture.Controller),
                "Cannot scan drag fixtures");
        drag(interface, "availablePack0", "packStack");
        require(fixture.Controller.Packs() == QStringList{first}, "Drag into empty stack failed");
        drag(interface, "availablePack0", "stackPack0");
        require(fixture.Controller.Packs() == QStringList{second, first},
                "Drag at stack row did not insert");
        drag(interface, "stackPack0", "stackPack1");
        require(fixture.Controller.Packs() == QStringList{first, second},
                "Drag did not reorder stack");
        drag(interface, "stackPack0", "availablePacks");
        require(fixture.Controller.Packs() == QStringList{second}, "Drag back to catalog failed");
        drag(interface, "stackPack0", "stackTitle");
        require(fixture.Controller.Packs() == QStringList{second},
                "Drop outside list changed stack");
        require(QFileInfo::exists(first) && QFileInfo::exists(second) && !interface.Warnings,
                "Dragging modified a source or produced QML warnings");
    }

    void testWrongPackTypeWarnsAndBlocksExport()
    {
        auto fixture = Fixture{};
        const auto data =
            fixture.Pack("data-pack", {{"data/example/function/load.mcfunction", ""}});
        fixture.Add(data);
        const auto interface = Interface(fixture.Controller);
        interface.Set("outputPath", "text", fixture.Path("data.zip"));
        const auto* row = interface.Item("stackPack0");
        const auto warning = row->property("typeWarning").toString();
        require(!warning.isEmpty() && !interface.Item("startExport")->property("enabled").toBool(),
                "Wrong pack type did not warn or block the GUI");
        require(!fixture.Start() && !fixture.Controller.Busy() &&
                    fixture.Controller.Status().contains(warning),
                "Controller started an incompatible merge");
        interface.Set("packType", "currentIndex", 1);
        require(row->property("typeWarning").toString().isEmpty() &&
                    interface.Item("startExport")->property("enabled").toBool(),
                "Selecting the matching type did not clear the warning");
        require(fixture.Controller.Start(1, "34", fixture.Path("data.zip"), 0, "Combined") &&
                    waitFor(fixture.Controller),
                "Correct data-pack mode did not export");
        const auto zip = fixture.Path("data.zip");
        require(fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("."))) &&
                    waitForCatalog(fixture.Controller) &&
                    !fixture.Controller.PackTypeWarning(zip, 0, "34").isEmpty() &&
                    fixture.Controller.PackTypeWarning(zip, 1, "34").isEmpty() &&
                    !interface.Warnings,
                "ZIP data pack was not detected correctly");
    }

    void testRefreshRechecksSelectedPackTypesAtTheSamePath()
    {
        auto fixture = Fixture{};
        const auto input = fixture.Pack("changing", {{"data/example/item.bin", "content"}});
        fixture.Add(input);
        fixture.Add(fixture.Pack("other"));
        const auto interface = Interface(fixture.Controller);
        interface.Set("outputPath", "text", fixture.Path("refreshed.zip"));
        const auto order = fixture.Controller.Packs();
        require(!interface.Item("stackPack0")->property("typeWarning").toString().isEmpty() &&
                    !interface.Item("startExport")->property("enabled").toBool(),
                "Initial data pack was not blocked in resource mode");
        require(QDir(input).rename("data", "assets"), "Cannot replace the pack contents");
        interface.Click("refreshCatalog");
        require(waitForCatalog(fixture.Controller) && fixture.Controller.Packs() == order,
                "Refreshing the catalog changed the selected stack");
        require(interface.Item("stackPack0")->property("typeWarning").toString().isEmpty() &&
                    interface.Item("startExport")->property("enabled").toBool() &&
                    !interface.Warnings,
                "Refreshing did not clear the cached type warning");
        require(fixture.Start("refreshed.zip", 0) && waitFor(fixture.Controller),
                "The refreshed pack could not be exported");
    }

    void testTypeDetectionRespectsActiveOverlaysAndAmbiguousContent()
    {
        auto fixture = Fixture{};
        const auto resource =
            fixture.Pack("overlay-pack", {
                                             {"assets/example/item.bin", "asset"},
                                             {"new/data/example/item.bin", "data"},
                                         });
        write(
            resource + "/pack.mcmeta",
            R"({"pack":{"pack_format":34,"supported_formats":[34,35],"description":"Test"},"overlays":{"entries":[{"directory":"new","formats":35}]}})");
        const auto empty = fixture.Pack("metadata-only");
        require(fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("."))) &&
                    waitForCatalog(fixture.Controller),
                "Cannot read the overlay catalog");
        const auto& controller = fixture.Controller;
        require(controller.PackTypeWarning(resource, 0, "34").isEmpty() &&
                    !controller.PackTypeWarning(resource, 1, "34").isEmpty(),
                "Inactive overlay changed the detected pack type");
        const auto mixed = controller.PackTypeWarning(resource, 0, "35");
        require(!mixed.isEmpty() && mixed == controller.PackTypeWarning(resource, 1, "35"),
                "Mixed pack was allowed in one of the modes");
        require(controller.PackTypeWarning(empty, 0, "34").isEmpty() &&
                    controller.PackTypeWarning(empty, 1, "34").isEmpty(),
                "Metadata-only pack was assigned an arbitrary type");
    }

    void testBatchDuplicatesRejectTheWholeGroupWithoutPartialAdditions()
    {
        auto fixture = Fixture{};
        const auto existing = fixture.Path("existing");
        const auto fresh = fixture.Path("fresh");
        fixture.Add(existing);
        auto notifications = qint32{0};
        QObject::connect(&fixture.Controller, &PackController::PacksChanged, &fixture.Controller,
                         [&] { ++notifications; });
        require(!fixture.Controller.AddPacks(
                    {QUrl::fromLocalFile(fresh), QUrl::fromLocalFile(existing)}),
                "Existing input duplicate accepted");
        require(!fixture.Controller.AddPacks({
                    QUrl::fromLocalFile(fresh),
                    QUrl::fromLocalFile(fixture.Path("nested/../fresh")),
                }),
                "Normalized in-batch duplicate accepted");
        require(fixture.Controller.Packs() == QStringList{existing} && notifications == 0,
                "Rejected batch partially changed the stack");
#ifdef Q_OS_WIN
        require(!fixture.Controller.AddPacks(
                    {QUrl::fromLocalFile(fresh), QUrl::fromLocalFile(fresh.toUpper())}),
                "Case-insensitive duplicate accepted on Windows");
#endif
        require(fixture.Controller.AddPacks({QUrl::fromLocalFile(fresh)}) &&
                    !fixture.Controller.Failed(),
                "Valid retry failed or retained stale error state");
    }

    void testBatchURLValidationIsTransactionalAndPreservesEncodedFilenameCharacters()
    {
        auto fixture = Fixture{};
        const auto fresh = QUrl::fromLocalFile(fixture.Path("fresh"));
        auto query = fresh;
        query.setQuery("download=1");
        auto fragment = fresh;
        fragment.setFragment("part");
        const auto nul = QUrl::fromLocalFile(fixture.Path("bad") + QChar(0) + "name");
        for (const auto& invalid : {
                 QUrl{},
                 QUrl("https://example.com/pack.zip"),
                 QUrl("file:"),
                 query,
                 fragment,
                 nul,
             })
        {
            require(PackStacker::PackController::LocalPath(invalid).isEmpty(),
                    "Invalid URL converted to a path");
            require(!fixture.Controller.AddPacks({fresh, invalid}) &&
                        fixture.Controller.Packs().isEmpty(),
                    "Invalid URL batch partially committed");
        }
        require(!fixture.Controller.AddPacks({}), "Empty batch accepted");
        const auto literal = fixture.Path(QStringLiteral("Пак # with %.zip"));
        require(PackStacker::PackController::LocalPath(QUrl::fromLocalFile(literal)) == literal,
                "Escaped URL filename was mistaken for URL metadata");
    }

    void testBatchAdditionsEnforceTheTotalPackLimitBeforeCommitting()
    {
        auto fixture = Fixture{};
        auto urls = QList<QUrl>{};
        for (auto index = quint64{0}; index < Limits{}.MaxPacks; ++index)
        {
            urls.append(QUrl::fromLocalFile(fixture.Path("pack-" + QString::number(index))));
        }
        const auto extra = QUrl::fromLocalFile(fixture.Path("extra"));
        auto oversized = urls;
        oversized.append(extra);
        require(!fixture.Controller.AddPacks(oversized) && fixture.Controller.Packs().isEmpty(),
                "Oversized group partially committed");
        require(fixture.Controller.AddPacks(urls), "Exact pack limit rejected");
        const auto before = fixture.Controller.Packs();
        require(!fixture.Controller.AddPacks({extra}) && fixture.Controller.Packs() == before,
                "Total pack limit exceeded");
    }

    void testQMLCatalogTransfersPreserveOrderAndSelection()
    {
        auto fixture = Fixture{};
        const auto first = fixture.Pack("library/first");
        const auto second = fixture.Pack("library/second");
        const auto interface = Interface(fixture.Controller);
        require(
            fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("library"))) &&
                waitForCatalog(fixture.Controller),
            "Cannot load catalog");
        interface.Set("availablePacks", "currentIndex", 0);
        interface.Click("addAvailable");
        interface.Set("availablePacks", "currentIndex", 0);
        interface.Click("addAvailable");
        require(fixture.Controller.Packs() == QStringList{first, second} &&
                    interface.Item("packStack")->property("currentIndex").toInt() == 1 &&
                    interface.Item("availablePacks")->property("count").toInt() == 0 &&
                    !interface.Warnings,
                "Catalog transfers did not preserve order and selection");
    }

    void testCatalogScanSeparatesAvailableAndSelectedPacksAndSupportsTransfers()
    {
        auto fixture = Fixture{};
        const auto first = fixture.Pack("library/a");
        const auto second = fixture.Pack("library/b");
        write(fixture.Path("library/broken.zip"), "invalid zip");
        require(QDir().mkpath(fixture.Path("library/not-a-pack")), "Cannot create non-pack folder");
        fixture.Add(first);
        require(fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("library"))),
                "Cannot start catalog scan");
        require(
            fixture.Controller.Scanning() && !fixture.Start() && !fixture.Controller.RemovePack(0),
            "Scanning did not lock conflicting actions");
        require(waitForCatalog(fixture.Controller), "Catalog scan failed");
        const auto available = fixture.Controller.AvailablePacks();
        require(available.size() == 2 && available.at(0).toMap().value("path") == second,
                "Selected pack or non-pack folder leaked into available list");
        require(!available.at(1).toMap().value("error").toString().isEmpty(),
                "Invalid ZIP error lost");
        require(fixture.Controller.SelectedPacks().at(0).toMap().value("format") == "34" &&
                    fixture.Controller.SelectedPacks().at(0).toMap().value("description") == "Test",
                "Preview metadata missing");
        require(fixture.Controller.AddAvailable(0) &&
                    fixture.Controller.Packs() == QStringList{first, second},
                "Available pack was not appended at low priority");
        require(!fixture.Controller.AddAvailable(0), "Invalid catalog pack accepted");
        require(fixture.Controller.RemovePack(0) && fixture.Controller.AvailablePacks().size() == 2,
                "Removal did not return pack to available list");
        require(fixture.Controller.ClearPacks() && fixture.Controller.Packs().isEmpty() &&
                    fixture.Controller.AvailablePacks().size() == 3,
                "Clearing stack lost catalog entries");
    }

    void testCatalogIconDecodingIsBoundedAndTextDescriptionsRemainPlain()
    {
        const auto fixture = Fixture{};
        const auto input = fixture.Pack("input");
        write(input + "/pack.mcmeta",
              encode(Json{
                  {
                      "pack",
                      {
                          {"pack_format", 34},
                          {
                              "description",
                              {
                                  {"text", "<b>Plain</b>"},
                                  {"extra", Json::array({{{"text", " §aGreen"}}})},
                              },
                          },
                      },
                  },
              }));
        auto image = QImage(16, 16, QImage::Format_ARGB32);
        image.fill(Qt::green);
        auto png = QByteArray{};
        auto buffer = QBuffer(&png);
        require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
                "Cannot create PNG fixture");
        write(input + "/pack.png", png);
        auto snapshot = take(ReadCatalog({}, {input}, {}));
        auto row = snapshot.Details.value(CatalogPathKey(input));
        require(row.Description == "<b>Plain</b> Green" &&
                    row.Icon.startsWith("data:image/png;base64,"),
                "Icon or plain text description missing");
        write(input + "/pack.png", "not png");
        snapshot = take(ReadCatalog({}, {input}, {}));
        row = snapshot.Details.value(CatalogPathKey(input));
        require(row.Icon.isEmpty() && !row.Warning.isEmpty() && row.Error.isEmpty(),
                "Malformed optional icon invalidated pack or hid warning");
        auto component = Json{{"text", "Tail"}};
        for (auto index = qint32{0}; index < 25; ++index)
        {
            component = Json{{"text", "x"}, {"extra", Json::array({component})}};
        }
        write(input + "/pack.mcmeta",
              encode(Json{{"pack", {{"pack_format", 34}, {"description", component}}}}));
        snapshot = take(ReadCatalog({}, {input}, {}));
        require(snapshot.Details.value(CatalogPathKey(input)).OutputDescription ==
                    QString(25, QChar('x')) + "Tail",
                "Automatic description silently truncated a valid nested text component");
        write(
            input + "/pack.mcmeta",
            R"({"pack":{"pack_format":34,"description":["A",{"translate":"key","extra":["B",{"text":"","translate":"fallback","extra":[{"text":"C"}]}]},"D",false,17]}})");
        snapshot = take(ReadCatalog({}, {input}, {}));
        require(
            snapshot.Details.value(CatalogPathKey(input)).OutputDescription == "AkeyBfallbackCD",
            "Description traversal changed component order or translation fallback");
    }

    void testFailedOrCancelledCatalogChangesPreserveThePreviousDirectoryAndStack()
    {
        auto fixture = Fixture{};
        fixture.Add(fixture.Pack("library/one"));
        require(
            fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("library"))) &&
                waitForCatalog(fixture.Controller),
            "Cannot load initial catalog");
        const auto source = fixture.Controller.SourceDirectory();
        const auto selected = fixture.Controller.Packs();
        require(
            fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("missing"))) &&
                !waitForCatalog(fixture.Controller),
            "Missing directory accepted");
        require(fixture.Controller.SourceDirectory() == source &&
                    fixture.Controller.Packs() == selected,
                "Failed scan changed current configuration");
        const auto connection = QObject::connect(&fixture.Controller, &PackController::StateChanged,
                                                 &fixture.Controller,
                                                 [&]
                                                 {
                                                     if (fixture.Controller.Scanning())
                                                     {
                                                         fixture.Controller.Cancel();
                                                     }
                                                 });
        require(fixture.Controller.RefreshCatalog() && !waitForCatalog(fixture.Controller),
                "Cancellation failed");
        QObject::disconnect(connection);
        require(!fixture.Controller.Failed() && fixture.Controller.SourceDirectory() == source &&
                    fixture.Controller.Packs() == selected,
                "Cancelled scan changed state or reported failure");
    }

    void testQMLTransfersPreserveBothPanelsAndAppearanceFollowsSystemHintsWithoutAThemeButton()
    {
        auto fixture = Fixture{};
        const auto input = fixture.Pack("library/one");
        const auto interface = Interface(fixture.Controller);
        require(interface.Root->property("width").toInt() == 1280 &&
                    interface.Root->property("height").toInt() == 720,
                "Approved window dimensions changed");
        require(
            fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("library"))) &&
                waitForCatalog(fixture.Controller),
            "Cannot load catalog for QML");
        interface.Set("availablePacks", "currentIndex", 0);
        interface.Click("addAvailable");
        require(fixture.Controller.Packs() == QStringList{input} &&
                    interface.Item("packStack")->property("count").toInt() == 1 &&
                    interface.Item("availablePacks")->property("count").toInt() == 0,
                "QML transfer did not update both panels");
        interface.Click("removePack");
        require(fixture.Controller.Packs().isEmpty() &&
                    interface.Item("availablePacks")->property("count").toInt() == 1,
                "QML removal did not restore library");
        require(interface.Root->findChild<QObject*>("toggleTheme") == nullptr,
                "Manual theme button is still present");
        const auto* colors = interface.Root->property("designTheme").value<QObject*>();
        require(colors != nullptr, "Theme object not exposed");
        const auto appearance = SystemAppearance{};
        for (const auto dark : {false, true, false})
        {
            appearance.SetDark(dark);
            require(colors->property("dark").toBool() == dark &&
                        interface.Root->property("color") == colors->property("background"),
                    "UI did not follow a color scheme change");
        }
        require(!interface.Warnings, "Two-panel controls or automatic theme produced QML warnings");
    }

    void testPopulatedQMLSupportsBothThemesAndOptionalVisualCaptures()
    {
        auto fixture = Fixture{};
        const auto samples = std::vector<std::pair<QString, QString>>{
            {"Clear Water", QStringLiteral("Прозрачная вода и мягкие оттенки")},
            {"Soft Light", QStringLiteral("Новые частицы и эффекты освещения")},
            {"Minimal Font", QStringLiteral("Компактный и читаемый шрифт")},
            {"Quiet UI", QStringLiteral("Чистый интерфейс без лишних деталей")},
            {"Russian Names", QStringLiteral("Уточнённые переводы предметов")},
            {"Stonebound", QStringLiteral("Обновлённые текстуры блоков · 32×")},
        };
        auto paths = QStringList{};
        auto shade = qint32{0};
        for (const auto& [name, description] : samples)
        {
            const auto input = fixture.Pack("library/" + name, {{OPAQUE, QByteArray(2048, 'x')}});
            write(input + "/pack.mcmeta",
                  encode(Json{
                      {
                          "pack",
                          {{"pack_format", 34}, {"description", description.toStdString()}},
                      },
                  }));
            auto image = QImage(16, 16, QImage::Format_ARGB32);
            for (auto pixelY = qint32{0}; pixelY < 16; ++pixelY)
            {
                for (auto pixelX = qint32{0}; pixelX < 16; ++pixelX)
                {
                    const auto value = (((pixelX / 4) + (pixelY / 4)) % 3) * 30;
                    image.setPixel(
                        pixelX, pixelY,
                        qRgb(40 + (shade * 15) + value, 65 + value, 75 + (shade * 12) + value));
                }
            }
            auto png = QByteArray{};
            auto buffer = QBuffer(&png);
            require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
                    "Cannot create preview icon");
            write(input + "/pack.png", png);
            paths.append(input);
            ++shade;
        }
        fixture.Add(paths.at(3));
        fixture.Add(paths.at(4));
        fixture.Add(paths.at(5));
        const auto interface = Interface(fixture.Controller);
        require(
            fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("library"))) &&
                waitForCatalog(fixture.Controller),
            "Cannot prepare populated preview");
        interface.Set("sourcePath", "text", "D:/Minecraft/resourcepacks");
        interface.Set("outputPath", "text", "D:/Minecraft/merged/my-pack.zip");
        interface.Set("targetFormat", "text", "34");
        interface.Set("description", "text", QStringLiteral("Моя сборка ресурсов"));
        const auto* colors = interface.Root->property("designTheme").value<QObject*>();
        require(colors != nullptr, "Theme object not exposed");
        const auto capture = qEnvironmentVariable("PACKSTACKER_CAPTURE_DIR");
        auto* previewWindow = qobject_cast<QQuickWindow*>(interface.Root);
        require(previewWindow != nullptr, "Preview is not a QQuickWindow");
        QCoreApplication::processEvents();
        require(QMetaObject::invokeMethod(interface.Item("availablePacks"), "forceLayout") &&
                    QMetaObject::invokeMethod(interface.Item("packStack"), "forceLayout"),
                "Cannot lay out preview lists");
        require(!previewWindow->grabWindow().isNull(), "Cannot render preview layout");
        QList<QObject*> icons;
        for (auto* item : visualItems(previewWindow->contentItem()))
        {
            if (item->objectName() == "packIcon")
            {
                icons.append(item);
            }
        }
        require(icons.size() == 6, "Not all pack icons were instantiated");
        auto imageLoop = QEventLoop{};
        auto imageTimeout = QTimer{};
        imageTimeout.setSingleShot(true);
        QObject::connect(&imageTimeout, &QTimer::timeout, &imageLoop, &QEventLoop::quit);
        for (const auto* icon : icons)
        {
            QObject::connect(icon, SIGNAL(statusChanged()), &imageLoop, SLOT(quit()));
        }
        imageTimeout.start(10000);
        while (imageTimeout.isActive())
        {
            const auto ready = std::ranges::all_of(
                icons, [](const QObject* icon) { return icon->property("status").toInt() == 1; });
            if (ready)
            {
                break;
            }
            imageLoop.exec();
        }
        require(std::ranges::all_of(icons, [](const QObject* icon)
                                    { return icon->property("status").toInt() == 1; }),
                "Pack icons did not become ready");
        const auto appearance = SystemAppearance{};
        for (const auto dark : {false, true})
        {
            appearance.SetDark(dark);
            require(colors->property("dark").toBool() == dark,
                    "Preview did not follow system color scheme");
            QCoreApplication::processEvents();
            if (!capture.isEmpty())
            {
                require(QDir().mkpath(capture), "Cannot create capture directory");
                auto* window = qobject_cast<QQuickWindow*>(interface.Root);
                require(window != nullptr && window->grabWindow().save(QDir(capture).filePath(
                                                 dark ? "qml-dark.png" : "qml-light.png")),
                        "Cannot capture real QML window");
            }
        }
        require(!interface.Warnings, "Populated QML or theme switching produced warnings");
    }

    void testStackMoveAndRemovalValidateIndices()
    {
        auto fixture = Fixture{};
        const auto first = fixture.Pack("first");
        const auto second = fixture.Pack("second");
        fixture.Add(first);
        fixture.Add(second);
        require(fixture.Controller.MovePack(1, 0), "Cannot raise priority");
        require(fixture.Controller.Packs() == QStringList{second, first}, "Wrong priority order");
        require(!fixture.Controller.MovePack(-1, 0) && !fixture.Controller.MovePack(0, 2) &&
                    !fixture.Controller.RemovePack(-1) && !fixture.Controller.RemovePack(2),
                "Invalid index accepted");
        require(
            fixture.Controller.RemovePack(0) && fixture.Controller.Packs() == QStringList{first},
            "Wrong removal");
    }

    void testInvalidRequestsFailSynchronouslyWithoutBecomingBusy()
    {
        auto fixture = Fixture{};
        require(!fixture.Start(), "Empty stack accepted");
        fixture.Add(fixture.Pack("input"));
        for (const auto& target :
             {"", "3", "-1", "34.", "34.-1", "1.21.5", "2147483648", "65.2147483648"})
        {
            require(!fixture.Controller.Start(0, target, fixture.Path("result"), 0, "test"),
                    "Invalid format accepted");
        }
        require(!fixture.Controller.Start(2, "34", fixture.Path("result"), 0, "test"),
                "Invalid pack type accepted");
        require(!fixture.Controller.Start(0, "34", fixture.Path("result"), 2, "test"),
                "Invalid output type accepted");
        require(!fixture.Controller.Start(0, "34", "", 0, "test"), "Empty output accepted");
        const auto invalid = QString(QChar(0xd800));
        require(!fixture.Controller.Start(0, "34", invalid, 0, "test"),
                "Invalid UTF-16 output was replaced during UTF-8 conversion");
        require(!fixture.Controller.Start(0, "34", fixture.Path("result"), 0, invalid),
                "Invalid UTF-16 description was replaced during UTF-8 conversion");
        require(fixture.Controller.Failed() && !fixture.Controller.Busy(),
                "Validation left wrong state");
        require(!QFileInfo::exists(fixture.Path("result")), "Invalid request created output");
    }

    void testAsyncExportPreservesPriorityTranslationsAndOpaqueBytes()
    {
        auto fixture = Fixture{};
        const auto bytes = QByteArray("\0\xff\r\n", 4);
        fixture.Add(fixture.Pack("high", {{LANGUAGE, R"({"shared":"high"})"}, {OPAQUE, bytes}}));
        fixture.Add(fixture.Pack(
            "low", {{LANGUAGE, R"({"shared":"low","unique":"low"})"}, {OPAQUE, "low"}}));
        require(fixture.Start(), "Cannot start export");
        require(fixture.Controller.Busy() && !fixture.Controller.Failed(),
                "Start did not notify busy");
        require(waitFor(fixture.Controller), "Export failed");
        require(Json::parse(read(fixture.Path("result/" + LANGUAGE)).toStdString()) ==
                    Json{{"shared", "high"}, {"unique", "low"}},
                "Wrong merge");
        require(read(fixture.Path("result/" + OPAQUE)) == bytes, "Opaque bytes changed");
        fixture.Clean();
    }

    void testRunningOperationLocksStackAndRejectsASecondStart()
    {
        auto fixture = Fixture{};
        fixture.Add(fixture.Pack("one"));
        fixture.Add(fixture.Pack("two"));
        const auto before = fixture.Controller.Packs();
        require(fixture.Start(), "Cannot start export");
        require(
            !fixture.Controller.AddPack(QUrl::fromLocalFile(fixture.Path("three"))) &&
                !fixture.Controller.AddPacks({QUrl::fromLocalFile(fixture.Path("four"))}) &&
                !fixture.Controller.MovePack(0, 1) && !fixture.Controller.RemovePack(0) &&
                !fixture.Start("second") && !fixture.Controller.ApplyRules({}) &&
                !fixture.Controller.ExportRules(QUrl::fromLocalFile(fixture.Path("busy.json")), {}),
            "Busy state allowed mutations");
        require(fixture.Controller.Packs() == before, "Running stack changed");
        require(waitFor(fixture.Controller), "First export failed");
        require(!QFileInfo::exists(fixture.Path("second")), "Second operation started");
        require(!QFileInfo::exists(fixture.Path("busy.json")), "Busy state exported rules");
    }

    void testUnicodeZIPOutputIsReusableAsInput()
    {
        auto fixture = Fixture{};
        fixture.Add(fixture.Pack(QStringLiteral("Вход 世界"), {{OPAQUE, "unicode"}}));
        const auto output = QStringLiteral("Выход 世界.zip");
        require(fixture.Start(output, 0) && waitFor(fixture.Controller), "ZIP export failed");
        require(fixture.Controller.RemovePack(0), "Cannot remove source");
        fixture.Add(fixture.Path(output));
        require(fixture.Start("roundtrip") && waitFor(fixture.Controller), "ZIP input failed");
        require(read(fixture.Path("roundtrip/" + OPAQUE)) == "unicode",
                "ZIP round trip changed bytes");
    }

    void testDataPackModeMergesTags()
    {
        auto fixture = Fixture{};
        const auto tag = QString("data/example/tags/function/load.json");
        fixture.Add(fixture.Pack("high", {{tag, R"({"values":["example:high"]})"}}, 48));
        fixture.Add(fixture.Pack("low", {{tag, R"({"values":["example:low"]})"}}, 48));
        require(fixture.Controller.Start(1, "48", fixture.Path("result"), 1, "Data"),
                "Cannot start data export");
        require(waitFor(fixture.Controller), "Data export failed");
        require(Json::parse(read(fixture.Path("result/" + tag)).toStdString()).at("values") ==
                    Json::array({"example:low", "example:high"}),
                "Wrong data merge");
    }

    void testExactMinorFormatAndDescriptionReachOutputMetadata()
    {
        auto fixture = Fixture{};
        const auto input = fixture.Pack("input");
        write(input + "/pack.mcmeta",
              R"({"pack":{"min_format":[65,2],"max_format":[65,2],"description":"Test"}})");
        fixture.Add(input);
        const auto description = QStringLiteral("Описание 世界");
        require(fixture.Controller.Start(0, "65.2", fixture.Path("result"), 1, description),
                "Cannot start minor export");
        require(waitFor(fixture.Controller), "Minor export failed");
        const auto metadata = Json::parse(read(fixture.Path("result/pack.mcmeta")).toStdString());
        require(metadata.at("pack").at("min_format") == Json::array({65, 2}) &&
                    metadata.at("pack").at("description") == description.toStdString(),
                "Metadata changed");
    }

    void testCoreErrorsExposeContextAndAllowRetry()
    {
        auto fixture = Fixture{};
        const auto input = fixture.Pack("invalid");
        write(input + "/pack.mcmeta", "{");
        fixture.Add(input);
        require(fixture.Start() && !waitFor(fixture.Controller), "Invalid metadata accepted");
        require(fixture.Controller.Failed() &&
                    fixture.Controller.Status().contains("pack.mcmeta") &&
                    fixture.Controller.Status().contains("invalid"),
                "Diagnostic lost context");
        fixture.Pack("invalid");
        require(fixture.Start() && waitFor(fixture.Controller), "Retry failed");
        require(!fixture.Controller.Failed(), "Retry retained error state");
    }

    void testExistingOutputAndSourceOverlapRemainProtected()
    {
        auto fixture = Fixture{};
        fixture.Add(fixture.Pack("input", {{OPAQUE, "keep"}}));
        write(fixture.Path("existing.zip"), "existing");
        require(fixture.Start("existing.zip", 0) && !waitFor(fixture.Controller),
                "Existing output overwritten");
        require(read(fixture.Path("existing.zip")) == "existing", "Existing bytes changed");
        require(fixture.Start("input/result") && !waitFor(fixture.Controller),
                "Source overlap allowed");
        require(!QFileInfo::exists(fixture.Path("input/result")) &&
                    read(fixture.Path("input/" + OPAQUE)) == "keep",
                "Source modified");
        fixture.Clean();
    }

    void testCancellationBeforeDispatchCleansUpAndPermitsRestart()
    {
        auto fixture = Fixture{};
        fixture.Add(fixture.Pack("input", {{OPAQUE, "test"}}));
        const auto connection = QObject::connect(&fixture.Controller, &PackController::StateChanged,
                                                 &fixture.Controller,
                                                 [&]
                                                 {
                                                     if (fixture.Controller.Busy())
                                                     {
                                                         fixture.Controller.Cancel();
                                                     }
                                                 });
        require(fixture.Start(), "Cannot start cancellable operation");
        require(fixture.Controller.Cancelling(), "Cancellation not visible");
        require(!waitFor(fixture.Controller) && !fixture.Controller.Failed(),
                "Cancellation reported as failure");
        require(!QFileInfo::exists(fixture.Path("result")), "Cancelled export published output");
        fixture.Clean();
        QObject::disconnect(connection);
        require(fixture.Start() && waitFor(fixture.Controller),
                "Restart after cancellation failed");
        fixture.Controller.Cancel();
        require(!fixture.Controller.Cancelling(), "Idle cancel changed state");
    }

    void testControllerDestructionJoinsOutstandingWorker()
    {
        const auto fixture = Fixture{};
        {
            auto controller = PackController{};
            require(controller.AddPack(QUrl::fromLocalFile(fixture.Pack("input"))),
                    "Cannot add source");
            require(controller.Start(0, "34", fixture.Path("result"), 0, "test"),
                    "Cannot start export");
        }
        QCoreApplication::processEvents();
        fixture.Clean();
    }

    void testQMLStackButtonsChangePriorityAndRemoveSelection()
    {
        auto fixture = Fixture{};
        const auto first = fixture.Pack("first");
        const auto second = fixture.Pack("second");
        fixture.Add(first);
        fixture.Add(second);
        const auto interface = Interface(fixture.Controller);
        interface.Set("packStack", "currentIndex", 1);
        interface.Click("moveUp");
        require(fixture.Controller.Packs() == QStringList{second, first},
                "QML Up did not change order");
        interface.Click("moveDown");
        require(fixture.Controller.Packs() == QStringList{first, second},
                "QML Down did not change order");
        interface.Click("removePack");
        require(fixture.Controller.Packs() == QStringList{first},
                "QML removal targeted wrong pack");
        require(!interface.Warnings, "QML stack operations produced warnings");
    }

    void testQMLFormStartsRealExportAndBindsBusyControls()
    {
        auto fixture = Fixture{};
        fixture.Add(fixture.Pack("input", {{OPAQUE, "from-qml"}}));
        const auto interface = Interface(fixture.Controller);
        require(interface.Item("targetFormat")->property("text").toString().isEmpty() &&
                    interface.Root->property("effectiveFormat").toString() == "34",
                "Export did not default to auto format");
        interface.Set("outputPath", "text", fixture.Path("result"));
        interface.Set("outputFormat", "currentIndex", 1);
        interface.Set("description", "text", "QML description");
        interface.Click("startExport");
        require(fixture.Controller.Busy(), "QML did not start worker");
        require(!interface.Item("startExport")->property("enabled").toBool() &&
                    interface.Item("cancelExport")->property("enabled").toBool() &&
                    !interface.Item("packStack")->property("enabled").toBool(),
                "QML busy controls incorrect");
        require(waitFor(fixture.Controller), "QML export failed");
        require(read(fixture.Path("result/" + OPAQUE)) == "from-qml", "QML export missing content");
        require(Json::parse(read(fixture.Path("result/pack.mcmeta")).toStdString())
                        .at("pack")
                        .at("description") == "QML description",
                "QML did not pass description");
        require(!interface.Item("startExport")->property("enabled").toBool() &&
                    interface.Root->property("completed").toBool() &&
                    interface.Item("showResult")->property("visible").toBool() &&
                    fixture.Controller.ResultPath() == fixture.Path("result") &&
                    interface.Item("packStack")->property("enabled").toBool() &&
                    !interface.Warnings,
                "QML did not show success or allowed overwriting output");
    }

    void testInputPreviewValidatesFormatBoundariesAndNewOutputPathsWithoutChangingState()
    {
        const auto fixture = Fixture{};
        const auto validOutput = fixture.Path("result.zip");
        for (const auto& target : {"4", "34", "65.2", "2147483647.2147483647"})
        {
            const auto errors = PackStacker::PackController::ValidateInputs(target, validOutput);
            require(errors.value("target").toString().isEmpty() &&
                        errors.value("output").toString().isEmpty(),
                    "Valid preview settings rejected");
        }
        for (const auto& target : {
                 "",
                 "3",
                 "-4",
                 "4.",
                 ".4",
                 "4.1.2",
                 "4.a",
                 " 34",
                 "2147483648",
                 "4.2147483648",
             })
        {
            require(!PackStacker::PackController::ValidateInputs(target, validOutput)
                         .value("target")
                         .toString()
                         .isEmpty(),
                    "Invalid format preview accepted");
        }
        write(fixture.Path("existing.zip"), "unchanged");
        for (const auto& output : {
                 QString{},
                 fixture.Path("existing.zip"),
                 fixture.Path("missing/result.zip"),
                 fixture.Path("CON"),
                 fixture.Path("bad?.zip"),
             })
        {
            require(!PackStacker::PackController::ValidateInputs("34", output)
                         .value("output")
                         .toString()
                         .isEmpty(),
                    "Unsafe output preview accepted");
        }
        require(!fixture.Controller.Busy() && !fixture.Controller.Failed() &&
                    fixture.Controller.Status().isEmpty() &&
                    read(fixture.Path("existing.zip")) == "unchanged",
                "Preview mutated controller or files");
    }

    void testCatalogAutoFormatUsesLegacyPrimaryOrExactModernValueButNeverGuessesARange()
    {
        const auto fixture = Fixture{};
        const auto legacy = fixture.Pack("legacy");
        const auto modern = fixture.Pack("modern");
        const auto range = fixture.Pack("range");
        write(legacy + "/pack.mcmeta", encode(Json{
                                           {
                                               "pack",
                                               {
                                                   {"pack_format", 34},
                                                   {"supported_formats", Json::array({32, 34})},
                                                   {"description", "Legacy"},
                                               },
                                           },
                                       }));
        write(modern + "/pack.mcmeta", encode(Json{
                                           {
                                               "pack",
                                               {
                                                   {"min_format", Json::array({65, 2})},
                                                   {"max_format", Json::array({65, 2})},
                                                   {"description", "Modern"},
                                               },
                                           },
                                       }));
        write(range + "/pack.mcmeta", encode(Json{
                                          {
                                              "pack",
                                              {
                                                  {"min_format", Json::array({65, 1})},
                                                  {"max_format", Json::array({65, 2})},
                                                  {"description", "Range"},
                                              },
                                          },
                                      }));
        const auto snapshot = take(ReadCatalog({}, {legacy, modern, range}, {}));
        require(snapshot.Details.value(CatalogPathKey(legacy)).AutoFormat == "34" &&
                    snapshot.Details.value(CatalogPathKey(modern)).AutoFormat == "65.2" &&
                    snapshot.Details.value(CatalogPathKey(range)).AutoFormat.isEmpty() &&
                    snapshot.Details.value(CatalogPathKey(range)).Error.isEmpty(),
                "Auto format guessed a range or ignored the declared primary");
    }

    void testQMLAutoFormatFollowsTopPackWhilePreservingExplicitOverride()
    {
        auto fixture = Fixture{};
        fixture.Add(fixture.Pack("first", {}, 34));
        fixture.Add(fixture.Pack("second", {}, 35));
        const auto interface = Interface(fixture.Controller);
        require(interface.Root->property("effectiveFormat").toString() == "34" &&
                    interface.Item("targetFormat")->property("text").toString().isEmpty(),
                "Default auto format missing");
        interface.Set("packStack", "currentIndex", 1);
        interface.Click("moveUp");
        require(interface.Root->property("effectiveFormat").toString() == "35",
                "Auto format did not follow priority");
        interface.Set("targetFormat", "text", "65.2");
        interface.Click("moveDown");
        require(interface.Root->property("effectiveFormat").toString() == "65.2",
                "Explicit override changed with priority");
        interface.Set("targetFormat", "text", "");
        require(interface.Root->property("effectiveFormat").toString() == "34",
                "Clearing override did not restore auto");
        require(!interface.Warnings, "Automatic format produced QML warnings");
    }

    void testQMLReadinessShowsFieldErrorsAndRangeAmbiguityBeforeExport()
    {
        auto fixture = Fixture{};
        const auto input = fixture.Pack("range");
        write(input + "/pack.mcmeta", encode(Json{
                                          {
                                              "pack",
                                              {
                                                  {"min_format", Json::array({65, 1})},
                                                  {"max_format", Json::array({65, 2})},
                                                  {"description", "Range"},
                                              },
                                          },
                                      }));
        fixture.Add(input);
        const auto interface = Interface(fixture.Controller);
        interface.Set("outputPath", "text", fixture.Path("result.zip"));
        require(!interface.Item("startExport")->property("enabled").toBool() &&
                    interface.Item("targetFormat")->property("invalid").toBool() &&
                    !interface.Item("targetValidation")->property("text").toString().isEmpty(),
                "Ambiguous auto format did not request manual choice");
        interface.Set("targetFormat", "text", "65.2");
        require(interface.Item("startExport")->property("enabled").toBool() &&
                    !interface.Root->property("completed").toBool(),
                "Valid fields did not enable export or implied successful export");
        interface.Set("targetFormat", "text", "invalid");
        require(!interface.Item("startExport")->property("enabled").toBool(),
                "Invalid format enabled export");
        interface.Set("targetFormat", "text", "65.2");
        interface.Set("outputPath", "text", input);
        require(!interface.Item("startExport")->property("enabled").toBool() &&
                    interface.Item("outputPath")->property("invalid").toBool() &&
                    !interface.Item("outputValidation")->property("text").toString().isEmpty() &&
                    !fixture.Controller.Busy() && !interface.Warnings,
                "Existing output did not show inline validation");
    }

    void testQMLDefaultsUseTheLibraryDirectoryAndFullTopPackDescriptionAndReachExport()
    {
        auto fixture = Fixture{};
        const auto input = fixture.Pack("library/input", {{OPAQUE, "original"}});
        const auto description =
            QStringLiteral("§aОписание\n") + QString(900, QChar('x')) + QStringLiteral(" 世界");
        write(input + "/pack.mcmeta",
              encode(Json{
                  {"pack", {{"pack_format", 34}, {"description", description.toStdString()}}},
              }));
        const auto originalMetadata = read(input + "/pack.mcmeta");
        fixture.Add(input);
        const auto interface = Interface(fixture.Controller);
        require(interface.Item("description")->property("text").toString() == description &&
                    fixture.Controller.SelectedPacks()
                            .at(0)
                            .toMap()
                            .value("description")
                            .toString()
                            .size() <= 512,
                "Default description used the truncated catalog preview");
        require(
            fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("library"))) &&
                waitForCatalog(fixture.Controller),
            "Cannot choose default output directory");
        require(interface.Item("outputPath")->property("text").toString() ==
                    fixture.Path("library/combined-pack.zip"),
                "ZIP default is not a new result inside the library directory");
        interface.Set("outputFormat", "currentIndex", 1);
        const auto output = fixture.Path("library/combined-pack");
        require(interface.Item("outputPath")->property("text").toString() == output &&
                    interface.Root->property("outputPathAutomatic").toBool() &&
                    interface.Root->property("descriptionAutomatic").toBool(),
                "Changing output kind did not update the automatic path");
        interface.Click("startExport");
        require(waitFor(fixture.Controller) && read(output + '/' + OPAQUE) == "original" &&
                    Json::parse(read(output + "/pack.mcmeta").toStdString())
                            .at("pack")
                            .at("description") == description.toStdString() &&
                    read(input + "/pack.mcmeta") == originalMetadata && !interface.Warnings,
                "Automatic defaults were not exported intact or changed the source");
    }

    void testQMLAutomaticDefaultsFollowPriorityUntilEdited()
    {
        auto fixture = Fixture{};
        const auto first = fixture.Pack("library/a");
        const auto second = fixture.Pack("library/b");
        write(first + "/pack.mcmeta",
              encode(Json{{"pack", {{"pack_format", 34}, {"description", "Alpha"}}}}));
        write(second + "/pack.mcmeta",
              encode(Json{{"pack", {{"pack_format", 34}, {"description", "Beta"}}}}));
        fixture.Pack("other/c");
        fixture.Add(first);
        const auto interface = Interface(fixture.Controller);
        require(
            fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("library"))) &&
                waitForCatalog(fixture.Controller),
            "Cannot load library");
        interface.Set("availablePacks", "currentIndex", 0);
        interface.Click("addAvailable");
        interface.Click("moveUp");
        require(interface.Item("description")->property("text").toString() == "Beta",
                "Description did not follow the new top pack");
        interface.Click("moveDown");
        require(interface.Item("description")->property("text").toString() == "Alpha",
                "Description did not follow restored priority");
        const auto custom = fixture.Path("custom.zip");
        interface.Set("outputPath", "text", custom);
        interface.Set("description", "text", "");
        interface.Click("moveUp");
        interface.Set("outputFormat", "currentIndex", 1);
        require(fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("other"))) &&
                    waitForCatalog(fixture.Controller),
                "Cannot switch library");
        require(interface.Item("outputPath")->property("text").toString() == custom &&
                    interface.Item("description")->property("text").toString().isEmpty() &&
                    !interface.Root->property("outputPathAutomatic").toBool() &&
                    !interface.Root->property("descriptionAutomatic").toBool(),
                "Automatic updates overwrote manual fields, including an empty description");
        require(!interface.Warnings, "Automatic defaults produced QML warnings");
    }

    void testQMLAutomaticOutputNeverOverwritesAnExistingResultAndAcceptsAManualReplacementPath()
    {
        auto fixture = Fixture{};
        fixture.Add(fixture.Pack("library/input"));
        const auto existing = fixture.Path("library/combined-pack.zip");
        write(existing, "keep this file");
        const auto interface = Interface(fixture.Controller);
        require(
            fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("library"))) &&
                waitForCatalog(fixture.Controller),
            "Cannot load library with an existing result");
        require(interface.Item("outputPath")->property("text").toString() == existing &&
                    !interface.Item("startExport")->property("enabled").toBool() &&
                    interface.Item("outputPath")->property("invalid").toBool(),
                "An existing automatic output was accepted");
        interface.Set("outputPath", "text", fixture.Path("library/new-result.zip"));
        interface.Click("startExport");
        require(waitFor(fixture.Controller) && read(existing) == "keep this file" &&
                    !interface.Warnings,
                "Manual output failed or overwrote the existing default result");
    }

    void testRuleImportsAreTransactionalAndReachExport()
    {
        auto fixture = Fixture{};
        const auto lower =
            fixture.Pack("lower", {{LANGUAGE, R"({"lower":"keep","shared":"low"})"}});
        const auto higher = fixture.Pack("higher", {{LANGUAGE, R"({"shared":"high"})"}});
        fixture.Add(higher);
        fixture.Add(lower);
        const auto rows = QVariantList{
            QVariantMap{{"match", "assets/**"}, {"strategy", "low_priority"}},
        };
        require(fixture.Controller.ApplyRules(rows), "Cannot apply rules");
        const auto path = QUrl::fromLocalFile(fixture.Path("правила-😀.json"));
        require(fixture.Controller.ExportRules(path, rows), "Cannot export rules");
        require(!fixture.Controller.ExportRules(path, rows), "Rules export overwrote a file");
        require(fixture.Controller.ApplyRules({}), "Cannot clear rules");
        const auto imported = fixture.Controller.ImportRules(path);
        require(imported.value("ok").toBool() && fixture.Controller.Rules().isEmpty(),
                "Import applied rules instead of returning a draft");
        require(fixture.Controller.ApplyRules(imported.value("rules").toList()),
                "Cannot apply imported draft");
        require(!fixture.Controller.ApplyRules(
                    {QVariantMap{{"match", "../bad"}, {"strategy", "standard"}}}) &&
                    fixture.Controller.Rules() == rows,
                "Invalid draft changed the applied rules");
        require(!fixture.Controller.ApplyRules(
                    {QVariantMap{{"match", "assets/**"}, {"strategy", "unknown"}}}) &&
                    fixture.Controller.Rules() == rows,
                "Unknown strategy changed the applied rules");
        require(!fixture.Controller.ApplyRules(
                    {QVariantMap{{"match", 42}, {"strategy", "standard"}}}) &&
                    fixture.Controller.Rules() == rows,
                "Non-string match changed the applied rules");
        require(fixture.Start() && waitFor(fixture.Controller), "Cannot export with rules");
        require(Json::parse(read(fixture.Path("result/") + LANGUAGE).toStdString()) ==
                    Json::parse(R"({"lower":"keep","shared":"low"})"),
                "Controller did not pass the applied rules to Core");
    }

    void testQMLRuleDraftAppliesCancelsAndReorders()
    {
        auto fixture = Fixture{};
        const auto interface = Interface(fixture.Controller);
        interface.Click("rulesButton");
        interface.Click("addRule");
        QCoreApplication::processEvents();
        require(fixture.Controller.Rules().isEmpty(), "Draft was applied immediately");
        interface.Click("cancelRuleDraft");
        interface.Click("rulesButton");
        require(interface.Item("ruleList")->property("count").toInt() == 0,
                "Cancelled draft survived reopening");
        interface.Click("addRule");
        interface.Click("addRule");
        require(QMetaObject::invokeMethod(interface.Item("ruleList"), "forceLayout"),
                "Cannot lay out rule rows");
        QCoreApplication::processEvents();
        interface.Set("ruleMatch0", "text", "../bad");
        require(QMetaObject::invokeMethod(interface.Item("ruleMatch0"), "textEdited"),
                "Cannot edit invalid rule match");
        interface.Click("applyRuleDraft");
        require(interface.Item("rulesEditor")->property("visible").toBool() &&
                    !interface.Item("rulesMessage")->property("text").toString().isEmpty() &&
                    fixture.Controller.Rules().isEmpty(),
                "Invalid rule closed the editor or changed the applied rules");
        interface.Set("ruleMatch0", "text", "extra/**");
        require(QMetaObject::invokeMethod(interface.Item("ruleMatch0"), "textEdited"),
                "Cannot edit rule match");
        interface.Set("ruleStrategy0", "currentIndex", 2);
        require(QMetaObject::invokeMethod(interface.Item("ruleStrategy0"), "activated",
                                          Q_ARG(qint32, 2)),
                "Cannot choose rule strategy");
        interface.Click("ruleDown0");
        QCoreApplication::processEvents();
        interface.Click("applyRuleDraft");
        require(fixture.Controller.Rules().size() == 2 &&
                    fixture.Controller.Rules().at(1).toMap().value("match") == "extra/**" &&
                    fixture.Controller.Rules().at(1).toMap().value("strategy") == "low_priority" &&
                    !interface.Item("rulesEditor")->property("visible").toBool(),
                "Editor did not apply the reordered draft");
        interface.Click("rulesButton");
        require(QMetaObject::invokeMethod(interface.Item("ruleList"), "forceLayout"),
                "Cannot lay out imported rule rows");
        interface.Click("ruleDelete0");
        interface.Click("applyRuleDraft");
        require(fixture.Controller.Rules().size() == 1 && !interface.Warnings,
                "Rule deletion failed or produced QML warnings");
    }

    void testQMLExternalPackReturnsToAvailableListAndIconButtonsExplainTheirActions()
    {
        auto fixture = Fixture{};
        const auto input = fixture.Pack("elsewhere/external");
        fixture.Pack("library/other");
        fixture.Add(input);
        const auto interface = Interface(fixture.Controller);
        require(
            fixture.Controller.SetSourceDirectory(QUrl::fromLocalFile(fixture.Path("library"))) &&
                waitForCatalog(fixture.Controller),
            "Cannot load library");
        interface.Set("packStack", "currentIndex", 0);
        interface.Click("removePack");
        QCoreApplication::processEvents();
        require(fixture.Controller.AvailablePacks().size() == 2 &&
                    fixture.Controller.AvailablePacks()
                            .at(interface.Item("availablePacks")->property("currentIndex").toInt())
                            .toMap()
                            .value("path") == input &&
                    QFileInfo::exists(input + "/pack.mcmeta"),
                "External pack was not returned and selected on the left");
        for (const auto* name : {
                 "addAvailable",
                 "removePack",
                 "moveUp",
                 "moveDown",
                 "browseSource",
                 "browseOutput",
             })
        {
            require(!interface.Item(name)->property("toolTip").toString().isEmpty(),
                    "Icon button is missing tooltip");
        }
        for (const auto* name : {
                 "rulesButton",
                 "browseSource",
                 "browseOutput",
                 "clearPacks",
                 "startExport",
             })
        {
            const auto* background = interface.Item(name)->property("background").value<QObject*>();
            require(
                background != nullptr && background->property("color").value<QColor>().alpha() > 0,
                "Button does not have a visible background");
        }
        require(!interface.Warnings, "Transfer or tooltips produced QML warnings");
    }

    QRectF layoutRectangle(const Interface& interface, const char* name)
    {
        const auto* item = qobject_cast<QQuickItem*>(interface.Item(name));
        require(item != nullptr, "Layout object is not an item");
        return {item->mapToScene(QPointF{}), item->size()};
    }

    bool layoutNear(qreal first, qreal second)
    {
        return qAbs(first - second) <= 1;
    }

    void checkPanelLayout(const Interface& interface)
    {
        const auto available = layoutRectangle(interface, "availablePacks");
        const auto stack = layoutRectangle(interface, "packStack");
        require(layoutNear(available.top(), stack.top()) &&
                    layoutNear(available.bottom(), stack.bottom()) && available.height() > 100,
                "Panel lists do not share top and bottom edges");
        require(layoutNear(layoutRectangle(interface, "clearPacks").center().y(),
                           layoutRectangle(interface, "browseSource").center().y()) &&
                    layoutRectangle(interface, "clearPacks").bottom() < stack.top(),
                "Clear stack button is not aligned with the panel header");
        require(interface.Item("sourcePath")->parent() == interface.Item("availableHeader") &&
                    interface.Item("browseSource")->parent() == interface.Item("availableHeader") &&
                    layoutNear(layoutRectangle(interface, "sourcePath").center().y(),
                               layoutRectangle(interface, "browseSource").center().y()) &&
                    layoutRectangle(interface, "sourcePath").width() >= 80,
                "Catalog selector is not usable in the available packs header");
        require(layoutRectangle(interface, "priorityHint").top() -
                            layoutRectangle(interface, "stackTitle").bottom() <=
                        3 &&
                    layoutRectangle(interface, "priorityHint").bottom() < stack.top(),
                "Priority hint is detached from the stack heading");
        require(layoutNear(layoutRectangle(interface, "packType").center().y(),
                           (layoutRectangle(interface, "targetFormat").top() +
                            layoutRectangle(interface, "targetValidation").bottom()) /
                               2),
                "Top form controls are not aligned");
        require(layoutNear(layoutRectangle(interface, "packTypeLabel").left(),
                           layoutRectangle(interface, "availablePanel").left()) &&
                    layoutNear(layoutRectangle(interface, "targetFormatLabel").left(),
                               layoutRectangle(interface, "selectedPanel").left()) &&
                    layoutNear(layoutRectangle(interface, "targetFormat").right(),
                               layoutRectangle(interface, "selectedPanel").right()),
                "Top settings are inset from the outer panel boundaries");
        require(layoutRectangle(interface, "packType").top() >=
                    layoutRectangle(interface, "appHeader").bottom() + 12,
                "Configuration controls touch the application header");
    }

    void checkOutputLayout(const Interface& interface)
    {
        const auto path = layoutRectangle(interface, "outputPath");
        const auto description = layoutRectangle(interface, "description");
        require(
            layoutRectangle(interface, "targetValidation").top() -
                        layoutRectangle(interface, "targetFormat").bottom() <=
                    3 &&
                layoutRectangle(interface, "targetValidation").height() <=
                    interface.Item("targetValidation")->property("implicitHeight").toReal() + 1 &&
                description.top() - layoutRectangle(interface, "descriptionLabel").bottom() <= 3,
            "Explanatory text has excessive spacing or reserved height");
        const auto pathLabel = layoutRectangle(interface, "outputPathLabel");
        const auto descriptionLabel = layoutRectangle(interface, "descriptionLabel");
        require(layoutNear(pathLabel.left(), path.left()) && pathLabel.bottom() <= path.top() &&
                    path.top() - pathLabel.bottom() <= 3 &&
                    layoutNear(descriptionLabel.left(), description.left()),
                "Output labels are not directly above their fields");
        const auto validationVisible =
            interface.Item("outputValidation")->property("visible").toBool();
        const auto previousBottom = validationVisible
                                        ? layoutRectangle(interface, "outputValidation").bottom()
                                        : path.bottom();
        require(descriptionLabel.top() - previousBottom <= 4 &&
                    (!validationVisible ||
                     layoutRectangle(interface, "outputValidation").top() - path.bottom() <= 3),
                "Description label did not replace the old output hint position");
        require(interface.Item("outputFormat")->property("vertical").toBool() &&
                    layoutRectangle(interface, "outputFormat").right() < path.left() &&
                    layoutNear(layoutRectangle(interface, "outputFormat").top(), path.top()) &&
                    layoutNear(layoutRectangle(interface, "outputFormat").bottom(),
                               description.bottom()) &&
                    layoutNear(layoutRectangle(interface, "browseOutput").center().y(),
                               path.center().y()) &&
                    layoutNear(path.left(), description.left()) &&
                    layoutNear(path.right(), description.right()),
                "Output form columns or control centers do not match");
        require(layoutNear((layoutRectangle(interface, "addAvailable").center().y() +
                            layoutRectangle(interface, "removePack").center().y()) /
                               2,
                           (layoutRectangle(interface, "moveUp").center().y() +
                            layoutRectangle(interface, "moveDown").center().y()) /
                               2),
                "Transfer and priority controls have different vertical centers");
        require(layoutRectangle(interface, "outputSettings").bottom() <
                        layoutRectangle(interface, "startExport").top() &&
                    description.bottom() < layoutRectangle(interface, "outputSettings").bottom() &&
                    !interface.Warnings,
                "Output settings overlap actions or exceed their container");
    }

    void saveLayoutCapture(QQuickWindow& window, bool english, const QSize& size,
                           const QString& output)
    {
        const auto capture = qEnvironmentVariable("PACKSTACKER_CAPTURE_DIR");
        if (capture.isEmpty() || size.width() == 1440)
        {
            return;
        }
        auto outputState = QStringLiteral("valid");
        if (output.isEmpty())
        {
            outputState = QStringLiteral("empty");
        }
        else if (output.contains("missing"))
        {
            outputState = QStringLiteral("error");
        }
        require(
            window.grabWindow().save(QDir(capture).filePath(QStringLiteral("layout-%1-%2-%3.png")
                                                                .arg(english ? "en" : "ru")
                                                                .arg(size.width())
                                                                .arg(outputState))),
            "Cannot save layout capture");
    }

    void testQMLLayoutKeepsPanelListsAndFormControlsAlignedAcrossSizesAndValidationStates()
    {
        for (const auto english : {false, true})
        {
            auto translator = QTranslator{};
            require(InstallInterfaceTranslation(translator, {english ? "en-US" : "ru-RU"}),
                    "Cannot install layout test translation");
            auto fixture = Fixture{};
            fixture.Add(fixture.Pack("input"));
            const auto interface = Interface(fixture.Controller);
            const auto* refresh = interface.Item("refreshCatalog");
            require(refresh->property("enabled").toBool() &&
                        refresh->property("toolTip").toString() ==
                            (english ? "Refresh catalog" : "Обновить каталог"),
                    "Catalog refresh is unavailable or untranslated");
            auto* window = qobject_cast<QQuickWindow*>(interface.Root);
            require(window != nullptr, "Layout root is not a window");
            for (const auto& size : {QSize(1000, 640), QSize(1280, 720), QSize(1440, 900)})
            {
                window->resize(size);
                for (const auto& output : {
                         QString{},
                         fixture.Path("result.zip"),
                         fixture.Path("missing/result.zip"),
                     })
                {
                    interface.Set("outputPath", "text", output);
                    interface.Set("targetFormat", "text", output.isEmpty() ? "bad format" : "");
                    QCoreApplication::processEvents();
                    require(!window->grabWindow().isNull(), "Cannot render layout variant");
                    checkPanelLayout(interface);
                    checkOutputLayout(interface);
                    saveLayoutCapture(*window, english, size, output);
                }
            }
            auto titleFont = interface.Item("stackTitle")->property("font").value<QFont>();
            titleFont.setPixelSize(titleFont.pixelSize() + 8);
            require(interface.Item("stackTitle")->setProperty("font", titleFont),
                    "Cannot enlarge stack heading font");
            QCoreApplication::processEvents();
            require(!window->grabWindow().isNull(), "Cannot render enlarged heading");
            const auto* header = interface.Item("clearPacks")->parent();
            require(header->property("height").toReal() >=
                            header->property("implicitHeight").toReal() &&
                        layoutNear(layoutRectangle(interface, "clearPacks").center().y(),
                                   layoutRectangle(interface, "browseSource").center().y()) &&
                        layoutRectangle(interface, "priorityHint").bottom() <
                            layoutRectangle(interface, "packStack").top(),
                    "Panel headers do not adapt to enlarged heading text");
            require(layoutNear((layoutRectangle(interface, "addAvailable").center().y() +
                                layoutRectangle(interface, "removePack").center().y()) /
                                   2,
                               (layoutRectangle(interface, "moveUp").center().y() +
                                layoutRectangle(interface, "moveDown").center().y()) /
                                   2),
                    "Transfer controls do not follow the resized panel headers");
        }
    }

    void testQMLPackWarningIsVisibleWithoutHoveringAndSelectedPackRemainsInViewAfterMoving()
    {
        auto fixture = Fixture{};
        for (auto index = qint32{0}; index < 9; ++index)
        {
            const auto input = fixture.Pack(QStringLiteral("pack-%1").arg(index));
            if (index == 0)
            {
                write(input + "/pack.png", "not png");
            }
            fixture.Add(input);
        }
        const auto interface = Interface(fixture.Controller);
        auto* window = qobject_cast<QQuickWindow*>(interface.Root);
        require(window != nullptr && !window->grabWindow().isNull(),
                "Cannot render warning preview");
        auto warningVisible = false;
        for (const auto* item : visualItems(window->contentItem()))
        {
            if (item->objectName() == "packWarning" && item->isVisible() &&
                !item->property("text").toString().isEmpty())
            {
                warningVisible = true;
            }
        }
        require(warningVisible, "Warning requires hovering to be discovered");
        interface.Set("packStack", "currentIndex", 8);
        interface.Click("moveUp");
        QCoreApplication::processEvents();
        require(!window->grabWindow().isNull(), "Cannot render moved selection");
        const auto* list = interface.Item("packStack");
        const auto* current = list->property("currentItem").value<QQuickItem*>();
        require(current != nullptr && list->property("currentIndex").toInt() == 7,
                "Moving pack lost selection");
        const auto itemTop = current->y();
        const auto contentY = list->property("contentY").toReal();
        require(
            itemTop >= contentY - 1 &&
                itemTop + current->height() <= contentY + list->property("height").toReal() + 1 &&
                !interface.Warnings,
            "Moved selection is outside visible list");
    }

    void testQMLCancelButtonForwardsCooperativeCancellation()
    {
        auto fixture = Fixture{};
        fixture.Add(fixture.Pack("input"));
        auto interface = Interface(fixture.Controller);
        const auto connection = QObject::connect(
            &fixture.Controller, &PackController::StateChanged, &fixture.Controller,
            [&]
            {
                if (fixture.Controller.Busy() && !fixture.Controller.Cancelling())
                {
                    interface.Click("cancelExport");
                }
            });
        require(fixture.Start(), "Cannot start export");
        require(fixture.Controller.Cancelling(), "QML Cancel did not reach controller");
        require(!interface.Item("cancelExport")->property("enabled").toBool(),
                "Cancel remained enabled");
        require(!waitFor(fixture.Controller) && !fixture.Controller.Failed(),
                "QML cancellation failed");
        QObject::disconnect(connection);
        require(!QFileInfo::exists(fixture.Path("result")) && !interface.Warnings,
                "Cancelled QML export produced output or warnings");
        fixture.Clean();
    }

    void testQMLCloseDuringWorkRequestsConfirmationAndWaits()
    {
        auto fixture = Fixture{};
        fixture.Add(fixture.Pack("input"));
        const auto interface = Interface(fixture.Controller);
        require(fixture.Start(), "Cannot start export");
        auto* window = qobject_cast<QQuickWindow*>(interface.Root);
        require(window != nullptr, "QML root is not a window");
        require(!window->close() && window->isVisible(), "Busy window closed without confirmation");
        require(interface.Item("closeConfirmation")->property("visible").toBool(),
                "Close confirmation did not open");
        require(QMetaObject::invokeMethod(interface.Item("closeConfirmation"), "accepted"),
                "Cannot confirm close");
        waitFor(fixture.Controller);
        require(!window->isVisible() && !interface.Warnings,
                "Window did not close after completion");
        fixture.Clean();
    }

}

int32_t main(int32_t argc, char* argv[])
{
    const auto application = QGuiApplication(argc, argv);
    QCoreApplication::setApplicationVersion(QStringLiteral("1.2.3"));
    QGuiApplication::setQuitOnLastWindowClosed(false);
    qmlRegisterUncreatableType<PackController>("PackStacker", 1, 0, "PackController",
                                               "The test supplies the controller");
    const auto tests = std::vector<std::pair<const char*, std::function<void()>>>{
        {
            "interface language follows supported UI preferences with English fallback",
            testInterfaceLanguageFollowsSupportedUIPreferencesWithEnglishFallback,
        },
        {
            "embedded translations localize QML controller and background catalog in both "
            "languages",
            testEmbeddedTranslationsLocalizeQMLControllerAndBackgroundCatalogInBothLanguages,
        },
        {
            "stack additions, Unicode, duplicate and remote URL rejection",
            testStackAdditionsUnicodeDuplicateAndRemoteURLRejection,
        },
        {
            "batch additions preserve URL order and notify once",
            testBatchAdditionsPreserveURLOrderAndNotifyOnce,
        },
        {
            "internal drag transfers packs and reorders the stack",
            testDraggingMovesPacksBetweenPanelsAndReordersStack,
        },
        {"wrong pack type warns and blocks export", testWrongPackTypeWarnsAndBlocksExport},
        {
            "refresh rechecks selected pack types at the same path",
            testRefreshRechecksSelectedPackTypesAtTheSamePath,
        },
        {
            "type detection respects active overlays and ambiguous content",
            testTypeDetectionRespectsActiveOverlaysAndAmbiguousContent,
        },
        {
            "batch duplicates reject the whole group without partial additions",
            testBatchDuplicatesRejectTheWholeGroupWithoutPartialAdditions,
        },
        {
            "batch URL validation is transactional and preserves encoded filename characters",
            testBatchURLValidationIsTransactionalAndPreservesEncodedFilenameCharacters,
        },
        {
            "batch additions enforce the total pack limit before committing",
            testBatchAdditionsEnforceTheTotalPackLimitBeforeCommitting,
        },
        {
            "QML catalog transfers preserve order and selection",
            testQMLCatalogTransfersPreserveOrderAndSelection,
        },
        {
            "catalog scan separates available and selected packs and supports transfers",
            testCatalogScanSeparatesAvailableAndSelectedPacksAndSupportsTransfers,
        },
        {
            "catalog icon decoding is bounded and text descriptions remain plain",
            testCatalogIconDecodingIsBoundedAndTextDescriptionsRemainPlain,
        },
        {
            "failed or cancelled catalog changes preserve the previous directory and stack",
            testFailedOrCancelledCatalogChangesPreserveThePreviousDirectoryAndStack,
        },
        {
            "QML transfers preserve both panels and appearance follows system hints without a "
            "theme "
            "button",
            testQMLTransfersPreserveBothPanelsAndAppearanceFollowsSystemHintsWithoutAThemeButton,
        },
        {
            "populated QML supports both themes and optional visual captures",
            testPopulatedQMLSupportsBothThemesAndOptionalVisualCaptures,
        },
        {
            "stack move and removal validate indices",
            testStackMoveAndRemovalValidateIndices,
        },
        {
            "invalid requests fail synchronously without becoming busy",
            testInvalidRequestsFailSynchronouslyWithoutBecomingBusy,
        },
        {
            "async export preserves priority, translations and opaque bytes",
            testAsyncExportPreservesPriorityTranslationsAndOpaqueBytes,
        },
        {
            "running operation locks stack and rejects a second start",
            testRunningOperationLocksStackAndRejectsASecondStart,
        },
        {
            "Unicode ZIP output is reusable as input",
            testUnicodeZIPOutputIsReusableAsInput,
        },
        {
            "data pack mode merges tags",
            testDataPackModeMergesTags,
        },
        {
            "exact minor format and description reach output metadata",
            testExactMinorFormatAndDescriptionReachOutputMetadata,
        },
        {
            "core errors expose context and allow retry",
            testCoreErrorsExposeContextAndAllowRetry,
        },
        {
            "existing output and source overlap remain protected",
            testExistingOutputAndSourceOverlapRemainProtected,
        },
        {
            "cancellation before dispatch cleans up and permits restart",
            testCancellationBeforeDispatchCleansUpAndPermitsRestart,
        },
        {
            "controller destruction joins outstanding worker",
            testControllerDestructionJoinsOutstandingWorker,
        },
        {
            "QML stack buttons change priority and remove selection",
            testQMLStackButtonsChangePriorityAndRemoveSelection,
        },
        {
            "QML form starts real export and binds busy controls",
            testQMLFormStartsRealExportAndBindsBusyControls,
        },
        {
            "input preview validates format boundaries and new output paths without changing state",
            testInputPreviewValidatesFormatBoundariesAndNewOutputPathsWithoutChangingState,
        },
        {
            "catalog auto format uses legacy primary or exact modern value but never guesses a "
            "range",
            testCatalogAutoFormatUsesLegacyPrimaryOrExactModernValueButNeverGuessesARange,
        },
        {
            "QML auto format follows top pack while preserving explicit override",
            testQMLAutoFormatFollowsTopPackWhilePreservingExplicitOverride,
        },
        {
            "QML readiness shows field errors and range ambiguity before export",
            testQMLReadinessShowsFieldErrorsAndRangeAmbiguityBeforeExport,
        },
        {
            "QML defaults use the library directory and full top pack description and reach export",
            testQMLDefaultsUseTheLibraryDirectoryAndFullTopPackDescriptionAndReachExport,
        },
        {
            "QML automatic defaults follow priority until edited",
            testQMLAutomaticDefaultsFollowPriorityUntilEdited,
        },
        {
            "QML automatic output never overwrites an existing result and accepts a manual "
            "replacement path",
            testQMLAutomaticOutputNeverOverwritesAnExistingResultAndAcceptsAManualReplacementPath,
        },
        {
            "rule imports are transactional and reach export",
            testRuleImportsAreTransactionalAndReachExport,
        },
        {
            "QML rule draft applies cancels and reorders",
            testQMLRuleDraftAppliesCancelsAndReorders,
        },
        {
            "QML external pack returns to available list and icon buttons explain their actions",
            testQMLExternalPackReturnsToAvailableListAndIconButtonsExplainTheirActions,
        },
        {
            "QML layout keeps panel lists and form controls aligned across sizes and validation "
            "states",
            testQMLLayoutKeepsPanelListsAndFormControlsAlignedAcrossSizesAndValidationStates,
        },
        {
            "QML pack warning is visible without hovering and selected pack remains in view after "
            "moving",
            testQMLPackWarningIsVisibleWithoutHoveringAndSelectedPackRemainsInViewAfterMoving,
        },
        {
            "QML Cancel button forwards cooperative cancellation",
            testQMLCancelButtonForwardsCooperativeCancellation,
        },
        {
            "QML close during work requests confirmation and waits",
            testQMLCloseDuringWorkRequestsConfirmationAndWaits,
        },
    };
    auto failures = std::size_t{0};
    for (const auto& [name, test] : tests)
    {
        try
        {
            test();
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception& error)
        {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
