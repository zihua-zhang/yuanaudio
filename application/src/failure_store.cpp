#include "failure_store.h"
#include "cache.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QUrl>

namespace {
QJsonObject trackJson(const Track &t) {
    return {{"id", t.id},     {"library", t.libraryId}, {"path", t.relativePath},
            {"name", t.name}, {"folder", t.folder},     {"root", t.root},
            {"size", t.size}, {"modified", t.modified}, {"duration", t.duration},
            {"tags", t.tags}, {"favorite", t.favorite}};
}
Track parseTrack(const QJsonObject &o) {
    Track t;
    t.id = o["id"].toInteger();
    t.libraryId = o["library"].toString();
    t.relativePath = o["path"].toString();
    t.name = o["name"].toString();
    t.folder = o["folder"].toString();
    t.root = o["root"].toString();
    t.size = o["size"].toInteger();
    t.modified = o["modified"].toInteger();
    t.duration = o["duration"].toInteger();
    t.tags = o["tags"].toString();
    t.favorite = o["favorite"].toBool();
    return t;
}
QString versionKey(const Track &t) {
    return waveKey(t.libraryId, t.relativePath, t.size, t.modified);
}
bool sqlError(const QSqlQuery &query, QString *error) {
    if (error)
        *error = query.lastError().text();
    return false;
}
} // namespace

FailureStore::FailureStore(QString path, QString indexPath)
    : path_(std::move(path)), indexPath_(std::move(indexPath)),
      connection_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {}
FailureStore::~FailureStore() {
    db_.close();
    db_ = {};
    QSqlDatabase::removeDatabase(connection_);
}
bool FailureStore::open(QString *error) {
    QDir().mkpath(QFileInfo(path_).absolutePath());
    db_ = QSqlDatabase::addDatabase("QSQLITE", connection_);
    db_.setDatabaseName(path_);
    db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=3000;QSQLITE_OPEN_URI");
    if (!db_.open()) {
        if (error)
            *error = db_.lastError().text();
        return false;
    }
    QSqlQuery q(db_);
    for (const auto &sql :
         {"PRAGMA journal_mode=WAL", "PRAGMA synchronous=NORMAL",
          "CREATE TABLE IF NOT EXISTS failures(id INTEGER PRIMARY KEY AUTOINCREMENT,version TEXT NOT NULL "
          "UNIQUE,"
          "library TEXT NOT NULL,relative TEXT NOT NULL,category TEXT NOT NULL,kind TEXT NOT NULL,"
          "message TEXT NOT NULL,snapshot TEXT NOT NULL,reference INTEGER NOT NULL DEFAULT 0,updated INTEGER "
          "NOT NULL)",
          "CREATE INDEX IF NOT EXISTS failure_scope ON failures(reference,category,id)",
          "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value INTEGER NOT NULL)"})
        if (!q.exec(QString::fromLatin1(sql)))
            return sqlError(q, error);
    return true;
}
QString FailureStore::category(const QString &kind) {
    if (kind == "source_unavailable" || kind == "network")
        return "network";
    if (kind == "unsupported" || kind == "unsupported_format")
        return "unsupported";
    if (kind == "invalid_audio" || kind == "damaged" || kind == "reference")
        return "damaged";
    if (kind == "decode_warning")
        return "decode_warning";
    if (kind == "cache_write" || kind == "storage_full" || kind == "write" || kind == "shared_cache_write" ||
        kind == "local_cache_missing")
        return "storage";
    if (kind == "timeout" || kind == "shared_timeout" || kind == "shared_publish_timeout")
        return "timeout";
    if (kind == "source_changed")
        return "changed";
    if (kind == "manual_skip")
        return "skipped";
    return "unknown";
}
bool FailureStore::attachCatalog(QString *error) const {
    if (catalogAttached_)
        return true;
    QSqlQuery attach(db_);
    attach.prepare("ATTACH DATABASE ? AS catalog");
    // mode=ro fails for a missing catalog instead of silently creating one.
    // Percent encoding also handles Chinese names, spaces, # and ? in paths.
    attach.addBindValue(QString::fromLatin1(
                            QUrl::fromLocalFile(QFileInfo(indexPath_).absoluteFilePath()).toEncoded()) +
                        "?mode=ro");
    if (!attach.exec())
        return sqlError(attach, error);
    catalogAttached_ = true;
    return true;
}
QString FailureStore::activeFilter(QString *error) const {
    QSqlQuery present(db_);
    if (!present.exec("SELECT 1 FROM failures LIMIT 1")) {
        sqlError(present, error);
        return {};
    }
    // A brand-new profile initializes the ledger before its catalog is ready.
    // There is no snapshot to validate or retry in an empty ledger.
    if (!present.next())
        return "0";
    present.finish();
    if (!attachCatalog(error))
        return {};
    return "EXISTS (SELECT 1 FROM catalog.tracks t JOIN catalog.libraries l ON l.id=t.library "
           "WHERE t.library=failures.library AND t.relative=failures.relative)";
}
bool FailureStore::currentTracks(QList<FailureRecord> *records, QString *error) const {
    if (records->isEmpty())
        return true;
    if (!attachCatalog(error))
        return false;
    QSqlQuery q(db_);
    if (!q.prepare("SELECT t.id,t.name,t.folder,t.size,t.modified,t.duration,t.tags,t.favorite,l.root "
                   "FROM catalog.tracks t JOIN catalog.libraries l ON l.id=t.library "
                   "WHERE t.library=? AND t.relative=?"))
        return sqlError(q, error);
    QList<FailureRecord> current;
    for (auto record : *records) {
        q.bindValue(0, record.track.libraryId);
        q.bindValue(1, record.track.relativePath);
        if (!q.exec())
            return sqlError(q, error);
        if (q.next()) {
            auto &t = record.track;
            t.id = q.value(0).toLongLong();
            t.name = q.value(1).toString();
            t.folder = q.value(2).toString();
            t.size = q.value(3).toLongLong();
            t.modified = q.value(4).toLongLong();
            t.duration = q.value(5).toLongLong();
            t.tags = q.value(6).toString();
            t.favorite = q.value(7).toBool();
            t.root = q.value(8).toString();
            current.append(std::move(record));
        } else if (q.lastError().isValid())
            return sqlError(q, error);
        q.finish();
    }
    *records = std::move(current);
    return true;
}
bool FailureStore::importLegacy(const QJsonArray &recent, qint64 failed, QString *error) {
    QSqlQuery check(db_);
    if (!check.exec("SELECT value FROM metadata WHERE key='legacy_imported'"))
        return sqlError(check, error);
    if (check.next())
        return true;
    QList<FailureRecord> records;
    for (const auto &value : recent) {
        const auto o = value.toObject();
        records.append({0, parseTrack(o), o["kind"].toString(), o["message"].toString(), false});
    }
    if (!records.isEmpty() && !currentTracks(&records, error))
        return false;
    QList<FailureOutcome> outcomes;
    for (const auto &r : records)
        if (r.track.id && !r.track.root.isEmpty() && !r.track.libraryId.isEmpty())
            outcomes.append({r.track, false, false, r.kind, r.message, 0});
    if (!apply(outcomes, error))
        return false;
    QSqlQuery q(db_);
    q.prepare("INSERT OR REPLACE INTO metadata(key,value) VALUES('legacy_unknown',?)");
    q.addBindValue(qMax(qint64(0), failed - summary(error)["knownFailures"].toInteger()));
    if (!q.exec())
        return sqlError(q, error);
    return q.exec("INSERT OR REPLACE INTO metadata(key,value) VALUES('legacy_imported',1)") ||
           sqlError(q, error);
}
bool FailureStore::apply(const QList<FailureOutcome> &outcomes, QString *error) {
    if (!db_.transaction()) {
        if (error)
            *error = db_.lastError().text();
        return false;
    }
    auto fail = [&](const QSqlQuery &q) {
        db_.rollback();
        return sqlError(q, error);
    };
    QSqlQuery erase(db_), insert(db_);
    erase.prepare("DELETE FROM failures WHERE (library=? AND relative=?) OR id=?");
    insert.prepare(
        "INSERT INTO failures(version,library,relative,category,kind,message,snapshot,reference,updated) "
        "VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(version) DO UPDATE SET category=excluded.category,"
        "kind=excluded.kind,message=excluded.message,snapshot=excluded.snapshot,"
        "reference=excluded.reference,updated=excluded.updated");
    for (const auto &o : outcomes) {
        // Success repairs every older failure for this source; retry failure retains its cursor id.
        if (o.success || o.retryId) {
            if (o.retryId && !o.success) {
                QSqlQuery duplicate(db_);
                duplicate.prepare("DELETE FROM failures WHERE version=? AND id<>?");
                duplicate.addBindValue(versionKey(o.track));
                duplicate.addBindValue(o.retryId);
                if (!duplicate.exec())
                    return fail(duplicate);
                QSqlQuery update(db_);
                update.prepare(
                    "UPDATE failures SET "
                    "version=?,category=?,kind=?,message=?,snapshot=?,updated=?,reference=? WHERE id=?");
                update.addBindValue(versionKey(o.track));
                update.addBindValue(category(o.kind));
                update.addBindValue(o.kind.isNull() ? QString("") : o.kind);
                update.addBindValue(o.message.isNull() ? QString("") : o.message);
                update.addBindValue(
                    QString::fromUtf8(QJsonDocument(trackJson(o.track)).toJson(QJsonDocument::Compact)));
                update.addBindValue(QDateTime::currentMSecsSinceEpoch());
                update.addBindValue(o.reference ? 1 : 0);
                update.addBindValue(o.retryId);
                if (!update.exec())
                    return fail(update);
                if (update.numRowsAffected() > 0)
                    continue;
            } else {
                erase.bindValue(0, o.track.libraryId);
                erase.bindValue(1, o.track.relativePath);
                erase.bindValue(2, o.retryId);
                if (!erase.exec())
                    return fail(erase);
            }
        }
        if (o.success && !o.reference)
            continue;
        insert.bindValue(0, versionKey(o.track));
        insert.bindValue(1, o.track.libraryId);
        insert.bindValue(2, o.track.relativePath);
        const auto kind = o.kind.isEmpty() && o.reference ? QString("reference") : o.kind;
        insert.bindValue(3, category(kind));
        insert.bindValue(4, kind.isNull() ? QString("") : kind);
        insert.bindValue(5, o.message.isNull() ? QString("") : o.message);
        insert.bindValue(6,
                         QString::fromUtf8(QJsonDocument(trackJson(o.track)).toJson(QJsonDocument::Compact)));
        insert.bindValue(7, o.reference ? 1 : 0);
        insert.bindValue(8, QDateTime::currentMSecsSinceEpoch());
        if (!insert.exec())
            return fail(insert);
    }
    if (!db_.commit()) {
        if (error)
            *error = db_.lastError().text();
        return false;
    }
    return true;
}
QJsonObject FailureStore::summary(QString *error) const {
    const auto active = activeFilter(error);
    if (active.isEmpty())
        return {};
    QJsonObject result{{"knownFailures", 0}, {"knownReferences", 0}, {"legacyUntracked", 0},
                       {"inactiveRecords", 0}, {"inactiveReferences", 0}};
    QJsonObject categories, referenceCategories;
    QSqlQuery q(db_);
    if (!q.exec("SELECT reference,category,COUNT(*) FROM failures WHERE " + active +
                " GROUP BY reference,category")) {
        sqlError(q, error);
        return {};
    }
    while (q.next()) {
        const bool reference = q.value(0).toBool();
        const auto count = q.value(2).toLongLong();
        const auto field = reference ? "knownReferences" : "knownFailures";
        result[field] = result[field].toInteger() + count;
        if (!reference)
            categories[q.value(1).toString()] = count;
        else
            referenceCategories[q.value(1).toString()] = count;
    }
    if (q.lastError().isValid()) {
        sqlError(q, error);
        return {};
    }
    if (!q.exec("SELECT COUNT(*),COALESCE(SUM(reference),0) FROM failures") || !q.next()) {
        sqlError(q, error);
        return {};
    }
    result["inactiveRecords"] = q.value(0).toLongLong() - result["knownFailures"].toInteger() -
                                 result["knownReferences"].toInteger();
    result["inactiveReferences"] = q.value(1).toLongLong() - result["knownReferences"].toInteger();
    if (!q.exec("SELECT value FROM metadata WHERE key='legacy_unknown'")) {
        sqlError(q, error);
        return {};
    }
    if (q.next())
        result["legacyUntracked"] = q.value(0).toLongLong();
    result["categories"] = categories;
    result["referenceCategories"] = referenceCategories;
    return result;
}
QJsonObject FailureStore::page(int offset, int limit, const QString &categoryFilter, bool references,
                               QString *error) const {
    QJsonObject result = summary(error);
    if (result.isEmpty())
        return {};
    const auto active = activeFilter(error);
    if (active.isEmpty())
        return {};
    QJsonArray items;
    const auto where =
        QString(" WHERE reference=? AND ") + active +
        (categoryFilter.isEmpty() ? QString() : QString(" AND category=?"));
    QSqlQuery q(db_);
    q.prepare("SELECT COUNT(*) FROM failures" + where);
    q.addBindValue(references ? 1 : 0);
    if (!categoryFilter.isEmpty())
        q.addBindValue(categoryFilter);
    if (!q.exec() || !q.next()) {
        sqlError(q, error);
        return {};
    }
    const auto total = q.value(0).toLongLong();
    q.prepare("SELECT id,snapshot,kind,message,category,updated FROM failures" + where +
              " ORDER BY id LIMIT ? OFFSET ?");
    q.addBindValue(references ? 1 : 0);
    if (!categoryFilter.isEmpty())
        q.addBindValue(categoryFilter);
    q.addBindValue(qBound(1, limit, 200));
    q.addBindValue(qMax(0, offset));
    if (!q.exec()) {
        sqlError(q, error);
        return {};
    }
    while (q.next()) {
        auto item = QJsonDocument::fromJson(q.value(1).toByteArray()).object();
        item["failureId"] = q.value(0).toLongLong();
        item["kind"] = q.value(2).toString();
        item["message"] = q.value(3).toString();
        item["category"] = q.value(4).toString();
        item["time"] = QDateTime::fromMSecsSinceEpoch(q.value(5).toLongLong()).toString(Qt::ISODate);
        items.append(item);
    }
    if (q.lastError().isValid()) {
        sqlError(q, error);
        return {};
    }
    result["items"] = items;
    result["total"] = total;
    result["offset"] = qMax(0, offset);
    result["more"] = offset + items.size() < total;
    result["references"] = references;
    return result;
}
QList<FailureRecord> FailureStore::retryPage(qint64 cursor, qint64 upper, int limit, QString *error,
                                             bool references) const {
    QList<FailureRecord> out;
    const auto active = activeFilter(error);
    if (active.isEmpty())
        return {};
    QSqlQuery q(db_);
    q.prepare("SELECT id,snapshot,kind,message FROM failures WHERE reference=? AND id>? AND id<=? AND " +
              active + " ORDER BY id LIMIT ?");
    q.addBindValue(references ? 1 : 0);
    q.addBindValue(cursor);
    q.addBindValue(upper);
    q.addBindValue(qBound(1, limit, 200));
    if (!q.exec()) {
        sqlError(q, error);
        return {};
    }
    while (q.next())
        out.append({q.value(0).toLongLong(),
                    parseTrack(QJsonDocument::fromJson(q.value(1).toByteArray()).object()),
                    q.value(2).toString(), q.value(3).toString(), references});
    if (q.lastError().isValid()) {
        sqlError(q, error);
        return {};
    }
    if (!currentTracks(&out, error))
        return {};
    for (const auto &record : out)
        if (!record.track.id || record.track.libraryId.isEmpty() || record.track.relativePath.isEmpty() ||
            record.track.root.isEmpty()) {
            if (error)
                *error = "异常记录缺少有效原文件位置，请重新检查素材库。";
            return {};
        }
    return out;
}
qint64 FailureStore::retryUpper(QString *error, bool references) const {
    const auto active = activeFilter(error);
    if (active.isEmpty())
        return 0;
    QSqlQuery q(db_);
    q.prepare("SELECT COALESCE(MAX(id),0) FROM failures WHERE reference=? AND " + active);
    q.addBindValue(references ? 1 : 0);
    if (!q.exec() || !q.next()) {
        sqlError(q, error);
        return 0;
    }
    return q.value(0).toLongLong();
}
bool FailureStore::clearLegacyUnknown(QString *error) {
    QSqlQuery q(db_);
    return q.exec("INSERT OR REPLACE INTO metadata(key,value) VALUES('legacy_unknown',0)") ||
           sqlError(q, error);
}
