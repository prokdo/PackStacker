#include "core/version.hpp"
#include "gui/localization.hpp"
#include "gui/pack_controller.hpp"

#include <QDebug>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <qhashfunctions.h>
#include <qlogging.h>
#include <qnamespace.h>
#include <qobject.h>
#include <qtypes.h>

qint32 main(qint32 argc, char* argv[])
{
    auto application = QGuiApplication(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("PackStacker"));
    QCoreApplication::setApplicationVersion(QString::fromLatin1(PackStacker::BuildVersion));
    auto translator = QTranslator{};
    if (!PackStacker::InstallInterfaceTranslation(translator, QLocale::system().uiLanguages()))
    {
        qCritical() << "Cannot load interface translations";
        return 1;
    }
    auto controller = PackStacker::PackController{};
    auto engine = QQmlApplicationEngine{};
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &application,
        [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.setInitialProperties({{"controller", QVariant::fromValue(&controller)}});
    engine.loadFromModule("PackStacker", "Main");
    if (engine.rootObjects().isEmpty())
    {
        return 1;
    }
    return QGuiApplication::exec();
}
