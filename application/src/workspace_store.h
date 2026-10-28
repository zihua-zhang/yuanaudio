#pragma once
#include "index.h"
#include <QSqlDatabase>

struct CandidateList {
    qint64 id = 0;
    QString name;
};

struct PinnedFolder {
    QString libraryId, path, name, root;
};

// Personal references only. This database never reads or writes audio files.
class WorkspaceStore {
  public:
    explicit WorkspaceStore(const QString &databasePath);
    ~WorkspaceStore();
    WorkspaceStore(const WorkspaceStore &) = delete;
    WorkspaceStore &operator=(const WorkspaceStore &) = delete;
    bool open();
    QString lastError() const;
    QList<CandidateList> candidateLists() const;
    qint64 createList(const QString &name);
    bool renameList(qint64 id, const QString &name);
    bool removeList(qint64 id);
    QList<Track> candidates(qint64 listId) const;
    bool addCandidate(qint64 listId, const Track &track);
    bool removeCandidate(qint64 listId, const QString &libraryId, const QString &relativePath);
    QList<Track> recents() const;
    bool recordAudition(const Track &track);
    bool removeRecent(const QString &libraryId, const QString &relativePath);
    QList<PinnedFolder> pinnedFolders() const;
    bool pinFolder(const PinnedFolder &folder);
    bool unpinFolder(const QString &libraryId, const QString &path);
    static constexpr int RecentLimit = 100;
    static constexpr int CandidateLimit = 2000;
    static constexpr int ListLimit = 100;
    static constexpr int FolderLimit = 200;

  private:
    QString path_, connection_;
    QSqlDatabase db_;
    mutable QString error_;
    bool fail(const QString &message) const;
    bool ready() const;
    QList<Track> readTracks(const QString &sql, const QList<QVariant> &values = {}) const;
};
