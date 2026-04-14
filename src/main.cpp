#include <QApplication>
#include <QTranslator>

#include "gui/main_window.hpp"

static inline constexpr char TRANSLATION_PREFIX[] = ":/i18n";

qint32 main(qint32 argc, char* argv[])
{
    const auto a = QApplication(argc, argv);

    QTranslator translator;
    const auto uiLanguages = QLocale::system().uiLanguages();
    for (const auto& locale : uiLanguages)
    {
        const auto baseName = QString("%1_%2").arg(MainWindow::APP_NAME, QLocale(locale).name());
        if (translator.load(QString("%1/%2").arg(TRANSLATION_PREFIX, baseName)))
        {
            a.installTranslator(&translator);
            break;
        }
    }
    MainWindow w;
    w.show();
    return a.exec();
}
