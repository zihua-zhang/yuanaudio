#include "index.h"
#include "file_publication.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>

QString normalizeSearch(const QString &text) {
    return text.normalized(QString::NormalizationForm_KC).toCaseFolded();
}
static QString unicodeToken(char32_t ch) {
    return QString::number(uint(ch), 16);
}
QString tokenText(const QString &text) {
    const auto chars = normalizeSearch(text).toUcs4();
    QSet<QString> tokens;
    for (qsizetype i = 0; i < chars.size(); ++i) {
        tokens.insert("u" + unicodeToken(chars[i]));
        if (i + 1 < chars.size())
            tokens.insert("b" + unicodeToken(chars[i]) + "x" + unicodeToken(chars[i + 1]));
    }
    auto sorted = tokens.values();
    sorted.sort();
    return sorted.join(' ');
}
static QString matchExpression(const QStringList &terms) {
    QStringList tokens;
    for (const auto &term : terms) {
        const auto chars = term.toUcs4();
        if (chars.size() == 1)
            tokens << "\"u" + unicodeToken(chars[0]) + "\"";
        else
            for (qsizetype i = 0; i + 1 < chars.size(); ++i)
                tokens << "\"b" + unicodeToken(chars[i]) + "x" + unicodeToken(chars[i + 1]) + "\"";
    }
    tokens.removeDuplicates();
    return tokens.join(" AND ");
}
Index::Index(const QString &path)
    : path_(path), connection_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}
Index::~Index() {
    if (db_.isValid())
        db_.close();
    db_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connection_);
}
bool Index::open(QString *error) {
    QDir().mkpath(QFileInfo(path_).absolutePath());
    db_ = QSqlDatabase::addDatabase("QSQLITE", connection_);
    db_.setDatabaseName(path_);
    db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=3000");
    auto fail = [&](const QString &s) {
        error_ = s;
        if (error)
            *error = s;
        return false;
    };
    if (!db_.open())
        return fail(db_.lastError().text());
    QSqlQuery q(db_);
    if (!q.exec("PRAGMA user_version") || !q.next())
        return fail(q.lastError().text());
    const int version = q.value(0).toInt();
    q.finish();
    if (version > 6)
        return fail("索引版本高于当前软件支持的版本，请使用较新版本。");
    if (version >= 2) {
        if (!q.exec("PRAGMA foreign_keys=ON"))
            return fail(q.lastError().text());
        if (version == 6)
            return true;
        if (version >= 4)
            return upgradeCatalogExclusions(error);
        if (version == 2 && !upgradeFolders(error))
            return false;
        return upgradeSharedIdentity(error, version == 3) && upgradeCatalogExclusions(error);
    }
    const QStringList schema = {
        "PRAGMA journal_mode=WAL",
        "PRAGMA synchronous=NORMAL",
        "PRAGMA foreign_keys=ON",
        "CREATE TABLE IF NOT EXISTS libraries(id TEXT PRIMARY KEY, name TEXT NOT NULL, root TEXT NOT NULL, "
        "cache TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS tracks(id INTEGER PRIMARY KEY, library TEXT NOT NULL REFERENCES "
        "libraries(id), relative TEXT NOT NULL, name TEXT NOT NULL, folder TEXT NOT NULL, size INTEGER NOT "
        "NULL, modified INTEGER NOT NULL, duration INTEGER NOT NULL DEFAULT 0, favorite INTEGER NOT NULL "
        "DEFAULT 0, tags TEXT NOT NULL DEFAULT '', search_text TEXT NOT NULL, name_norm TEXT NOT NULL, "
        "UNIQUE(library,relative))",
        "CREATE INDEX IF NOT EXISTS tracks_library ON tracks(library,folder,name,id)",
        "CREATE INDEX IF NOT EXISTS tracks_order ON tracks(name_norm,id)",
        "CREATE VIRTUAL TABLE IF NOT EXISTS search_fts USING fts5(tokens,tokenize='ascii')",
        "CREATE TABLE IF NOT EXISTS index_state(id INTEGER PRIMARY KEY CHECK(id=1), generation INTEGER NOT "
        "NULL)",
        "INSERT OR IGNORE INTO index_state(id,generation) VALUES(1,0)",
        "PRAGMA user_version=2"};
    for (const auto &sql : schema)
        if (!q.exec(sql))
            return fail(q.lastError().text());
    return upgradeFolders(error) && upgradeSharedIdentity(error) && upgradeCatalogExclusions(error);
}
bool Index::upgradeSharedIdentity(QString *error, bool makeBackup) {
    QSqlQuery q(db_);
    auto fail = [&](const QString &message) {
        db_.rollback();
        error_ = message;
        if (error)
            *error = message;
        return false;
    };
    if (makeBackup) {
        if (!q.exec("PRAGMA user_version") || !q.next())
            return fail(q.lastError().text());
        const bool alreadyUpgraded = q.value(0).toInt() >= 4;
        q.finish();
        if (alreadyUpgraded)
            return true;
        const auto info = QFileInfo(path_);
        const auto backup = info.dir().filePath(info.completeBaseName() + ".pre-0.8.sqlite");
        auto validBackup = [&](const QString &candidate) {
            const auto connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
            bool valid = false;
            {
                auto snapshot = QSqlDatabase::addDatabase("QSQLITE", connection);
                snapshot.setDatabaseName(candidate);
                snapshot.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=3000");
                if (snapshot.open()) {
                    QSqlQuery check(snapshot);
                    valid =
                        check.exec("PRAGMA quick_check") && check.next() && check.value(0).toString() == "ok";
                    valid = valid && check.exec("PRAGMA user_version") && check.next() &&
                            check.value(0).toInt() == 3;
                    valid = valid && check.exec("SELECT id,root FROM libraries ORDER BY id");
                    QList<QPair<QString, QString>> expected, actual;
                    if (valid) {
                        while (check.next())
                            actual << qMakePair(check.value(0).toString(), check.value(1).toString());
                        QSqlQuery current(db_);
                        valid = !check.lastError().isValid() &&
                                current.exec("SELECT id,root FROM libraries ORDER BY id");
                        while (valid && current.next())
                            expected << qMakePair(current.value(0).toString(), current.value(1).toString());
                        valid = valid && !current.lastError().isValid() && actual == expected;
                    }
                }
            }
            QSqlDatabase::removeDatabase(connection);
            return valid;
        };
        if (QFileInfo::exists(backup)) {
            if (!validBackup(backup))
                return fail("升级前备份已存在但校验未通过，已停止升级并保留原索引与备份文件。");
        } else {
            const auto stage = backup + ".pending-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            q.prepare("VACUUM INTO ?");
            q.addBindValue(stage);
            if (!q.exec()) {
                const auto message = q.lastError().text();
                QFile::remove(stage);
                return fail("无法创建升级前备份，原索引未升级：" + message);
            }
            q.finish();
            // Another opener may have published the backup and migrated while our snapshot ran.
            const bool existingValid = QFileInfo::exists(backup) && validBackup(backup);
            const bool stagedValid = existingValid || validBackup(stage);
            const bool published = existingValid || (stagedValid && publishFileExclusive(stage, backup));
            bool racedWinner = false;
            QElapsedTimer publicationWait;
            publicationWait.start();
            while (!published && !racedWinner && publicationWait.elapsed() < 1000) {
                racedWinner = QFileInfo::exists(backup) && validBackup(backup);
                if (!racedWinner)
                    QThread::msleep(25);
            }
            QFile::remove(stage);
            if (!published && !racedWinner)
                return fail("升级前备份无法安全保存或校验失败，原索引未升级。");
        }
    }
    // Serializes first-open migration across the UI and worker processes without rebuilding tracks.
    if (!q.exec("BEGIN IMMEDIATE"))
        return fail(q.lastError().text());
    if (!q.exec("PRAGMA user_version") || !q.next())
        return fail(q.lastError().text());
    const bool migratedByPeer = q.value(0).toInt() >= 4;
    q.finish();
    if (migratedByPeer)
        return db_.commit() || fail(db_.lastError().text());
    if (!q.exec("PRAGMA table_info(libraries)"))
        return fail(q.lastError().text());
    bool hasSharedId = false;
    while (q.next())
        hasSharedId |= q.value(1).toString() == "shared_id";
    q.finish();
    if (!hasSharedId && !q.exec("ALTER TABLE libraries ADD COLUMN shared_id TEXT NOT NULL DEFAULT ''"))
        return fail(q.lastError().text());
    if (!q.exec("UPDATE libraries SET shared_id=id WHERE cache<>'' AND shared_id=''"))
        return fail(q.lastError().text());
    if (!q.exec("PRAGMA user_version=4"))
        return fail(q.lastError().text());
    if (!db_.commit())
        return fail(db_.lastError().text());
    return true;
}
bool Index::upgradeCatalogExclusions(QString *error) {
    QSqlQuery q(db_);
    auto fail = [&](const QString &message) {
        db_.rollback();
        error_ = message;
        if (error)
            *error = message;
        return false;
    };
    if (!q.exec("BEGIN IMMEDIATE"))
        return fail(q.lastError().text());
    // Another opener may have completed migration while this connection waited.
    if (!q.exec("PRAGMA user_version") || !q.next())
        return fail(q.lastError().text());
    const auto currentVersion = q.value(0).toInt();
    q.finish();
    if (currentVersion > 6)
        return fail("索引版本高于当前软件支持的版本，请使用较新版本。");
    if (currentVersion == 6)
        return db_.commit() || fail(db_.lastError().text());
    const QStringList statements{
        "CREATE TABLE IF NOT EXISTS catalog_exclusions(library TEXT NOT NULL REFERENCES libraries(id), "
        "path TEXT NOT NULL, kind TEXT NOT NULL CHECK(kind IN ('folder','track')), "
        "PRIMARY KEY(library,kind,path))",
        "CREATE TABLE IF NOT EXISTS removed_libraries(id TEXT PRIMARY KEY)",
        "CREATE TABLE IF NOT EXISTS excluded_track_metadata("
        "library TEXT NOT NULL REFERENCES libraries(id), relative TEXT NOT NULL, original_id INTEGER NOT NULL, "
        "size INTEGER NOT NULL, modified INTEGER NOT NULL, duration INTEGER NOT NULL, "
        "favorite INTEGER NOT NULL, tags TEXT NOT NULL, PRIMARY KEY(library,relative))",
        "PRAGMA user_version=6"};
    for (const auto &sql : statements)
        if (!q.exec(sql))
            return fail(q.lastError().text());
    if (db_.commit())
        return true;
    return fail(db_.lastError().text());
}
bool CatalogExclusions::excludesFolder(const QString &relativePath) const {
    auto path = relativePath;
    while (!path.isEmpty() && path != ".") {
        if (folders.contains(path))
            return true;
        const auto slash = path.lastIndexOf('/');
        if (slash < 0)
            break;
        path.truncate(slash);
    }
    return false;
}
bool CatalogExclusions::excludesTrack(const QString &relativePath) const {
    return tracks.contains(relativePath) || excludesFolder(relativePath);
}
std::optional<CatalogExclusions> Index::exclusions(const QString &library, QString *error) const {
    QSqlQuery q(db_);
    q.prepare("SELECT path,kind FROM catalog_exclusions WHERE library=?");
    q.addBindValue(library);
    CatalogExclusions result;
    if (q.exec()) {
        while (q.next()) {
            auto &paths = q.value(1).toString() == "folder" ? result.folders : result.tracks;
            paths.insert(q.value(0).toString());
        }
        if (!q.lastError().isValid())
            return result;
    }
    error_ = q.lastError().text();
    if (error)
        *error = error_;
    return std::nullopt;
}
static bool validCatalogPath(const QString &path) {
    return !path.isEmpty() && path != "." && !QDir::isAbsolutePath(path) && !path.contains(':') &&
           !path.contains('\\') && !path.contains(QChar::Null) && !path.endsWith('/') &&
           !path.contains("//") && !path.split('/').contains(".") && !path.split('/').contains("..");
}
bool Index::removeLibrary(const QString &library, QString *error) {
    return changeCatalog(library, {}, "library", error);
}
bool Index::excludeFolder(const QString &library, const QString &path, QString *error) {
    return changeCatalog(library, path, "folder", error);
}
bool Index::excludeTrack(const QString &library, const QString &path, QString *error) {
    return changeCatalog(library, path, "track", error);
}
QList<CatalogExcludedItem> Index::excludedItems(const QString &library, QString *error) const {
    if (error)
        error->clear();
    const auto rules = exclusions(library, error);
    if (!rules)
        return {};
    QList<CatalogExcludedItem> result;
    auto folders = rules->folders.values(), tracks = rules->tracks.values();
    folders.sort();
    tracks.sort();
    for (const auto &path : folders)
        result << CatalogExcludedItem{path, true};
    for (const auto &path : tracks)
        result << CatalogExcludedItem{path, false};
    return result;
}
bool Index::restoreExcludedItem(const QString &library, const QString &path, bool isFolder,
                                QString *error) {
    auto fail = [&](const QString &message) {
        db_.rollback();
        error_ = message;
        if (error)
            *error = message;
        return false;
    };
    if (library.isEmpty() || !validCatalogPath(path))
        return fail("素材库或相对路径无效，排除记录保持不变。");
    QSqlQuery q(db_);
    if (!q.exec("BEGIN IMMEDIATE"))
        return fail(q.lastError().text());
    q.prepare("SELECT id FROM libraries WHERE id=?");
    q.addBindValue(library);
    if (!q.exec())
        return fail(q.lastError().text());
    if (!q.next())
        return fail("素材库已不存在，排除记录保持不变。");
    q.finish();
    q.prepare("DELETE FROM catalog_exclusions WHERE library=? AND path=? AND kind=?");
    q.addBindValue(library);
    q.addBindValue(path);
    q.addBindValue(isFolder ? "folder" : "track");
    if (!q.exec())
        return fail(q.lastError().text());
    if (q.numRowsAffected() > 0 &&
        !q.exec("UPDATE index_state SET generation=generation+1 WHERE id=1"))
        return fail(q.lastError().text());
    if (!db_.commit())
        return fail(db_.lastError().text());
    error_.clear();
    if (error)
        error->clear();
    return true;
}
bool Index::changeCatalog(const QString &library, const QString &path, const QString &kind,
                          QString *error) {
    auto fail = [&](const QString &message) {
        db_.rollback();
        error_ = message;
        if (error)
            *error = message;
        return false;
    };
    if (library.isEmpty() || (kind != "library" && !validCatalogPath(path)))
        return fail("素材库或相对路径无效，目录保持不变。");
    QSqlQuery q(db_);
    if (!q.exec("BEGIN IMMEDIATE"))
        return fail(q.lastError().text());
    q.prepare("SELECT id FROM libraries WHERE id=?");
    q.addBindValue(library);
    if (!q.exec())
        return fail(q.lastError().text());
    if (!q.next())
        return fail("素材库已不存在，目录保持不变。");
    q.finish();
    auto execute = [&](const QString &sql, const QList<QVariant> &values) {
        q.prepare(sql);
        for (const auto &value : values)
            q.addBindValue(value);
        if (q.exec())
            return true;
        error_ = q.lastError().text();
        return false;
    };
    QString trackScope = "library=?", folderScope = "library=?";
    QList<QVariant> trackValues{library}, folderValues{library};
    if (kind != "library") {
        if (!execute("INSERT OR IGNORE INTO catalog_exclusions(library,path,kind) VALUES(?,?,?)",
                     {library, path, kind}))
            return fail(error_);
        if (kind == "track") {
            trackScope += " AND relative=?";
            trackValues << path;
        } else {
            // Prefix boundaries are literal: %, _ and same-prefix siblings are not wildcards.
            trackScope += " AND substr(relative,1,length(?))=?";
            trackValues << path + '/' << path + '/';
            folderScope += " AND (path=? OR substr(path,1,length(?))=?)";
            folderValues << path << path + '/' << path + '/';
        }
    }
    // Keep personal data independent of list visibility. The archive and removal share
    // one transaction, so a failed removal cannot leave a misleading recovery record.
    if (kind != "library" &&
        !execute("INSERT OR REPLACE INTO excluded_track_metadata "
                 "(library,relative,original_id,size,modified,duration,favorite,tags) "
                 "SELECT library,relative,id,size,modified,duration,favorite,tags FROM tracks WHERE " + trackScope,
                 trackValues))
        return fail(error_);
    if (!execute("DELETE FROM search_fts WHERE rowid IN (SELECT id FROM tracks WHERE " + trackScope + ")",
                 trackValues) ||
        !execute("DELETE FROM tracks WHERE " + trackScope, trackValues))
        return fail(error_);
    if (kind != "track" &&
        (!execute("DELETE FROM folder_fts WHERE rowid IN (SELECT id FROM folders WHERE " + folderScope + ")",
                  folderValues) ||
         !execute("DELETE FROM folders WHERE " + folderScope, folderValues)))
        return fail(error_);
    if (kind == "library" &&
        (!execute("DELETE FROM excluded_track_metadata WHERE library=?", {library}) ||
         !execute("DELETE FROM catalog_exclusions WHERE library=?", {library}) ||
         !execute("DELETE FROM libraries WHERE id=?", {library}) ||
         !execute("INSERT OR IGNORE INTO removed_libraries(id) VALUES(?)", {library})))
        return fail(error_);
    if (!q.exec("UPDATE index_state SET generation=generation+1 WHERE id=1"))
        return fail(q.lastError().text());
    if (!db_.commit())
        return fail(db_.lastError().text());
    error_.clear();
    return true;
}
bool Index::saveLibrary(const Library &l, bool restoreRemoved) {
    auto fail = [&](const QString &message) {
        db_.rollback();
        error_ = message;
        return false;
    };
    QSqlQuery transaction(db_);
    if (!transaction.exec("BEGIN IMMEDIATE"))
        return fail(transaction.lastError().text());
    if (!restoreRemoved) {
        transaction.prepare("SELECT id FROM removed_libraries WHERE id=?");
        transaction.addBindValue(l.id);
        if (!transaction.exec())
            return fail(transaction.lastError().text());
        if (transaction.next())
            return fail("素材库已从本机目录移除；请重新添加后再扫描。");
        transaction.finish();
    }
    QSqlQuery existing(db_);
    existing.prepare("SELECT root FROM libraries WHERE id=?");
    existing.addBindValue(l.id);
    if (!existing.exec()) {
        return fail(existing.lastError().text());
    }
    if (existing.next() && QDir::cleanPath(existing.value(0).toString()) != QDir::cleanPath(l.root)) {
        return fail("这个共享缓存已对应另一个音乐目录，请为不同的库选择独立缓存。原路径已保留。");
    }
    existing.finish();
    QSqlQuery q(db_);
    q.prepare(
        "INSERT INTO libraries(id,name,root,cache,shared_id) VALUES(?,?,?,?,?) ON CONFLICT(id) DO UPDATE "
        "SET name=excluded.name,root=excluded.root,cache=excluded.cache,shared_id=CASE WHEN "
        "excluded.cache=libraries.cache AND ?=0 THEN libraries.shared_id ELSE excluded.shared_id END");
    q.addBindValue(l.id);
    q.addBindValue(l.name);
    q.addBindValue(l.root);
    q.addBindValue(l.sharedCache.isNull() ? QString("") : l.sharedCache);
    q.addBindValue(l.sharedCache.isEmpty() ? QString("") : l.sharedId.isEmpty() ? l.id : l.sharedId);
    q.addBindValue(!l.sharedId.isEmpty());
    if (!q.exec()) {
        return fail(q.lastError().text());
    }
    if (!writeFolders(l.id, {""}))
        return fail(error_);
    q.prepare("DELETE FROM removed_libraries WHERE id=?");
    q.addBindValue(l.id);
    if (!q.exec())
        return fail(q.lastError().text());
    if (!db_.commit())
        return fail(db_.lastError().text());
    return true;
}
bool Index::attachSharedCache(const QString &library, const QString &cache, const QString &sharedId,
                              QString *error) {
    auto fail = [&](const QString &message) {
        db_.rollback();
        error_ = message;
        if (error)
            *error = message;
        return false;
    };
    if (cache.isEmpty() || !QDir::isAbsolutePath(cache) || QUuid(sharedId).isNull())
        return fail("共享缓存目录或库标识无效，现有资料保持不变。");
    QSqlQuery q(db_);
    if (!q.exec("BEGIN IMMEDIATE"))
        return fail(q.lastError().text());
    q.prepare("SELECT id FROM libraries WHERE shared_id=? AND id<>? LIMIT 1");
    q.addBindValue(sharedId);
    q.addBindValue(library);
    if (!q.exec())
        return fail(q.lastError().text());
    if (q.next())
        return fail("这个共享缓存已绑定本机的另一个素材库，请选择对应的素材库。");
    q.finish();
    q.prepare("UPDATE libraries SET cache=?,shared_id=? WHERE id=?");
    q.addBindValue(cache);
    q.addBindValue(sharedId);
    q.addBindValue(library);
    if (!q.exec())
        return fail(q.lastError().text());
    if (q.numRowsAffected() != 1)
        return fail("素材库不存在，原有资料保持不变。");
    q.finish();
    if (!db_.commit())
        return fail(db_.lastError().text());
    return true;
}
QList<Library> Index::libraries() const {
    QList<Library> out;
    QSqlQuery q(db_);
    if (q.exec("SELECT id,name,root,cache,shared_id FROM libraries ORDER BY name,id"))
        while (q.next())
            out << Library{q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                           q.value(3).toString(), q.value(4).toString()};
    return out;
}
std::optional<Track> Index::trackByIdentity(const QString &library, const QString &relative) const {
    error_.clear();
    QSqlQuery q(db_);
    q.prepare(
        "SELECT t.id,t.library,t.relative,t.name,t.folder,t.size,t.modified,t.duration,t.favorite,"
        "t.tags,l.root FROM tracks t JOIN libraries l ON l.id=t.library WHERE t.library=? AND t.relative=?");
    q.addBindValue(library);
    q.addBindValue(relative);
    if (!q.exec()) {
        error_ = q.lastError().text();
        return std::nullopt;
    }
    if (!q.next())
        return std::nullopt;
    Track t;
    t.id = q.value(0).toLongLong();
    t.libraryId = q.value(1).toString();
    t.relativePath = q.value(2).toString();
    t.name = q.value(3).toString();
    t.folder = q.value(4).toString();
    t.size = q.value(5).toLongLong();
    t.modified = q.value(6).toLongLong();
    t.duration = q.value(7).toLongLong();
    t.favorite = q.value(8).toBool();
    t.tags = q.value(9).toString();
    t.root = q.value(10).toString();
    return t;
}
bool Index::upsert(const QList<Track> &tracks, QString *error, bool *filesChanged) {
    if (filesChanged)
        *filesChanged = false;
    auto fail = [&](const QString &s) {
        db_.rollback();
        error_ = s;
        if (error)
            *error = s;
        return false;
    };
    QSqlQuery transaction(db_);
    // Serialize filtering with catalog removals so an already queued scan batch
    // cannot recreate an excluded entry after the removal commits.
    if (!transaction.exec("BEGIN IMMEDIATE"))
        return fail(transaction.lastError().text());
    QSqlQuery find(db_), insert(db_), update(db_), eraseFts(db_), insertFts(db_);
    QSqlQuery archived(db_), occupiedId(db_), eraseArchive(db_);
    find.prepare("SELECT id,size,modified,tags FROM tracks WHERE library=? AND relative=?");
    insert.prepare("INSERT INTO tracks(id,library,relative,name,folder,size,modified,duration,favorite,tags,"
                   "search_text,name_norm) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
    archived.prepare("SELECT original_id,size,modified,duration,favorite,tags FROM excluded_track_metadata "
                     "WHERE library=? AND relative=?");
    occupiedId.prepare("SELECT id FROM tracks WHERE id=?");
    eraseArchive.prepare("DELETE FROM excluded_track_metadata WHERE library=? AND relative=?");
    update.prepare("UPDATE tracks SET size=?,modified=?,duration=0 WHERE id=?");
    eraseFts.prepare("DELETE FROM search_fts WHERE rowid=?");
    insertFts.prepare("INSERT INTO search_fts(rowid,tokens) VALUES(?,?)");
    bool changed = false;
    QMap<QString, QStringList> directoryPaths;
    QMap<QString, CatalogExclusions> excluded;
    for (const auto &t : tracks) {
        if (!excluded.contains(t.libraryId)) {
            const auto policy = exclusions(t.libraryId, error);
            if (!policy)
                return fail(error_);
            excluded.insert(t.libraryId, *policy);
        }
        if (!excluded[t.libraryId].excludesTrack(t.relativePath))
            directoryPaths[t.libraryId].append(t.folder);
    }
    for (auto it = directoryPaths.cbegin(); it != directoryPaths.cend(); ++it)
        if (!writeFolders(it.key(), it.value(), excluded[it.key()]))
            return fail(error_);
    for (const auto &t : tracks) {
        if (t.relativePath.isEmpty() || t.relativePath.startsWith('/') || t.relativePath.contains("\\") ||
            t.relativePath.split('/').contains(".."))
            return fail("素材相对路径无效");
        if (excluded[t.libraryId].excludesTrack(t.relativePath))
            continue;
        find.bindValue(0, t.libraryId);
        find.bindValue(1, t.relativePath);
        if (!find.exec())
            return fail(find.lastError().text());
        qint64 id;
        QString personal;
        if (find.next()) {
            id = find.value(0).toLongLong();
            personal = find.value(3).toString();
            if (find.value(1).toLongLong() == t.size && find.value(2).toLongLong() == t.modified)
                continue;
            update.bindValue(0, t.size);
            update.bindValue(1, t.modified);
            update.bindValue(2, id);
            if (!update.exec())
                return fail(update.lastError().text());
        } else {
            find.finish();
            QVariant restoredId;
            qint64 duration = 0;
            bool favorite = false;
            personal = QString("");
            archived.bindValue(0, t.libraryId);
            archived.bindValue(1, t.relativePath);
            if (!archived.exec())
                return fail(archived.lastError().text());
            const bool hasArchive = archived.next();
            if (hasArchive) {
                const auto originalId = archived.value(0).toLongLong();
                occupiedId.bindValue(0, originalId);
                if (!occupiedId.exec())
                    return fail(occupiedId.lastError().text());
                // SQLite may have reused the old row ID for a new song while hidden.
                if (!occupiedId.next() && originalId > 0)
                    restoredId = originalId;
                occupiedId.finish();
                if (archived.value(1).toLongLong() == t.size && archived.value(2).toLongLong() == t.modified)
                    duration = archived.value(3).toLongLong();
                favorite = archived.value(4).toBool();
                personal = archived.value(5).toString();
            }
            archived.finish();
            insert.bindValue(0, restoredId);
            insert.bindValue(1, t.libraryId);
            insert.bindValue(2, t.relativePath);
            insert.bindValue(3, t.name);
            insert.bindValue(4, t.folder);
            insert.bindValue(5, t.size);
            insert.bindValue(6, t.modified);
            insert.bindValue(7, duration);
            insert.bindValue(8, favorite);
            insert.bindValue(9, personal);
            insert.bindValue(10, normalizeSearch(t.relativePath + " " + personal));
            insert.bindValue(11, normalizeSearch(t.name));
            if (!insert.exec())
                return fail(insert.lastError().text());
            id = insert.lastInsertId().toLongLong();
            if (hasArchive) {
                eraseArchive.bindValue(0, t.libraryId);
                eraseArchive.bindValue(1, t.relativePath);
                if (!eraseArchive.exec())
                    return fail(eraseArchive.lastError().text());
            }
        }
        changed = true;
        eraseFts.bindValue(0, id);
        if (!eraseFts.exec())
            return fail(eraseFts.lastError().text());
        insertFts.bindValue(0, id);
        insertFts.bindValue(1, tokenText(t.relativePath + " " + personal));
        if (!insertFts.exec())
            return fail(insertFts.lastError().text());
    }
    if (changed) {
        QSqlQuery revision(db_);
        if (!revision.exec("UPDATE index_state SET generation=generation+1 WHERE id=1"))
            return fail(revision.lastError().text());
    }
    if (!db_.commit())
        return fail(db_.lastError().text());
    if (filesChanged)
        *filesChanged = changed;
    return true;
}
SearchPage Index::search(const QString &query, const QString &library, const QString &folder, bool favorites,
                         int offset, int limit, bool includeSubfolders) const {
    return readSearch(query, library, folder, favorites, offset, qBound(1, limit, 500), includeSubfolders,
                      {});
}
SearchPage Index::searchAll(const QString &query, const QString &library, const QString &folder,
                            bool favorites, bool includeSubfolders,
                            const std::function<bool()> &cancelled) const {
    return readSearch(query, library, folder, favorites, 0, -1, includeSubfolders, cancelled);
}
SearchPage Index::readSearch(const QString &query, const QString &library, const QString &folder,
                             bool favorites, int offset, int limit, bool includeSubfolders,
                             const std::function<bool()> &cancelled) const {
    QElapsedTimer timer;
    timer.start();
    SearchPage page;
    QSqlQuery snapshot(db_);
    error_.clear();
    auto cancel = [&] {
        page.tracks.clear();
        page.more = false;
        page.cancelled = true;
        snapshot.exec("ROLLBACK");
        page.elapsedMs = timer.elapsed();
        return page;
    };
    if (cancelled && cancelled()) {
        page.cancelled = true;
        return page;
    }
    auto fail = [&](const QString &message) {
        page.tracks.clear();
        page.more = false;
        page.error = message;
        error_ = message;
        snapshot.exec("ROLLBACK");
        page.elapsedMs = timer.elapsed();
        return page;
    };
    if (!snapshot.exec("BEGIN"))
        return fail(snapshot.lastError().text());
    if (!snapshot.exec("SELECT generation FROM index_state WHERE id=1"))
        return fail(snapshot.lastError().text());
    if (!snapshot.next())
        return fail(snapshot.lastError().isValid() ? snapshot.lastError().text()
                                                   : QString("索引状态缺失，请刷新素材库后重试。"));
    page.generation = snapshot.value(0).toLongLong();
    snapshot.finish();
    const auto normalized = normalizeSearch(query.left(512));
    const auto terms = normalized.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QString sql = "SELECT "
                  "t.id,t.library,t.relative,t.name,t.folder,t.size,t.modified,t.duration,t.favorite,t.tags,"
                  "l.root FROM tracks t JOIN libraries l ON l.id=t.library WHERE 1=1";
    QList<QVariant> binds;
    if (!terms.isEmpty()) {
        sql += " AND t.id IN (SELECT rowid FROM search_fts WHERE search_fts MATCH ?)";
        binds << matchExpression(terms);
        for (const auto &term : terms) {
            sql += " AND instr(t.search_text,?)>0";
            binds << term;
        }
    }
    if (!library.isEmpty()) {
        sql += " AND t.library=?";
        binds << library;
    }
    if (!includeSubfolders && !library.isEmpty()) {
        sql += " AND t.folder=?";
        binds << (folder.isEmpty() ? QString(".") : folder);
    } else if (!folder.isEmpty()) {
        sql += " AND (t.folder=? OR substr(t.folder,1,?)=?)";
        binds << folder << folder.toUcs4().size() + 1 << folder + "/";
    }
    if (favorites)
        sql += " AND t.favorite=1";
    if (!normalized.isEmpty()) {
        sql += " ORDER BY CASE WHEN t.name_norm=? THEN 0 WHEN substr(t.name_norm,1,?)=? THEN 1 ELSE 2 "
               "END,t.name_norm,t.id";
        binds << normalized << normalized.toUcs4().size() << normalized;
    } else
        sql += " ORDER BY t.name_norm,t.id";
    if (limit >= 0) {
        sql += " LIMIT ? OFFSET ?";
        binds << limit + 1 << qMax(0, offset);
    }
    QSqlQuery q(db_);
    q.setForwardOnly(true); // Do not retain a second copy of every metadata row in the SQL driver.
    if (!q.prepare(sql))
        return fail(q.lastError().text());
    for (const auto &b : binds)
        q.addBindValue(b);
    if (cancelled && cancelled())
        return cancel();
    if (!q.exec())
        return fail(q.lastError().text());
    QHash<QString, QString> libraryRoots;
    for (;;) {
        // The callback is cheap (an atomic generation check in the desktop caller).
        // Check in bounded batches, including immediately after a potentially expensive SQL sort.
        if (page.tracks.size() % 128 == 0 && cancelled && cancelled()) {
            q.finish();
            return cancel();
        }
        if (!q.next())
            break;
        if (limit >= 0 && page.tracks.size() >= limit) {
            page.more = true;
            break;
        }
        Track t;
        t.id = q.value(0).toLongLong();
        const auto libraryKey = q.value(1).toString();
        auto root = libraryRoots.constFind(libraryKey);
        if (root == libraryRoots.cend())
            root = libraryRoots.insert(libraryKey, q.value(10).toString());
        // All rows in a library share these two QString allocations.
        t.libraryId = root.key();
        t.root = root.value();
        t.relativePath = q.value(2).toString();
        t.name = q.value(3).toString();
        t.folder = q.value(4).toString();
        t.size = q.value(5).toLongLong();
        t.modified = q.value(6).toLongLong();
        t.duration = q.value(7).toLongLong();
        t.favorite = q.value(8).toBool();
        t.tags = q.value(9).toString();
        page.tracks << t;
    }
    if (q.lastError().isValid()) {
        const auto message = q.lastError().text();
        q.finish();
        return fail(message);
    }
    q.finish();
    if (cancelled && cancelled())
        return cancel();
    if (!snapshot.exec("COMMIT"))
        return fail(snapshot.lastError().text());
    page.elapsedMs = timer.elapsed();
    return page;
}
bool Index::setPersonal(const Track &version, bool favorite, const QString &tags) {
    auto fail = [&](const QString &message) {
        db_.rollback();
        error_ = message;
        return false;
    };
    QSqlQuery q(db_);
    if (!q.exec("BEGIN IMMEDIATE"))
        return fail(q.lastError().text());
    const auto savedTags = tags.isNull() ? QString("") : tags.left(4096);
    const auto text = version.relativePath + " " + savedTags;
    q.prepare("UPDATE tracks SET favorite=?,tags=?,search_text=? WHERE id=? AND library=? AND relative=? "
              "AND size=? AND modified=?");
    q.addBindValue(favorite);
    q.addBindValue(savedTags);
    q.addBindValue(normalizeSearch(text));
    q.addBindValue(version.id);
    q.addBindValue(version.libraryId);
    q.addBindValue(version.relativePath);
    q.addBindValue(version.size);
    q.addBindValue(version.modified);
    if (!q.exec())
        return fail(q.lastError().text());
    if (q.numRowsAffected() != 1)
        return fail("这首音乐已从列表移除或文件已变化，未保存标记；请重新选择后再试。");
    q.prepare("DELETE FROM search_fts WHERE rowid=?");
    q.addBindValue(version.id);
    if (!q.exec())
        return fail(q.lastError().text());
    q.prepare("INSERT INTO search_fts(rowid,tokens) VALUES(?,?)");
    q.addBindValue(version.id);
    q.addBindValue(tokenText(text));
    if (!q.exec())
        return fail(q.lastError().text());
    if (!q.exec("UPDATE index_state SET generation=generation+1 WHERE id=1"))
        return fail(q.lastError().text());
    if (!db_.commit())
        return fail(db_.lastError().text());
    error_.clear();
    return true;
}
bool Index::setPersonal(qint64 id, bool favorite, const QString &tags) {
    QSqlQuery q(db_);
    q.prepare("SELECT library,relative,size,modified FROM tracks WHERE id=?");
    q.addBindValue(id);
    if (!q.exec()) {
        error_ = q.lastError().text();
        return false;
    }
    if (!q.next()) {
        error_ = "这首音乐已从列表移除，未保存标记。";
        return false;
    }
    Track version;
    version.id = id;
    version.libraryId = q.value(0).toString();
    version.relativePath = q.value(1).toString();
    version.size = q.value(2).toLongLong();
    version.modified = q.value(3).toLongLong();
    q.finish();
    return setPersonal(version, favorite, tags);
}
bool Index::setDuration(const Track &version, qint64 ms) {
    QSqlQuery q(db_);
    q.prepare("UPDATE tracks SET duration=? WHERE id=? AND library=? AND relative=? AND size=? AND "
              "modified=?");
    q.addBindValue(ms);
    q.addBindValue(version.id);
    q.addBindValue(version.libraryId);
    q.addBindValue(version.relativePath);
    q.addBindValue(version.size);
    q.addBindValue(version.modified);
    return q.exec();
}
qint64 Index::count() const {
    QSqlQuery q(db_);
    return q.exec("SELECT count(*) FROM tracks") && q.next() ? q.value(0).toLongLong() : 0;
}
qint64 Index::countForLibrary(const QString &library) const {
    QSqlQuery q(db_);
    q.prepare("SELECT count(*) FROM tracks WHERE library=?");
    q.addBindValue(library);
    return q.exec() && q.next() ? q.value(0).toLongLong() : 0;
}
QList<Track> Index::tracksAfter(qint64 id, int limit) const {
    QSqlQuery q(db_);
    q.prepare("SELECT t.id,t.library,t.relative,t.name,t.folder,t.size,t.modified,t.duration,l.root "
              "FROM tracks t JOIN libraries l ON l.id=t.library WHERE t.id>? ORDER BY t.id LIMIT ?");
    q.addBindValue(id);
    q.addBindValue(qBound(1, limit, 200));
    QList<Track> tracks;
    if (!q.exec()) {
        error_ = q.lastError().text();
        return tracks;
    }
    while (q.next()) {
        Track t;
        t.id = q.value(0).toLongLong();
        t.libraryId = q.value(1).toString();
        t.relativePath = q.value(2).toString();
        t.name = q.value(3).toString();
        t.folder = q.value(4).toString();
        t.size = q.value(5).toLongLong();
        t.modified = q.value(6).toLongLong();
        t.duration = q.value(7).toLongLong();
        t.root = q.value(8).toString();
        tracks << t;
    }
    return tracks;
}
QStringList Index::folders(const QString &library) const {
    QSqlQuery q(db_);
    q.prepare("SELECT path FROM folders WHERE library=? ORDER BY path");
    q.addBindValue(library);
    QStringList out;
    if (q.exec())
        while (q.next())
            out << q.value(0).toString();
    return out;
}
bool Index::upgradeFolders(QString *error) {
    auto fail = [&](const QString &message) {
        db_.rollback();
        error_ = message;
        if (error)
            *error = message;
        return false;
    };
    QSqlQuery q(db_);
    if (!q.exec("BEGIN IMMEDIATE"))
        return fail(q.lastError().text());
    if (!q.exec("PRAGMA user_version") || !q.next())
        return fail(q.lastError().text());
    const bool migratedByPeer = q.value(0).toInt() >= 3;
    q.finish();
    if (migratedByPeer)
        return db_.commit() || fail(db_.lastError().text());
    const QStringList schema = {
        "CREATE TABLE IF NOT EXISTS folders(id INTEGER PRIMARY KEY, library TEXT NOT NULL REFERENCES "
        "libraries(id), "
        "path TEXT NOT NULL, parent TEXT NOT NULL, name TEXT NOT NULL, name_norm TEXT NOT NULL, search_text "
        "TEXT NOT NULL, UNIQUE(library,path))",
        "CREATE INDEX IF NOT EXISTS folders_parent ON folders(library,parent,path)",
        "CREATE VIRTUAL TABLE IF NOT EXISTS folder_fts USING fts5(tokens,tokenize='ascii')"};
    for (const auto &sql : schema)
        if (!q.exec(sql))
            return fail(q.lastError().text());
    for (const auto &library : libraries()) {
        QSqlQuery paths(db_);
        paths.prepare("SELECT DISTINCT folder FROM tracks WHERE library=?");
        paths.addBindValue(library.id);
        if (!paths.exec())
            return fail(paths.lastError().text());
        QStringList all{QString("")};
        while (paths.next())
            all << paths.value(0).toString();
        paths.finish();
        if (!writeFolders(library.id, all))
            return fail(error_);
    }
    if (!q.exec("PRAGMA user_version=3"))
        return fail(q.lastError().text());
    if (!db_.commit())
        return fail(db_.lastError().text());
    return true;
}
bool Index::writeFolders(const QString &library, const QStringList &paths,
                         const CatalogExclusions &excluded) {
    QSqlQuery insert(db_), fts(db_), lib(db_);
    lib.prepare("SELECT name FROM libraries WHERE id=?");
    lib.addBindValue(library);
    if (!lib.exec() || !lib.next()) {
        error_ = "素材库不存在";
        return false;
    }
    const auto libraryName = lib.value(0).toString();
    lib.finish();
    insert.prepare(
        "INSERT OR IGNORE INTO folders(library,path,parent,name,name_norm,search_text) VALUES(?,?,?,?,?,?)");
    fts.prepare("INSERT INTO folder_fts(rowid,tokens) VALUES(?,?)");
    QSet<QString> seen;
    for (auto path : paths) {
        if (path == ".")
            path.clear();
        if (path.startsWith('/') || path.contains('\\') || path.split('/').contains("..") ||
            (!path.isEmpty() && (path.endsWith('/') || path.contains("//")))) {
            error_ = "目录相对路径无效";
            return false;
        }
        if (excluded.excludesFolder(path))
            continue;
        QStringList chain{QString("")};
        QString current;
        for (const auto &part : path.split('/', Qt::SkipEmptyParts)) {
            current = current.isEmpty() ? part : current + "/" + part;
            chain << current;
        }
        for (const auto &value : chain) {
            if (seen.contains(value))
                continue;
            seen.insert(value);
            const int slash = value.lastIndexOf('/');
            const auto name = value.isEmpty() ? libraryName : value.mid(slash + 1);
            const auto parent = slash < 0 ? QString("") : value.left(slash);
            const auto text = normalizeSearch(value.isEmpty() ? libraryName : value);
            insert.bindValue(0, library);
            insert.bindValue(1, value.isNull() ? QString("") : value);
            insert.bindValue(2, parent);
            insert.bindValue(3, name);
            insert.bindValue(4, normalizeSearch(name));
            insert.bindValue(5, text);
            if (!insert.exec()) {
                error_ = insert.lastError().text();
                return false;
            }
            if (insert.numRowsAffected() == 0)
                continue;
            fts.bindValue(0, insert.lastInsertId());
            fts.bindValue(1, tokenText(text));
            if (!fts.exec()) {
                error_ = fts.lastError().text();
                return false;
            }
        }
    }
    return true;
}
bool Index::upsertFolders(const QString &library, const QStringList &paths, QString *error) {
    QSqlQuery transaction(db_);
    if (!transaction.exec("BEGIN IMMEDIATE")) {
        error_ = transaction.lastError().text();
        if (error)
            *error = error_;
        return false;
    }
    const auto excluded = exclusions(library, error);
    if (!excluded || !writeFolders(library, paths, *excluded) || !db_.commit()) {
        if (error_.isEmpty())
            error_ = db_.lastError().text();
        db_.rollback();
        if (error)
            *error = error_;
        return false;
    }
    return true;
}
FolderPage Index::readFolders(const QString &where, const QList<QVariant> &binds, const QString &order,
                              int offset, int limit) const {
    QElapsedTimer timer;
    timer.start();
    FolderPage page;
    const int count = qBound(1, limit, 500);
    QSqlQuery q(db_);
    q.prepare(
        "SELECT f.library,f.path,f.parent,f.name,l.name,"
        "EXISTS(SELECT 1 FROM folders c WHERE c.library=f.library AND c.parent=f.path AND c.path<>f.path) "
        "FROM folders f JOIN libraries l ON l.id=f.library WHERE " +
        where + " ORDER BY " + order + " LIMIT ? OFFSET ?");
    for (const auto &value : binds)
        q.addBindValue(value);
    q.addBindValue(count + 1);
    q.addBindValue(qMax(0, offset));
    if (!q.exec())
        page.error = q.lastError().text();
    else
        while (q.next()) {
            if (page.folders.size() == count) {
                page.more = true;
                break;
            }
            page.folders << FolderRecord{q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                                         q.value(3).toString(), q.value(4).toString(), q.value(5).toBool()};
        }
    page.elapsedMs = timer.elapsed();
    return page;
}
FolderPage Index::findFolders(const QString &query, int offset, int limit) const {
    const auto normalized = normalizeSearch(query.left(512)).trimmed();
    const auto terms = normalized.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (terms.isEmpty())
        return {};
    QString where = "f.id IN (SELECT rowid FROM folder_fts WHERE folder_fts MATCH ?)";
    QList<QVariant> binds{matchExpression(terms)};
    for (const auto &term : terms) {
        where += " AND instr(f.search_text,?)>0";
        binds << term;
    }
    binds << normalized << normalized.toUcs4().size() << normalized << normalized;
    return readFolders(where, binds,
                       "CASE WHEN f.name_norm=? THEN 0 WHEN substr(f.name_norm,1,?)=? THEN 1 WHEN "
                       "instr(f.name_norm,?)>0 THEN 2 ELSE 3 END,f.path,f.library",
                       offset, limit);
}
FolderPage Index::childFolders(const QString &library, const QString &parent, int offset, int limit) const {
    return readFolders("f.library=? AND f.parent=? AND f.path<>?", {library, parent, parent}, "f.path",
                       offset, limit);
}
FolderPage Index::folderAncestors(const QString &library, const QString &path) const {
    FolderPage result;
    QStringList values{QString("")};
    QString current;
    for (const auto &part : path.split('/', Qt::SkipEmptyParts)) {
        current = current.isEmpty() ? part : current + "/" + part;
        values << current;
    }
    for (const auto &value : values) {
        auto found = readFolders("f.library=? AND f.path=?", {library, value}, "f.path", 0, 1);
        if (!found.error.isEmpty() || found.folders.isEmpty()) {
            result.error = found.error.isEmpty() ? QString("目录尚未收录，请刷新素材库。") : found.error;
            result.folders.clear();
            return result;
        }
        result.folders << found.folders.first();
    }
    return result;
}
bool Index::importSnapshot(const QString &source, const QString &target, QString *error) {
    if (QFileInfo::exists(target) || !QFileInfo::exists(source))
        return true;
    QDir().mkpath(QFileInfo(target).absolutePath());
    const auto temporary = target + ".import-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(source);
        db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=3000");
        if (!db.open())
            *error = db.lastError().text();
        else {
            QSqlQuery q(db);
            q.prepare("VACUUM INTO ?");
            q.addBindValue(temporary);
            ok = q.exec();
            if (!ok)
                *error = q.lastError().text();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    if (ok) {
        ok = QFile::rename(temporary, target);
        if (!ok)
            *error = "无法保存新版索引副本；原索引未修改。";
    }
    if (!ok)
        QFile::remove(temporary);
    return ok;
}
QString Index::lastError() const {
    return error_;
}
QString Track::absolutePath() const {
    return QDir(root).filePath(relativePath);
}
QList<QUrl> originalFileUrls(const QList<Track> &tracks) {
    QList<QUrl> out;
    for (const auto &t : tracks)
        out << QUrl::fromLocalFile(t.absolutePath());
    return out;
}
