#include "gui/localization.hpp"
#include "gui/pack_controller.hpp"

#include <QDebug>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QTimer>
#include <QTranslator>
#include <QUrl>
#include <QVariant>
#include <qcoreapplication.h>
#include <qhashfunctions.h>
#include <qlogging.h>
#include <qobject.h>
#include <qqml.h>

#include <cstdint>

int32_t main(int32_t argc, char* argv[])
{
    auto application = QGuiApplication(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("PackStacker"));
    auto translator = QTranslator{};
    if (!PackStacker::InstallInterfaceTranslation(translator, QLocale::system().uiLanguages()))
    {
        qCritical() << "Cannot load interface translations";
        return 1;
    }
    qmlRegisterUncreatableType<PackStacker::PackController>("PackStacker", 1, 0, "PackController",
                                                            "Created by the application");
    auto controller = PackStacker::PackController{};
    auto warnings = false;
    auto engine = QQmlApplicationEngine{};
    QObject::connect(&engine, &QQmlEngine::warnings, &application,
                     [&warnings] { warnings = true; });
    engine.setInitialProperties({{"controller", QVariant::fromValue(&controller)}});
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/PackStacker/Main.qml")));
    if (engine.rootObjects().isEmpty())
    {
        return 1;
    }
    QTimer::singleShot(250, &application, [&warnings] { QGuiApplication::exit(warnings ? 1 : 0); });
    return QGuiApplication::exec();
}
