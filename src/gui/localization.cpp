#include "gui/localization.hpp"

#include <QCoreApplication>
#include <QLocale>
#include <qcontainerfwd.h>
#include <qhashfunctions.h>

namespace PackStacker
{
    QString InterfaceLanguage(const QStringList& preferences)
    {
        for (const auto& preference : preferences)
        {
            const auto language = QLocale(preference).language();
            if (language == QLocale::Russian)
            {
                return QStringLiteral("ru");
            }
            if (language == QLocale::English)
            {
                return QStringLiteral("en");
            }
        }
        return QStringLiteral("en");
    }

    bool InstallInterfaceTranslation(QTranslator& translator, const QStringList& preferences)
    {
        const auto language = InterfaceLanguage(preferences);
        return translator.load(QStringLiteral(":/i18n/packstacker_") + language + ".qm") &&
               QCoreApplication::installTranslator(&translator);
    }
}
