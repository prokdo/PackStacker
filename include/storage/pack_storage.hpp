#pragma once

#include <QAbstractListModel>
#include <QIcon>
#include <QString>
#include <QDir>

struct PackInfo
{
        QIcon icon;
        QString name;
        QString description;
        quint8 version;
        QString path;
};

class PackStorage : public QAbstractListModel
{
    Q_OBJECT

    public:
        explicit PackStorage(QObject* parent = nullptr);

        enum Role
        {
            DescriptionRole = Qt::UserRole + 1,
            VersionRole
        };

        qint32 rowCount(const QModelIndex& parent = QModelIndex()) const override;
        QVariant data(const QModelIndex& index, qint32 role = Qt::DisplayRole) const override;

        void addPack(const PackInfo& pack);
        std::optional<PackInfo> takePack(qint32 row);
        const QList<PackInfo>& packs() const;

        void setRootPath(const QString& path);
        QString rootPath() const;

        void clear();

    signals:
        void rootPathChanged(const QString& newRoot);

    private:
        static inline constexpr char _PACK_META_FILENAME[] = "pack.mcmeta";
        static inline constexpr char _PACK_ICON_FILENAME[] = "pack.png";
        static inline constexpr char _PACK_FILE_EXTENSION[] = "zip";
        static inline constexpr quint8 _PACK_ICON_DIMS = 48;

        QList<PackInfo> _data;
        QString _rootPath;

        void _scanRoot();
        void _clearData();

        std::optional<PackInfo> _loadPackInfo(const QFileInfo& dirInfo) const;
        std::optional<PackInfo> _loadPackInfoFromDir(const QFileInfo& dirInfo) const;
        std::optional<PackInfo> _loadPackInfoFromZip(const QFileInfo& dirInfo) const;
};
