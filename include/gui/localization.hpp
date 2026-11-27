#pragma once

#include <QStringList>
#include <QTranslator>

namespace PackStacker
{
    QString InterfaceLanguage(const QStringList& preferences);
    bool InstallInterfaceTranslation(QTranslator& translator, const QStringList& preferences);
}
