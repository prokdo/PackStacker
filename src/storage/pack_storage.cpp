#include "storage/pack_storage.hpp"

PackStorage::PackStorage(QObject* parent) : QAbstractListModel(parent) {}

qint32 PackStorage::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
    {
        return 0;
    }

    return _data.count();
}

QVariant PackStorage::data(const QModelIndex& index, qint32 role) const
{
    if (!index.isValid() || index.row() >= _data.count())
    {
        return QVariant();
    }

    const auto& pack = _data.at(index.row());
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
        case Qt::SizeHintRole:
            return QSize(0, _PACK_ICON_DIMS);
        default:
            return QVariant();
    }
}


void PackStorage::addPack(const PackInfo& pack)
{
    beginInsertRows(QModelIndex(), _data.count(), _data.count());
    _data.append(pack);
    endInsertRows();
}

std::optional<PackInfo> PackStorage::takePack(qint32 row)
{
    if (row < 0 || row >= _data.count())
    {
        return std::nullopt;
    }

    beginRemoveRows(QModelIndex(), row, row);
    const auto pack = _data.takeAt(row);
    endRemoveRows();

    return pack;
}

const QList<PackInfo>& PackStorage::packs() const
{
    return _data;
}

void PackStorage::setRootPath(const QString& path)
{
    if (_rootPath == path)
    {
        return;
    }

    const auto dir = QDir(path);
    if (path.isEmpty() || !dir.exists() || !dir.isReadable())
    {
        clear();
        _rootPath = QString();
        return;
    }

    _rootPath = path;
    emit rootPathChanged(_rootPath);
    _scanRoot();
}

QString PackStorage::rootPath() const
{
    return _rootPath;
}

void PackStorage::clear()
{
    beginResetModel();
    _clearData();
    endResetModel();
}

void PackStorage::_scanRoot()
{
    beginResetModel();
    _clearData();

    auto dir = QDir(_rootPath);
    dir.setFilter(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable);

    const auto list = dir.entryInfoList();

    for (const auto& entry : list)
    {
        if (const auto opt = _loadPackInfo(entry); opt.has_value())
        {
            _data.append(opt.value());
        }
    }
    endResetModel();
}

void PackStorage::_clearData()
{
    _data.clear();
}

std::optional<PackInfo> PackStorage::_loadPackInfo(const QFileInfo& dirInfo) const
{
    if (dirInfo.isDir())
    {
        return _loadPackInfoFromDir(dirInfo);
    }

    if (dirInfo.isFile() && dirInfo.suffix() == _PACK_FILE_EXTENSION)
    {
        return _loadPackInfoFromZip(dirInfo);
    }

    return std::nullopt;
}

std::optional<PackInfo> PackStorage::_loadPackInfoFromDir(const QFileInfo& dirInfo) const
{
    // TODO
    Q_UNUSED(dirInfo);
    return std::nullopt;
}

std::optional<PackInfo> PackStorage::_loadPackInfoFromZip(const QFileInfo& dirInfo) const
{
    // TODO
    Q_UNUSED(dirInfo);
    return std::nullopt;
}
