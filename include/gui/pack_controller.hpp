#pragma once

#include "core/model.hpp"
#include "gui/pack_catalog.hpp"

#include <QList>
#include <QObject>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QVariantMap>
#include <QtClassHelperMacros>

#include <expected>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace PackStacker
{
    class PackController : public QObject
    {
        Q_OBJECT
        Q_PROPERTY(QStringList packs READ Packs NOTIFY PacksChanged)
        Q_PROPERTY(bool busy READ Busy NOTIFY StateChanged)
        Q_PROPERTY(bool cancelling READ Cancelling NOTIFY StateChanged)
        Q_PROPERTY(bool failed READ Failed NOTIFY StateChanged)
        Q_PROPERTY(QString status READ Status NOTIFY StateChanged)
        Q_PROPERTY(QString sourceDirectory READ SourceDirectory NOTIFY CatalogChanged)
        Q_PROPERTY(QVariantList availablePacks READ AvailablePacks NOTIFY CatalogChanged)
        Q_PROPERTY(QVariantList selectedPacks READ SelectedPacks NOTIFY CatalogChanged)
        Q_PROPERTY(bool scanning READ Scanning NOTIFY StateChanged)
        Q_PROPERTY(QString resultPath READ ResultPath NOTIFY StateChanged)
        Q_PROPERTY(QVariantList rules READ Rules NOTIFY RulesChanged)

    public:
        explicit PackController(QObject* parent = nullptr);
        ~PackController() override;

        QStringList Packs() const;
        bool Busy() const;
        bool Cancelling() const;
        bool Failed() const;
        QString Status() const;
        QString SourceDirectory() const;
        QVariantList AvailablePacks() const;
        QVariantList SelectedPacks() const;
        bool Scanning() const;
        QString ResultPath() const;
        QVariantList Rules() const;

        Q_INVOKABLE bool AddPack(const QUrl& url);
        Q_INVOKABLE bool AddPacks(const QList<QUrl>& urls);
        Q_INVOKABLE bool RemovePack(qint32 index);
        Q_INVOKABLE bool MovePack(qint32 sourceIndex, qint32 destinationIndex);
        Q_INVOKABLE bool AddAvailable(qint32 index);
        Q_INVOKABLE bool ClearPacks();
        Q_INVOKABLE bool SetSourceDirectory(const QUrl& url);
        Q_INVOKABLE bool RefreshCatalog();
        Q_INVOKABLE static QString LocalPath(const QUrl& url);
        Q_INVOKABLE static QString DefaultOutputPath(const QString& directory, qint32 outputFormat);
        Q_INVOKABLE static QVariantMap ValidateInputs(const QString& target, const QString& output);
        Q_INVOKABLE QString PackTypeWarning(const QString& path, qint32 type,
                                            const QString& target) const;
        Q_INVOKABLE bool OpenResultFolder();
        Q_INVOKABLE bool ApplyRules(const QVariantList& values);
        Q_INVOKABLE QVariantMap ImportRules(const QUrl& url);
        Q_INVOKABLE bool ExportRules(const QUrl& url, const QVariantList& values);
        Q_INVOKABLE bool Start(qint32 type, const QString& target, const QString& output,
                               qint32 outputFormat, const QString& description);
        Q_INVOKABLE void Cancel();

    signals:
        void PacksChanged();
        void StateChanged();
        void CatalogChanged();
        void CatalogFinished(bool success);
        void Finished(bool success);
        void RulesChanged();

    private:
        Q_DISABLE_COPY_MOVE(PackController)

        bool reject(const QString& message);
        bool beginScan(const QString& directory);
        void returnToCatalog(const QString& path);
        QStringList availablePaths() const;
        CatalogEntry catalogEntry(const QString& path) const;
        std::expected<std::vector<std::string>, QString> validatedPacks(
            qint32 type, const QString& target) const;

        QStringList packs;
        bool busy = false;
        bool failed = false;
        QString status;
        QString resultPath;
        std::stop_source stop;
        std::unique_ptr<QThread> worker;
        QString sourceDirectory;
        QStringList library;
        QMap<QString, CatalogEntry> details;
        bool scanning = false;
        std::stop_source scanStop;
        std::unique_ptr<QThread> scanner;
        std::vector<MergeRule> rules;
    };
}
