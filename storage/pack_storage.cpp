#include "pack_storage.hpp"

PackStorage::PackStorage(QObject* parent) : QAbstractListModel(parent) {}

qint32 PackStorage::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
    {
        return 0;
    }

    return data_.count();
}

QVariant PackStorage::data(const QModelIndex& index, const qint32 role) const
{
    if (!index.isValid() || index.row() >= data_.count())
    {
        return QVariant();
    }

    const auto& pack = data_.at(index.row());
    switch (role)
    {
        case Qt::DecorationRole:
            return pack.icon;
        case Qt::DisplayRole:
            return pack.name;
        case DescriptionRole:
            return pack.description;
        case Qt::ToolTipRole:
            return pack.description;
        case VersionRole:
            return pack.version;
        default:
            return QVariant();
    }
}


void PackStorage::addPack(const PackInfo& pack)
{
    beginInsertRows(QModelIndex(), data_.count(), data_.count());
    data_.append(pack);
    endInsertRows();
}

PackInfo PackStorage::takePack(const qint32 row)
{
    if (row < 0 || row >= data_.count())
    {
        return {};
    }

    beginRemoveRows(QModelIndex(), row, row);
    auto pack = data_.takeAt(row);
    endRemoveRows();

    return pack;
}

const QList<PackInfo>& PackStorage::getAll() const
{
    return data_;
}

void PackStorage::setRootPath(const QString& path)
{
    if (rootPath_ == path)
    {
        return;
    }

    QDir dir(path);
    if (path.isEmpty() || !dir.exists() || !dir.isReadable())
    {
        clear();
        rootPath_ = QString();
        return;
    }

    rootPath_ = path;
    emit rootPathChanged(rootPath_);
    scanRoot_();
}

QString PackStorage::rootPath() const
{
    return rootPath_;
}

void PackStorage::clear()
{
    beginResetModel();
    data_.clear();
    endResetModel();
}

void PackStorage::scanRoot_()
{
    beginResetModel();
    data_.clear();

    QDir dir(rootPath_);
    dir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);

    const auto list = dir.entryInfoList();

    for (const auto& entry : list)
    {
        const auto opt = loadPackInfo_(entry);
        if (opt.has_value())
        {
            data_.append(opt.value());
        }
    }
    endResetModel();
}

std::optional<PackInfo> PackStorage::loadPackInfo_(const QFileInfo& dirInfo) const
{
    if (dirInfo.isDir())
        return loadPackInfoFromDir_(dirInfo);

    if (dirInfo.isFile() && dirInfo.suffix() == PACK_FILE_EXTENSION)
    {
        return loadPackInfoFromZip_(dirInfo);
    }

    return std::nullopt;
}

std::optional<PackInfo> PackStorage::loadPackInfoFromDir_(const QFileInfo& dirInfo) const
{
    // TODO
    return std::nullopt;
}

std::optional<PackInfo> PackStorage::loadPackInfoFromZip_(const QFileInfo& dirInfo) const
{
    // TODO
    return std::nullopt;
}
