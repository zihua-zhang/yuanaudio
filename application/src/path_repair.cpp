#include "path_repair.h"
#include "failure_store.h"
#include "file_publication.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
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
            // A second SMB session may have just created this name. Give the
            // mounted client a bounded visibility window, without accepting an
            // incomplete hash or rewriting the source.
            if (attempt < 2 && noLinks(path)) { QThread::msleep(150); continue; }
            *error = "兼容文件不可读取或是链接"; return {};
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

std::optional<NasRepairFile> PathRepairBackend::stat(const QString &path, QString *error) {
    if (error) error->clear();
    QString detail;
    const auto files = list(QFileInfo(path).path(), &detail);
    if (detail.isEmpty())
        for (const auto &file : files)
            if (file.path == path) return file;
    if (error) *error = detail.isEmpty() ? QString("NAS 原件不存在或不可访问") : detail;
    return {};
}
bool PathRepairBackend::createHardLink(const QString &, const QString &, QString *error) {
    if (error) *error = "当前连接不支持创建硬链接";
    return false;
}

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

static PathRepairResult runCopyRepair(const PathRepairOptions &o, const QList<Track> &tracks,
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

namespace {
bool sameSource(const NasRepairFile &before, const NasRepairFile &after) {
    return !after.directory && before.path == after.path && before.size == after.size &&
           before.modified == after.modified && (!before.fileId || before.fileId == after.fileId);
}
bool sameEntrySource(const QJsonObject &entry, const NasRepairFile &source) {
    const auto fileId = entry["sourceFileId"].toString().toULongLong();
    return entry["source"].toString() == source.path && entry["sourceSize"].toInteger() == source.size &&
           entry["sourceModified"].toInteger() == source.modified && (!fileId || fileId == source.fileId) &&
           (entry["strategy"].toString() != "hardlink" || fileId > 0);
}
bool ownedOrEmpty(const QString &root, QString *error) {
    const auto markerPath = QDir(root).filePath(".yuanaudio-compatible.json");
    if (!noLinks(markerPath)) { *error = "兼容目录标记不能是链接"; return false; }
    if (!QFile::exists(markerPath)) {
        auto contents = QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        contents.removeAll(".yuanaudio-repair.lock");
        if (!contents.isEmpty()) { *error = "请选择空目录或由本软件管理的兼容目录"; return false; }
        return true;
    }
    QFile marker(markerPath);
    if (!marker.open(QIODevice::ReadOnly) || marker.size() > 4096 ||
        QJsonDocument::fromJson(marker.readAll()).object()["format"].toString() != "yuanaudio-compatible") {
        *error = "兼容目录标记无效"; return false;
    }
    return true;
}
bool managedRoot(const QString &root, QString *error, bool *unsafe = nullptr) {
    if (unsafe) *unsafe = false;
    error->clear();
    if (!noLinks(root) || (QFileInfo::exists(root) && !QFileInfo(root).isDir()) || !QDir().mkpath(root)) {
        *error = "不能创建或写入兼容目录"; return false;
    }
    QLockFile lock(QDir(root).filePath(".yuanaudio-repair.lock"));
    if (!lock.tryLock()) { *error = "兼容目录暂时不可写或已有准备任务"; return false; }
    if (!ownedOrEmpty(root, error)) { if (unsafe) *unsafe = true; return false; }
    const auto markerPath = QDir(root).filePath(".yuanaudio-compatible.json");
    if (!QFile::exists(markerPath)) {
        return saveJson(markerPath, {{"format", "yuanaudio-compatible"}, {"version", 2}}, error);
    }
    return true;
}
bool localStorage(const QString &root) {
    auto existing = root;
    while (!QFileInfo::exists(existing)) {
        const auto parent = QFileInfo(existing).absolutePath();
        if (parent == existing) return false;
        existing = parent;
    }
    const QStorageInfo volume(existing);
    const auto type = volume.fileSystemType().toLower();
    return volume.isValid() && !type.contains("smb") && !type.contains("cifs") &&
           !type.contains("nfs") && !type.contains("afp") && !type.contains("sshfs") &&
           !type.contains("webdav") && !type.contains("davfs") && type != "9p";
}
QString newDestination(const QString &root, const QString &key, const QString &name, QString *error) {
    const auto folder = QDir(root).filePath("files/" + key.left(2) + '/' + key + '/' +
                                          QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!noLinks(folder) || !QDir().mkpath(folder) || !noLinks(folder)) {
        *error = "兼容文件目录创建失败"; return {};
    }
    const auto path = QDir(folder).filePath(name);
    // Each attempt owns a fresh UUID directory. Do not prime the SMB mounted
    // client's negative-name cache just before a separate SMB session creates
    // the alias. The actual SMB link and local publication both reject replace.
    return path;
}
QString remoteAlias(const PathRepairOptions &o, const QString &path) {
    const auto relative = QDir(o.mountRoot).relativeFilePath(path);
    return safeRelative(relative) ? o.serverRoot + '/' + relative : QString();
}
bool verifyLink(PathRepairBackend &nas, const NasRepairFile &source, const QString &alias, QString *error) {
    const auto linked = nas.stat(alias, error);
    if (!linked) return false;
    const auto current = nas.stat(source.path, error);
    if (!current) return false;
    if (!source.fileId || !sameSource(source, *current) || linked->directory || linked->path != alias ||
        linked->fileId != source.fileId || linked->size != source.size || linked->modified != source.modified ||
        linked->links < 2 || current->links < 2) {
        *error = "硬链接与原件的文件标识、版本或链接数不一致"; return false;
    }
    return true;
}
} // namespace

PathRepairResult runPathRepair(const PathRepairOptions &o, const QList<Track> &tracks,
                              PathRepairBackend &nas, std::atomic_bool *pause,
                              const std::function<void(const QJsonObject &)> &progress) {
    if (!o.preferHardLinks) return runCopyRepair(o, tracks, nas, pause, progress);
    PathRepairResult result;
    result.total = tracks.size();
    if (!cleanAbsolute(o.mountRoot) || !cleanAbsolute(o.serverRoot) || !cleanAbsolute(o.outputRoot) ||
        !cleanAbsolute(o.localCacheRoot) || !within(o.outputRoot, o.mountRoot) || o.outputRoot == o.mountRoot ||
        within(o.localCacheRoot, o.mountRoot) || within(o.mountRoot, o.localCacheRoot) ||
        !QFileInfo(o.mountRoot).isDir() || !noLinks(o.mountRoot) || !noLinks(o.outputRoot) ||
        !noLinks(o.localCacheRoot) || !localStorage(o.localCacheRoot)) {
        result.error = "NAS 兼容目录须在挂载内，本机缓存须在本机磁盘且位于挂载外，不能使用链接目录"; return result;
    }
    QStringList protectedRoots;
    for (const auto &library : o.libraries)
        for (const auto &root : {library.root, library.sharedCache}) {
            if (root.isEmpty()) continue;
            const auto clean = QDir::cleanPath(root);
            protectedRoots.append(clean);
            if (within(o.outputRoot, clean) || within(clean, o.outputRoot) ||
                within(o.localCacheRoot, clean) || within(clean, o.localCacheRoot)) {
                result.error = "兼容目录或本机缓存与原素材库、共享缓存重叠"; return result;
            }
        }
    protectedRoots.removeDuplicates(); protectedRoots.sort();
    // Enforce ownership even when all items can use hard links, and before a
    // resumed local-cache entry can bypass directory preparation.
    if (QFileInfo(o.localCacheRoot).isDir() && !ownedOrEmpty(o.localCacheRoot, &result.error)) return result;
    if (!QDir().mkpath(QFileInfo(o.checkpointPath).absolutePath())) {
        result.error = "不能创建本机处理记录目录"; return result;
    }
    QLockFile jobLock(o.checkpointPath + ".lock");
    if (!jobLock.tryLock()) { result.error = "这个修复任务正在处理，请稍后继续"; return result; }
    QJsonObject state{{"version", 2}, {"mode", "hardlink-local-cache"}, {"mountRoot", o.mountRoot},
                      {"serverRoot", o.serverRoot}, {"serverIdentity", o.serverIdentity},
                      {"outputRoot", o.outputRoot}, {"localCacheRoot", o.localCacheRoot},
                      {"indexPath", o.indexPath}, {"protectedRoots", QJsonArray::fromStringList(protectedRoots)},
                      {"entries", QJsonObject{}}};
    QFile existing(o.checkpointPath);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly) || existing.size() > 32 * 1024 * 1024) {
            result.error = "处理记录不可读取"; return result;
        }
        const auto saved = QJsonDocument::fromJson(existing.readAll()).object();
        for (const auto &key : {"version", "mode", "mountRoot", "serverRoot", "serverIdentity", "outputRoot",
                               "localCacheRoot", "indexPath", "protectedRoots"})
            if (saved[key] != state[key]) {
                result.error = "继续记录的模式、服务器或目录配置不同，请恢复原配置或使用新的处理记录"; return result;
            }
        state = saved;
    }
    Index index(o.indexPath);
    if (!index.open(&result.error)) return result;
    QString nasRootError;
    bool unsafeNasRoot = false;
    const bool nasReady = managedRoot(o.outputRoot, &nasRootError, &unsafeNasRoot);
    if (unsafeNasRoot) { result.error = nasRootError; return result; }
    bool localReady = false;
    auto entries = state["entries"].toObject();
    if (!saveJson(o.checkpointPath, state, &result.error)) { result.paused = true; return result; }
    for (const auto &t : tracks) {
        if (pause && pause->load()) { result.paused = true; break; }
        QString error, destination, strategy, fallbackReason, uncertainAlias;
        QByteArray sha;
        bool storageStop = false, reused = false;
        const auto key = keyFor(t);
        auto entry = entries[key].toObject();
        auto previousReceipts = entry["previousReceipts"].toArray();
        fallbackReason = entry["fallbackReason"].toString();
        uncertainAlias = entry["unconfirmedAlias"].toString();
        auto report = [&] {
            if (progress) progress({{"done", result.done}, {"total", result.total}, {"succeeded", result.succeeded},
                                    {"failed", result.failed}, {"hardlinked", result.hardlinked}, {"cached", result.cached},
                                    {"reused", result.reused}, {"reusedEntry", reused}, {"fallbackReason", fallbackReason},
                                    {"current", t.name}, {"strategy", strategy}, {"message", error}});
        };
        report();
        QString relative;
        if (!safeRelative(t.relativePath) || !within(t.root, o.mountRoot)) error = "原文件相对路径或挂载范围无效";
        else relative = QDir(o.mountRoot).relativeFilePath(t.absolutePath());
        std::optional<NasRepairFile> source;
        if (error.isEmpty() && safeRelative(relative)) {
            const auto listed = resolve(nas, o.serverRoot, relative, &error);
            if (listed) {
                source = nas.stat(listed->path, &error);
                if (source && !sameSource(*listed, *source)) error = "NAS 原件在定位期间发生变化";
            }
        } else if (error.isEmpty()) error = "原文件路径越出挂载目录";
        if (source && ((t.size > 0 && source->size != t.size) ||
                       (t.modified > 0 && source->modified != t.modified))) error = "NAS 原件版本与索引不一致，请定点刷新后重试";
        if (error.isEmpty() && !source) error = "NAS 原件不可访问";
        if (source && entry["status"].toString() == "verified" && !sameEntrySource(entry, *source)) {
            error = "原件文件标识或版本已改变，保留旧结果；请核对原件并定点确认新版本后重试";
            entry["lastAttemptError"] = error;
            entry["observedSourceFileId"] = QString::number(source->fileId);
            entries[key] = entry; state["entries"] = entries;
            ++result.done; ++result.failed; result.lastError = error;
            if (!saveJson(o.checkpointPath, state, &result.error)) result.paused = true;
            report(); if (result.paused) break; continue;
        }
        if (error.isEmpty() && entry["status"].toString() == "verified" && sameEntrySource(entry, *source)) {
            strategy = entry["strategy"].toString(); destination = entry["destination"].toString();
            const auto root = strategy == "hardlink" ? o.outputRoot : strategy == "local_cache" ? o.localCacheRoot : QString();
            if (!root.isEmpty() && within(destination, root) && destination != root && noLinks(destination)) {
                sha = fileHash(destination, &error);
                if (error.isEmpty() && sha == entry["sha256"].toString().toLatin1() &&
                    (strategy != "hardlink" || verifyLink(nas, *source, remoteAlias(o, destination), &error)))
                    reused = true;
            }
            if (!reused) {
                if (strategy == "local_cache" && !entry["cacheRepairPending"].toBool()) {
                    error = "已验证的本机缓存失效，已保留旧文件和记录并暂停；再次点击开始 / 继续仅重建失效项";
                    entry["cacheRepairPending"] = true; entry["integrityError"] = error;
                    entries[key] = entry; state["entries"] = entries;
                    ++result.done; ++result.failed; result.lastError = error; result.paused = true;
                    saveJson(o.checkpointPath, state, &result.error); report(); break;
                }
                auto evidence = entry; evidence.remove("previousReceipts");
                previousReceipts.append(evidence);
                destination.clear(); sha.clear(); strategy.clear(); error.clear();
            }
        }
        if (error.isEmpty() && !reused) {
            const auto name = copyName(source->name);
            if (name.isEmpty()) error = "文件名称无法安全用于兼容文件";
            if (error.isEmpty() && nasReady && source->fileId) {
                QString linkError;
                const auto aliasPath = newDestination(o.outputRoot, key, name, &linkError);
                uncertainAlias = aliasPath;
                const auto aliasRemote = remoteAlias(o, aliasPath);
                if (!aliasPath.isEmpty() && !aliasRemote.isEmpty() &&
                    nas.createHardLink(source->path, aliasRemote, &linkError) &&
                    verifyLink(nas, *source, aliasRemote, &linkError)) {
                    const auto linkedSha = fileHash(aliasPath, &linkError);
                    if (!linkedSha.isEmpty() && verifyLink(nas, *source, aliasRemote, &linkError)) {
                        destination = aliasPath; sha = linkedSha; strategy = "hardlink";
                    }
                }
                if (destination.isEmpty()) fallbackReason = linkError;
                if (nas.sessionUnavailable()) error = "NAS 连接已中断，保留可能建立的入口；重新登录后继续";
                // Unusable aliases remain as evidence. Never unlink or overwrite a created alias.
            }
            if (error.isEmpty() && destination.isEmpty()) {
                strategy = "local_cache";
                if (!localReady) localReady = managedRoot(o.localCacheRoot, &error);
                if (!localReady) storageStop = true;
                if (error.isEmpty()) {
                    const QStorageInfo space(o.localCacheRoot);
                    if (space.isValid() && space.bytesAvailable() >= 0 &&
                        space.bytesAvailable() < source->size + 2LL * 1024 * 1024 * 1024) {
                        error = "本机异常缓存空间不足，释放空间后可继续"; storageStop = true;
                    }
                }
                if (error.isEmpty()) {
                    destination = newDestination(o.localCacheRoot, key, name, &error);
                    if (destination.isEmpty()) storageStop = true;
                }
                if (error.isEmpty()) {
                    const auto partial = QDir(QFileInfo(destination).absolutePath()).filePath(".download-partial");
                    QByteArray downloaded;
                    if (nas.download(source->path, partial, &downloaded, &error)) {
                        sha = fileHash(partial, &error);
                        const auto expected = downloaded.size() == 32 ? downloaded.toHex() : downloaded.toLower();
                        if (error.isEmpty() && (QFileInfo(partial).size() != source->size || sha != expected))
                            error = "原件下载与本机缓存完整校验不一致";
                        if (error.isEmpty() && !publishFileExclusive(partial, destination)) {
                            error = "本机缓存保存失败，未覆盖已有文件"; storageStop = true;
                        }
                    } else if (error.isEmpty()) error = "原件下载失败";
                    storageStop |= nas.storageFailure();
                    if (QFile::exists(partial)) QFile::remove(partial); // only this UUID job's partial
                }
            }
        }
        if (error.isEmpty()) {
            const auto current = nas.stat(source->path, &error);
            if (error.isEmpty() && (!current || !sameSource(*source, *current)))
                error = "NAS 原件在处理期间发生变化，未接入兼容文件";
            if (error.isEmpty() && !index.setCompatibleSource(t, destination, sha, &error) && error.isEmpty())
                error = "索引未能保存兼容文件映射";
        }
        ++result.done;
        if (error.isEmpty()) {
            ++result.succeeded;
            if (reused) ++result.reused;
            else if (strategy == "hardlink") ++result.hardlinked;
            else if (strategy == "local_cache") ++result.cached;
            if (const auto mapped = index.trackByIdentity(t.libraryId, t.relativePath)) result.repairedTracks << *mapped;
        } else { ++result.failed; result.lastError = error; }
        const auto priorEntry = entry;
        entry = {{"status", error.isEmpty() ? "verified" : "failed"}, {"strategy", strategy}, {"library", t.libraryId},
                 {"relative", t.relativePath}, {"originalSize", t.size}, {"originalModified", t.modified},
                 {"destination", destination}, {"sha256", QString::fromLatin1(sha)}, {"error", error}};
        if (source) {
            entry["source"] = source->path; entry["sourceSize"] = source->size;
            entry["sourceModified"] = source->modified; entry["sourceFileId"] = QString::number(source->fileId);
        }
        if (!previousReceipts.isEmpty()) entry["previousReceipts"] = previousReceipts;
        if (!fallbackReason.isEmpty()) entry["fallbackReason"] = fallbackReason;
        if (!uncertainAlias.isEmpty() && strategy != "hardlink") entry["unconfirmedAlias"] = uncertainAlias;
        if (!error.isEmpty() && priorEntry["status"].toString() == "verified") {
            auto retained = priorEntry; retained["lastAttemptError"] = error;
            if (!uncertainAlias.isEmpty()) retained["unconfirmedAlias"] = uncertainAlias;
            entry = retained;
        }
        entries[key] = entry; state["entries"] = entries;
        if (!saveJson(o.checkpointPath, state, &result.error)) { result.paused = true; break; }
        report();
        if (nas.sessionUnavailable()) { result.paused = true; break; }
        if (storageStop) {
            result.paused = true; result.error = "本机异常缓存写入失败，已保存记录并暂停；检查本机磁盘空间和权限后继续"; break;
        }
    }
    return result;
}
