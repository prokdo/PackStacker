#pragma once

#include "core/model.hpp"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#include <stop_token>
#include <vector>

namespace PackStacker
{
    struct CatalogContent
    {
        bool HasAssets = false;
        bool HasData = false;
    };

    struct CatalogOverlay
    {
        FormatRange Formats;
        CatalogContent Content;
    };

    struct CatalogEntry
    {
        QString Path;
        QString Name;
        QString Description;
        QString OutputDescription;
        QString Storage;
        QString Format;
        QString AutoFormat;
        QString Size;
        QString Icon;
        QString Error;
        QString Warning;
        CatalogContent Content;
        std::vector<CatalogOverlay> Overlays;

        QVariantMap ToRow() const;
    };

    struct CatalogSnapshot
    {
        QStringList Available;
        QMap<QString, CatalogEntry> Details;
    };

    QString CatalogPathKey(const QString& path);
    CatalogEntry CatalogPlaceholder(const QString& path);
    Result<CatalogSnapshot> ReadCatalog(const QString& directory, const QStringList& selected,
                                        const std::stop_token& stop);
}
