#include "path_repair.h"
#include "failure_store.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>

namespace {
bool within(const QString &path, const QString &root) {
    // macOS and common SMB shares accept alternate case/normalization spellings.
    // Be conservative for protected roots even on a case-sensitive share.
    const auto a = path.normalized(QString::NormalizationForm_C);
    const auto b = root.normalized(QString::NormalizationForm_C);
    return a.compare(b, Qt::CaseInsensitive) == 0 || a.startsWith(b + '/', Qt::CaseInsensitive);
}
bool cleanAbsolute(const QString &p) {
    return p.startsWith('/') && QDir::cleanPath(p) == p && p != "/" && !p.contains('\\');
}
bool safeRelative(const QString &p) {
    return !p.isEmpty() && !QDir::isAbsolutePath(p) && QDir::cleanPath(p) == p &&
           !p.contains('\\') && !p.split('/').contains("..") && !p.split('/').contains(".");
}
bool noLinks(const QString &path) {
    QString current = "/";
    for (const auto &part : path.split('/', Qt::SkipEmptyParts)) {
        current = QDir(current).filePath(part);
        if (QFileInfo(current).isSymbolicLink()) return false;
    }
    return true;
}
QByteArray fileHash(const QString &path, QString *error) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        const QFileInfo before(path);
        const auto beforeSize = before.size();
        const auto beforeModified = before.lastModified().toMSecsSinceEpoch();
        QFile file(path);
        if (!before.isFile() || !noLinks(path) || !file.open(QIODevice::ReadOnly)) {
            *error = "副本不可读取或是链接"; return {};
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file) || file.error() != QFileDevice::NoError) { *error = "副本校验读取失败"; return {}; }
        const QFileInfo after(path);
        if (after.size() != beforeSize || after.lastModified().toMSecsSinceEpoch() != beforeModified) {
            // SMB may deliver the final write's attributes after close. Never accept
            // an unstable pass: reopen and fully rehash after attributes settle.
            if (attempt < 2) { file.close(); QThread::msleep(100); continue; }
            *error = QString("副本在校验期间发生变化（字节 %1→%2，时间 %3→%4）")
                .arg(beforeSize).arg(after.size()).arg(beforeModified).arg(after.lastModified().toMSecsSinceEpoch());
            return {};
        }
        return hash.result().toHex();
    }
    return {};
}
bool saveJson(const QString &path, const QJsonObject &data, QString *error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(data).toJson()) < 0 || !file.commit()) {
        *error = "处理记录保存失败：" + file.errorString(); return false;
    }
    return true;
}
QString keyFor(const Track &t) {
    const QJsonObject identity{{"library", t.libraryId}, {"path", t.relativePath},
                              {"root", t.root}, {"size", t.size}, {"modified", t.modified}};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),
                                                       QCryptographicHash::Sha256).toHex());
}
QString copyName(const QString &name) {
    QString clean = name.normalized(QString::NormalizationForm_C);
    if (clean.isEmpty() || clean == "." || clean == ".." || clean.contains('/') || clean.contains('\\')) return {};
    if (clean.toUtf8().size() <= 180) return clean;
    const auto suffix = QFileInfo(clean).suffix().left(12);
    while (clean.toUtf8().size() > 150) clean.chop(1);
    if (!clean.isEmpty() && clean.back().isHighSurrogate()) clean.chop(1);
    return clean + (suffix.isEmpty() ? QString() : "." + suffix);
}
std::optional<NasRepairFile> resolve(PathRepairBackend &nas, const QString &serverRoot,
                                    const QString &relative, QString *error) {
    QString path = serverRoot;
    const auto parts = relative.split('/');
    NasRepairFile found;
    for (int i = 0; i < parts.size(); ++i) {
        const auto entries = nas.list(path, error);
        if (!error->isEmpty()) return {};
        QList<NasRepairFile> matches;
        for (const auto &entry : entries)
            if (entry.name.normalized(QString::NormalizationForm_C) == parts[i].normalized(QString::NormalizationForm_C))
                matches << entry;
        if (matches.size() != 1) {
            *error = matches.isEmpty() ? "NAS 端未找到对应路径" : "规范化后存在多个同名路径，未猜选";
            return {};
        }
        found = matches.first();
        if (found.path != path + '/' + found.name || found.name.contains('/') || found.name == ".." ||
            found.name == "." || (i + 1 < parts.size() && !found.directory) ||
            (i + 1 == parts.size() && found.directory)) {
            *error = "NAS 返回的路径或文件类型不一致"; return {};
        }
        path = found.path;
    }
    return found;
}
} // namespace

QList<Track> pathRepairCandidates(const QString &indexPath, const QString &failurePath,
                                 const QString &library, QString *error) {
    FailureStore failures(failurePath, indexPath);
    if (!failures.open(error)) return {};
    QList<Track> candidates;
    QSet<QString> seen;
    for (bool references : {false, true}) {
        const auto upper = failures.retryUpper(error, references);
        if (!error->isEmpty()) return {};
        qint64 cursor = 0;
        while (cursor < upper) {
            const auto page = failures.retryPage(cursor, upper, 100, error, references);
            if (!error->isEmpty()) return {};
            if (page.isEmpty()) break;
            for (const auto &record : page) {
                cursor = qMax(cursor, record.id);
                const auto &t = record.track;
                const auto key = keyFor(t);
                if ((record.kind == "source_unavailable" || record.kind == "compatible_unavailable") &&
                    (library.isEmpty() || t.libraryId == library) &&
                    !seen.contains(key)) { seen.insert(key); candidates << t; }
            }
        }
    }
    return candidates;
}

PathRepairResult runPathRepair(const PathRepairOptions &o, const QList<Track> &tracks,
                              PathRepairBackend &nas, std::atomic_bool *pause,
                              const std::function<void(const QJsonObject &)> &progress) {
    PathRepairResult result;
    result.total = tracks.size();
    if (!cleanAbsolute(o.mountRoot) || !cleanAbsolute(o.serverRoot) || !cleanAbsolute(o.outputRoot) ||
        !within(o.outputRoot, o.mountRoot) || o.outputRoot == o.mountRoot ||
        !QFileInfo(o.mountRoot).isDir() || !noLinks(o.mountRoot) || !noLinks(o.outputRoot)) {
        result.error = "挂载目录、NAS 目录或副本目录无效，不能使用链接或共享根目录作为副本目录"; return result;
    }
    for (const auto &library : o.libraries) {
        for (const auto &protectedRoot : {library.root, library.sharedCache}) {
            if (!protectedRoot.isEmpty() &&
                (within(o.outputRoot, QDir::cleanPath(protectedRoot)) || within(QDir::cleanPath(protectedRoot), o.outputRoot))) {
                result.error = "副本目录与原素材库或共享缓存重叠，请选择独立目录"; return result;
            }
        }
    }
    Index index(o.indexPath);
    if (!index.open(&result.error)) return result;
    if (!QDir().mkpath(o.outputRoot) || !QDir().mkpath(QFileInfo(o.checkpointPath).absolutePath())) {
        result.error = "不能创建副本目录或本机记录目录"; return result;
    }
    QLockFile lock(QDir(o.outputRoot).filePath(".yuanaudio-repair.lock"));
    if (!lock.tryLock()) { result.error = "这个副本目录已有处理任务，请稍后继续"; return result; }
    const auto markerPath = QDir(o.outputRoot).filePath(".yuanaudio-compatible.json");
    if (!QFile::exists(markerPath)) {
        if (QDir(o.outputRoot).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden).size() > 1) {
            result.error = "请选择空目录或由本软件管理的兼容副本目录"; return result;
        }
        if (!saveJson(markerPath, {{"format", "yuanaudio-compatible"}, {"version", 1}}, &result.error)) return result;
    } else {
        QFile marker(markerPath);
        if (!marker.open(QIODevice::ReadOnly) || QJsonDocument::fromJson(marker.readAll()).object()["format"].toString() != "yuanaudio-compatible") {
            result.error = "兼容目录标记无效"; return result;
        }
    }
    QJsonObject state{{"version", 1}, {"mountRoot", o.mountRoot}, {"serverRoot", o.serverRoot},
                      {"outputRoot", o.outputRoot}, {"entries", QJsonObject{}}};
    QFile existing(o.checkpointPath);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly) || existing.size() > 32 * 1024 * 1024) {
            result.error = "处理记录不可读取"; return result;
        }
        const auto saved = QJsonDocument::fromJson(existing.readAll()).object();
        if (saved["version"].toInt() != 1 || saved["mountRoot"] != state["mountRoot"] ||
            saved["serverRoot"] != state["serverRoot"] || saved["outputRoot"] != state["outputRoot"]) {
            result.error = "继续记录的目录配置不同，请恢复原配置或选择新的副本目录"; return result;
        }
        state = saved;
    }
    auto entries = state["entries"].toObject();
    for (const auto &t : tracks) {
        if (pause && pause->load()) { result.paused = true; break; }
        QString error;
        const auto key = keyFor(t);
        auto entry = entries[key].toObject();
        auto report = [&] {
            if (progress) progress({{"done", result.done}, {"total", result.total},
                                    {"succeeded", result.succeeded}, {"failed", result.failed},
                                    {"current", t.name}, {"message", error}});
        };
        report();
        QString relative;
        if (!safeRelative(t.relativePath) || !within(t.root, o.mountRoot)) error = "原文件相对路径或挂载范围无效";
        else relative = QDir(o.mountRoot).relativeFilePath(t.absolutePath());
        std::optional<NasRepairFile> source;
        if (error.isEmpty() && safeRelative(relative)) source = resolve(nas, o.serverRoot, relative, &error);
        else if (error.isEmpty()) error = "原文件路径越出挂载目录";
        if (source && ((t.size > 0 && source->size != t.size) ||
                       (t.modified > 0 && source->modified != t.modified))) error = "NAS 原件版本与索引不一致，请定点刷新后重试";
        bool storageStop = false;
        QString destination;
        QByteArray sha;
        if (error.isEmpty() && entry["status"].toString() == "verified" &&
            entry["source"].toString() == source->path && entry["sourceSize"].toInteger() == source->size &&
            entry["sourceModified"].toInteger() == source->modified) {
            destination = entry["destination"].toString();
            if (!within(destination, o.outputRoot) || !noLinks(destination)) error = "继续记录的副本路径无效";
            else {
                sha = fileHash(destination, &error);
                if (!error.isEmpty() || sha != entry["sha256"].toString().toLatin1()) {
                    // Keep the old copy and its evidence. A new UUID directory is
                    // used below, so repair never overwrites a changed file.
                    destination.clear(); sha.clear(); error.clear();
                }
            }
        }
        if (error.isEmpty() && destination.isEmpty()) {
            const QStorageInfo space(o.outputRoot);
            if (space.isValid() && space.bytesAvailable() >= 0 &&
                space.bytesAvailable() < source->size + 2LL * 1024 * 1024 * 1024) {
                result.paused = true; result.error = "副本盘空间不足，保留已完成记录，释放空间后可继续"; break;
            }
            const auto name = copyName(source->name);
            if (name.isEmpty()) error = "文件名称无法安全用于副本";
            const auto folder = QDir(o.outputRoot).filePath("files/" + key.left(2) + '/' + key + '/' +
                                  QUuid::createUuid().toString(QUuid::WithoutBraces));
            if (error.isEmpty() && (!noLinks(folder) || !QDir().mkpath(folder))) error = "副本目录创建失败";
            destination = QDir(folder).filePath(name);
            const auto partial = QDir(folder).filePath(".download-partial");
            QByteArray downloaded;
            if (error.isEmpty() && nas.download(source->path, partial, &downloaded, &error)) {
                sha = fileHash(partial, &error);
                const auto expected = downloaded.size() == 32 ? downloaded.toHex() : downloaded.toLower();
                if (error.isEmpty() && (QFileInfo(partial).size() != source->size || sha != expected)) error = "原件下载与 SMB 副本完整校验不一致";
                if (error.isEmpty() && !QFile::rename(partial, destination)) error = "副本命名失败，未覆盖已有文件";
            } else if (error.isEmpty()) error = "原件下载失败";
            storageStop = nas.storageFailure();
            if (QFile::exists(partial)) QFile::remove(partial); // only this task's own newly-created partial
        }
        if (error.isEmpty()) {
            // Fresh listing, not the preflight snapshot, must still identify the original version.
            const auto current = nas.list(QFileInfo(source->path).path(), &error);
            bool stable = false;
            for (const auto &f : current)
                if (f.path == source->path && !f.directory && f.size == source->size && f.modified == source->modified) stable = true;
            if (error.isEmpty() && !stable) error = "NAS 原件在处理期间发生变化，未接入副本";
            if (error.isEmpty() && !index.setCompatibleSource(t, destination, sha, &error)) {}
        }
        ++result.done;
        if (error.isEmpty()) {
            ++result.succeeded;
            if (const auto mapped = index.trackByIdentity(t.libraryId, t.relativePath))
                result.repairedTracks << *mapped;
        }
        else { ++result.failed; result.lastError = error; }
        entry = {{"status", error.isEmpty() ? "verified" : "failed"}, {"library", t.libraryId},
                 {"relative", t.relativePath}, {"originalSize", t.size}, {"originalModified", t.modified},
                 {"destination", destination}, {"sha256", QString::fromLatin1(sha)}, {"error", error}};
        if (source) { entry["source"] = source->path; entry["sourceSize"] = source->size; entry["sourceModified"] = source->modified; }
        entries[key] = entry;
        state["entries"] = entries;
        if (!saveJson(o.checkpointPath, state, &result.error)) break;
        report();
        if (storageStop) {
            result.paused = true;
            result.error = "副本写入失败，已保存记录并暂停；检查副本盘空间及写入权限后继续";
            break;
        }
    }
    return result;
}
