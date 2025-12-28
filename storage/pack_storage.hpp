#pragma once

#include <QAbstractListModel>
#include <QIcon>
#include <QString>
#include <QDir>

static inline constexpr char PACK_META_FILENAME[] = "pack.mcmeta";
static inline constexpr char PACK_ICON_FILENAME[] = "pack.png";
static inline constexpr char PACK_FILE_EXTENSION[] = "zip";

struct PackInfo
{
    public:
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
        QVariant data(const QModelIndex& index, const qint32 role = Qt::DisplayRole) const override;

        void addPack(const PackInfo& pack);
        PackInfo takePack(const qint32 row);
        const QList<PackInfo>& getAll() const;

        void setRootPath(const QString& path);
        QString rootPath() const;

        void clear();

    signals:
        void rootPathChanged(const QString& newRoot);

    private:
        QList<PackInfo> data_;
        QString rootPath_;

        void scanRoot_();

        std::optional<PackInfo> loadPackInfo_(const QFileInfo& dirInfo) const;
        std::optional<PackInfo> loadPackInfoFromDir_(const QFileInfo& dirInfo) const;
        std::optional<PackInfo> loadPackInfoFromZip_(const QFileInfo& dirInfo) const;
};
