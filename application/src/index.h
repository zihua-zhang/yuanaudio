#pragma once
#include <QByteArray>
#include <QList>
#include <QSet>
#include <QSqlDatabase>
#include <QString>
#include <QUrl>
#include <QVariant>
#include <functional>
#include <optional>

struct Library {
    QString id, name, root, sharedCache;
    QString sharedId = {};
};
struct Track {
    qint64 id = 0;
    QString libraryId, relativePath, name, folder, root, tags;
    qint64 size = 0, modified = 0, duration = 0;
    bool favorite = false;
    // Indexed identity remains the original. These fields describe only a separately
    // verified, optional compatible copy used by audio consumers.
    QString compatiblePath;
    qint64 compatibleSize = 0, compatibleModified = 0;
    QByteArray compatibleSha256;
    QString absolutePath() const;
    QString sourcePath() const;
    // Cheap compatible-copy check. Does not probe SMB originals or rehash content;
    // audio consumers perform the full hash check off the UI thread before use.
    bool sourceMatches(QString *error = nullptr) const;
};
struct SearchPage {
    QList<Track> tracks;
    bool more = false;
    bool cancelled = false;
    qint64 elapsedMs = 0, generation = 0;
    QString error;
};
struct FolderRecord {
    QString libraryId, path, parent, name, libraryName;
    bool hasChildren = false;
};
struct FolderPage {
    QList<FolderRecord> folders;
    bool more = false;
    qint64 elapsedMs = 0;
    QString error;
};
struct CatalogExclusions {
    QSet<QString> folders, tracks;
    bool excludesFolder(const QString &relativePath) const;
    bool excludesTrack(const QString &relativePath) const;
};
struct CatalogExcludedItem {
    QString path;
    bool isFolder = false;
};
class Index {
  public:
    explicit Index(const QString &path);
    ~Index();
    Index(const Index &) = delete;
    bool open(QString *error = nullptr);
    bool saveLibrary(const Library &library, bool restoreRemoved = true);
    // Catalog-only changes: never remove or modify original audio/shared cache files.
    bool removeLibrary(const QString &libraryId, QString *error = nullptr);
    bool excludeFolder(const QString &libraryId, const QString &relativePath, QString *error = nullptr);
    bool excludeTrack(const QString &libraryId, const QString &relativePath, QString *error = nullptr);
    QList<CatalogExcludedItem> excludedItems(const QString &libraryId, QString *error = nullptr) const;
    bool restoreExcludedItem(const QString &libraryId, const QString &relativePath, bool isFolder,
                             QString *error = nullptr);
    std::optional<CatalogExclusions> exclusions(const QString &libraryId, QString *error = nullptr) const;
    bool attachSharedCache(const QString &libraryId, const QString &cache, const QString &sharedId,
                           QString *error = nullptr);
    QList<Library> libraries() const;
    std::optional<Track> trackByIdentity(const QString &libraryId, const QString &relativePath) const;
    bool setCompatibleSource(const Track &original, const QString &path, const QByteArray &sha256,
                             QString *error = nullptr);
    bool upsert(const QList<Track> &tracks, QString *error = nullptr, bool *filesChanged = nullptr);
    SearchPage search(const QString &query, const QString &libraryId = {}, const QString &folder = {},
                      bool favorites = false, int offset = 0, int limit = 50,
                      bool includeSubfolders = true) const;
    // One consistent, sorted metadata snapshot. Cancellation is cooperative between row reads;
    // SQLite may finish its current exec()/sort before the callback can be checked again.
    SearchPage searchAll(const QString &query, const QString &libraryId = {}, const QString &folder = {},
                         bool favorites = false, bool includeSubfolders = true,
                         const std::function<bool()> &cancelled = {}) const;
    bool setPersonal(qint64 id, bool favorite, const QString &tags);
    bool setPersonal(const Track &version, bool favorite, const QString &tags);
    bool setDuration(const Track &version, qint64 milliseconds);
    qint64 count() const;
    qint64 countForLibrary(const QString &libraryId) const;
    QList<Track> tracksAfter(qint64 id, int limit = 200) const;
    QStringList folders(const QString &libraryId) const;
    bool upsertFolders(const QString &libraryId, const QStringList &paths, QString *error = nullptr);
    FolderPage findFolders(const QString &query, int offset = 0, int limit = 50) const;
    FolderPage childFolders(const QString &libraryId, const QString &parent, int offset = 0,
                            int limit = 100) const;
    FolderPage folderAncestors(const QString &libraryId, const QString &path) const;
    static bool importSnapshot(const QString &source, const QString &target, QString *error);
    QString lastError() const;

  private:
    QString path_, connection_;
    QSqlDatabase db_;
    mutable QString error_;
    SearchPage readSearch(const QString &query, const QString &libraryId, const QString &folder,
                          bool favorites, int offset, int limit, bool includeSubfolders,
                          const std::function<bool()> &cancelled) const;
    bool writeFolders(const QString &libraryId, const QStringList &paths,
                      const CatalogExclusions &exclusions = {});
    bool upgradeFolders(QString *error);
    bool upgradeSharedIdentity(QString *error, bool makeBackup = false);
    bool upgradeCatalogExclusions(QString *error);
    bool upgradeCompatibleSources(QString *error);
    bool changeCatalog(const QString &libraryId, const QString &relativePath, const QString &kind,
                       QString *error);
    FolderPage readFolders(const QString &where, const QList<QVariant> &binds, const QString &order,
                           int offset, int limit) const;
};
QString normalizeSearch(const QString &text);
QString tokenText(const QString &text);
QList<QUrl> originalFileUrls(const QList<Track> &tracks);
