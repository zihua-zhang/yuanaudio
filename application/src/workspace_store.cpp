#include "workspace_store.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {
QString snapshot(const Track &t) {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"id", t.id},
                                                       {"library", t.libraryId},
                                                       {"relative", t.relativePath},
                                                       {"name", t.name},
                                                       {"folder", t.folder},
                                                       {"root", t.root},
                                                       {"tags", t.tags},
                                                       {"size", t.size},
                                                       {"modified", t.modified},
                                                       {"duration", t.duration},
                                                       {"favorite", t.favorite}})
                                 .toJson(QJsonDocument::Compact));
}
Track fromSnapshot(const QString &value) {
    const auto o = QJsonDocument::fromJson(value.toUtf8()).object();
    Track t;
    t.id = o["id"].toInteger();
    t.libraryId = o["library"].toString();
    t.relativePath = o["relative"].toString();
    t.name = o["name"].toString();
    t.folder = o["folder"].toString();
    t.root = o["root"].toString();
    t.tags = o["tags"].toString();
    t.size = o["size"].toInteger();
    t.modified = o["modified"].toInteger();
    t.duration = o["duration"].toInteger();
    t.favorite = o["favorite"].toBool();
    return t;
}
bool relativePathValid(const QString &path, bool allowRoot = false) {
    return (allowRoot || !path.isEmpty()) && !QDir::isAbsolutePath(path) && !path.startsWith('/') &&
           !path.contains('\\') && !path.split('/').contains("..");
}
bool validTrack(const Track &t) {
    return !t.libraryId.isEmpty() && !t.root.isEmpty() && relativePathValid(t.relativePath);
}
bool execute(QSqlQuery &q, const QString &sql, const QList<QVariant> &values = {}) {
    if (!q.prepare(sql))
        return false;
    for (const auto &value : values)
        q.addBindValue(value);
    return q.exec();
}
QString listWriteError(const QSqlError &error) {
    return error.text().contains("UNIQUE constraint failed: candidate_lists.name")
               ? QString("已有同名候选清单，请换一个名称。")
               : error.text();
}
} // namespace

WorkspaceStore::WorkspaceStore(const QString &databasePath)
    : path_(databasePath), connection_("workspace-" + QUuid::createUuid().toString(QUuid::WithoutBraces)) {}
WorkspaceStore::~WorkspaceStore() {
    if (db_.isValid())
        db_.close();
    db_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection_);
}
bool WorkspaceStore::fail(const QString &message) const {
    error_ = message;
    return false;
}
bool WorkspaceStore::ready() const {
    error_.clear();
    return db_.isOpen() || fail("个人工作区暂不可用，请重新打开软件后重试。");
}
QString WorkspaceStore::lastError() const {
    return error_;
}
bool WorkspaceStore::open() {
    error_.clear();
    if (db_.isOpen())
        return true;
    if (!QDir().mkpath(QFileInfo(path_).absolutePath()))
        return fail("无法创建个人工作区目录。");
    if (!db_.isValid())
        db_ = QSqlDatabase::addDatabase("QSQLITE", connection_);
    db_.setDatabaseName(path_);
    db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=200");
    if (!db_.open())
        return fail(db_.lastError().text());
    bool initialized = false;
    auto closeOnFailure = qScopeGuard([&] {
        if (!initialized) {
            db_.rollback();
            db_.close();
        }
    });
    QSqlQuery q(db_);
    if (!q.exec("PRAGMA user_version") || !q.next())
        return fail(q.lastError().text());
    if (q.value(0).toInt() > 1)
        return fail("个人工作区版本较新，请使用新版软件打开。");
    q.finish();
    for (const auto &sql : {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL", "PRAGMA foreign_keys=ON"})
        if (!q.exec(sql))
            return fail(q.lastError().text());
    if (!db_.transaction())
        return fail(db_.lastError().text());
    const QStringList schema{
        "CREATE TABLE IF NOT EXISTS candidate_lists(id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "name TEXT NOT NULL UNIQUE COLLATE NOCASE)",
        "CREATE TABLE IF NOT EXISTS candidate_tracks(position INTEGER PRIMARY KEY AUTOINCREMENT, "
        "list_id INTEGER NOT NULL REFERENCES candidate_lists(id) ON DELETE CASCADE, library TEXT NOT NULL, "
        "relative TEXT NOT NULL, snapshot TEXT NOT NULL, UNIQUE(list_id,library,relative))",
        "CREATE TABLE IF NOT EXISTS recent_tracks(position INTEGER PRIMARY KEY AUTOINCREMENT, "
        "library TEXT NOT NULL, relative TEXT NOT NULL, snapshot TEXT NOT NULL, UNIQUE(library,relative))",
        "CREATE TABLE IF NOT EXISTS pinned_folders(position INTEGER PRIMARY KEY AUTOINCREMENT, "
        "library TEXT NOT NULL, path TEXT NOT NULL, name TEXT NOT NULL, root TEXT NOT NULL, "
        "UNIQUE(library,path))",
        "PRAGMA user_version=1"};
    for (const auto &sql : schema)
        if (!q.exec(sql))
            return fail(q.lastError().text());
    if (!db_.commit())
        return fail(db_.lastError().text());
    initialized = true;
    return true;
}
QList<CandidateList> WorkspaceStore::candidateLists() const {
    QList<CandidateList> result;
    if (!ready())
        return result;
    QSqlQuery q(db_);
    if (!q.exec("SELECT id,name FROM candidate_lists ORDER BY id")) {
        fail(q.lastError().text());
        return result;
    }
    while (q.next())
        result << CandidateList{q.value(0).toLongLong(), q.value(1).toString()};
    if (q.lastError().isValid()) {
        fail(q.lastError().text());
        result.clear();
    }
    return result;
}
qint64 WorkspaceStore::createList(const QString &name) {
    if (!ready())
        return 0;
    const auto clean = name.trimmed();
    if (clean.isEmpty() || clean.size() > 80) {
        fail("候选清单名称需要 1–80 个字符。");
        return 0;
    }
    QSqlQuery q(db_);
    if (!execute(q,
                 "INSERT INTO candidate_lists(name) SELECT ? WHERE "
                 "(SELECT COUNT(*) FROM candidate_lists) < ?",
                 {clean, ListLimit})) {
        fail(listWriteError(q.lastError()));
        return 0;
    }
    if (q.numRowsAffected() != 1) {
        fail(QString("最多保留 %1 个候选清单，请整理后再添加。").arg(ListLimit));
        return 0;
    }
    return q.lastInsertId().toLongLong();
}
bool WorkspaceStore::renameList(qint64 id, const QString &name) {
    if (!ready())
        return false;
    const auto clean = name.trimmed();
    if (clean.isEmpty() || clean.size() > 80)
        return fail("候选清单名称需要 1–80 个字符。");
    QSqlQuery q(db_);
    if (!execute(q, "UPDATE candidate_lists SET name=? WHERE id=?", {clean, id}))
        return fail(listWriteError(q.lastError()));
    return q.numRowsAffected() == 1 || fail("这个候选清单已不存在。");
}
bool WorkspaceStore::removeList(qint64 id) {
    if (!ready() || !db_.transaction())
        return fail(error_.isEmpty() ? db_.lastError().text() : error_);
    auto rollback = qScopeGuard([&] { db_.rollback(); });
    QSqlQuery q(db_);
    if (!execute(q, "DELETE FROM candidate_lists WHERE id=?", {id}))
        return fail(q.lastError().text());
    if (q.numRowsAffected() != 1)
        return fail("这个候选清单已不存在。");
    if (!db_.commit())
        return fail(db_.lastError().text());
    rollback.dismiss();
    return true;
}
QList<Track> WorkspaceStore::readTracks(const QString &sql, const QList<QVariant> &values) const {
    QList<Track> result;
    if (!ready())
        return result;
    QSqlQuery q(db_);
    if (!execute(q, sql, values)) {
        fail(q.lastError().text());
        return result;
    }
    while (q.next()) {
        const auto t = fromSnapshot(q.value(0).toString());
        if (validTrack(t))
            result << t;
    }
    if (q.lastError().isValid()) {
        fail(q.lastError().text());
        result.clear();
    }
    return result;
}
QList<Track> WorkspaceStore::candidates(qint64 listId) const {
    return readTracks("SELECT snapshot FROM candidate_tracks WHERE list_id=? ORDER BY position", {listId});
}
bool WorkspaceStore::addCandidate(qint64 listId, const Track &track) {
    if (!ready())
        return false;
    if (!validTrack(track))
        return fail("这首音乐没有可用的原文件位置。");
    QSqlQuery q(db_);
    if (!execute(q,
                 "INSERT INTO candidate_tracks(list_id,library,relative,snapshot) SELECT ?,?,?,? WHERE "
                 "(SELECT COUNT(*) FROM candidate_tracks WHERE list_id=?) < ? OR "
                 "EXISTS(SELECT 1 FROM candidate_tracks WHERE list_id=? AND library=? AND relative=?) "
                 "ON CONFLICT(list_id,library,relative) DO UPDATE SET snapshot=excluded.snapshot",
                 {listId, track.libraryId, track.relativePath, snapshot(track), listId, CandidateLimit,
                  listId, track.libraryId, track.relativePath}))
        return fail(q.lastError().text());
    return q.numRowsAffected() == 1 ||
           fail(QString("这个清单已达到 %1 首，请新建清单或移除不需要的候选。").arg(CandidateLimit));
}
bool WorkspaceStore::removeCandidate(qint64 listId, const QString &libraryId, const QString &relativePath) {
    if (!ready())
        return false;
    QSqlQuery q(db_);
    return execute(q, "DELETE FROM candidate_tracks WHERE list_id=? AND library=? AND relative=?",
                   {listId, libraryId, relativePath}) ||
           fail(q.lastError().text());
}
QList<Track> WorkspaceStore::recents() const {
    return readTracks("SELECT snapshot FROM recent_tracks ORDER BY position DESC LIMIT 100");
}
bool WorkspaceStore::recordAudition(const Track &track) {
    if (!ready())
        return false;
    if (!validTrack(track))
        return fail("这首音乐没有可用的原文件位置。");
    if (!db_.transaction())
        return fail(db_.lastError().text());
    auto rollback = qScopeGuard([&] { db_.rollback(); });
    QSqlQuery q(db_);
    if (!execute(q, "DELETE FROM recent_tracks WHERE library=? AND relative=?",
                 {track.libraryId, track.relativePath}) ||
        !execute(q, "INSERT INTO recent_tracks(library,relative,snapshot) VALUES(?,?,?)",
                 {track.libraryId, track.relativePath, snapshot(track)}) ||
        !q.exec("DELETE FROM recent_tracks WHERE position NOT IN "
                "(SELECT position FROM recent_tracks ORDER BY position DESC LIMIT 100)"))
        return fail(q.lastError().text());
    if (!db_.commit())
        return fail(db_.lastError().text());
    rollback.dismiss();
    return true;
}
bool WorkspaceStore::removeRecent(const QString &libraryId, const QString &relativePath) {
    if (!ready())
        return false;
    QSqlQuery q(db_);
    return execute(q, "DELETE FROM recent_tracks WHERE library=? AND relative=?",
                   {libraryId, relativePath}) ||
           fail(q.lastError().text());
}
QList<PinnedFolder> WorkspaceStore::pinnedFolders() const {
    QList<PinnedFolder> result;
    if (!ready())
        return result;
    QSqlQuery q(db_);
    if (!q.exec("SELECT library,path,name,root FROM pinned_folders ORDER BY position")) {
        fail(q.lastError().text());
        return result;
    }
    while (q.next())
        result << PinnedFolder{q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                               q.value(3).toString()};
    if (q.lastError().isValid()) {
        fail(q.lastError().text());
        result.clear();
    }
    return result;
}
bool WorkspaceStore::pinFolder(const PinnedFolder &folder) {
    if (!ready())
        return false;
    const auto path = folder.path == "." ? QString("") : folder.path;
    if (folder.libraryId.isEmpty() || !relativePathValid(path, true))
        return fail("这个文件夹没有可用的素材库位置。");
    QSqlQuery q(db_);
    if (!execute(q,
                 "INSERT INTO pinned_folders(library,path,name,root) SELECT ?,?,?,? WHERE "
                 "(SELECT COUNT(*) FROM pinned_folders) < ? OR "
                 "EXISTS(SELECT 1 FROM pinned_folders WHERE library=? AND path=?) "
                 "ON CONFLICT(library,path) DO UPDATE SET name=excluded.name,root=excluded.root",
                 {folder.libraryId, path, folder.name, folder.root.isNull() ? QString("") : folder.root,
                  FolderLimit, folder.libraryId, path}))
        return fail(q.lastError().text());
    return q.numRowsAffected() == 1 ||
           fail(QString("最多保留 %1 个常用目录，请整理后再添加。").arg(FolderLimit));
}
bool WorkspaceStore::unpinFolder(const QString &libraryId, const QString &path) {
    if (!ready())
        return false;
    QSqlQuery q(db_);
    return execute(q, "DELETE FROM pinned_folders WHERE library=? AND path=?",
                   {libraryId, path == "." ? QString("") : path}) ||
           fail(q.lastError().text());
}
