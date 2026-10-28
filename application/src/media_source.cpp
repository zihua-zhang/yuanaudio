#include "media_source.h"
#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QLockFile>
#include <QMutex>
#include <QSqlQuery>
#include <QSysInfo>
#include <QUuid>
#include <memory>
#include <mutex>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
struct Fingerprint {
    bool regular = false;
    // Empty means this platform/filesystem did not supply a reliable identity.
    QByteArray identity;
};
#ifdef Q_OS_UNIX
Fingerprint fingerprint(const struct stat &state) {
    Fingerprint result;
    result.regular = S_ISREG(state.st_mode);
#ifdef Q_OS_MACOS
    const auto modified = state.st_mtimespec, changed = state.st_ctimespec;
#else
    const auto modified = state.st_mtim, changed = state.st_ctim;
#endif
    if (!result.regular || !state.st_dev || !state.st_ino || state.st_size < 0 ||
        modified.tv_nsec < 0 || modified.tv_nsec >= 1000000000 ||
        changed.tv_nsec < 0 || changed.tv_nsec >= 1000000000 || !changed.tv_sec)
        return result;
    result.identity = QByteArray::number(quint64(state.st_dev)) + ':' +
                      QByteArray::number(quint64(state.st_ino)) + ':' +
                      QByteArray::number(qint64(state.st_size)) + ':' +
                      QByteArray::number(qint64(modified.tv_sec)) + ':' +
                      QByteArray::number(qint64(modified.tv_nsec)) + ':' +
                      QByteArray::number(qint64(changed.tv_sec)) + ':' +
                      QByteArray::number(qint64(changed.tv_nsec));
    return result;
}
#endif
Fingerprint openedFingerprint(const QFile &file) {
#ifdef Q_OS_UNIX
    struct stat state {};
    if (::fstat(file.handle(), &state) == 0)
        return fingerprint(state);
    return {};
#else
    // Portable QFile metadata lacks a stable device/inode/ctime identity.
    // These platforms still hash the entire content without issuing receipts.
    return {QFileInfo(file).isFile(), {}};
#endif
}
bool pathStillMatches(const QString &path, const Fingerprint &opened) {
#ifdef Q_OS_UNIX
    struct stat state {};
    if (::lstat(QFile::encodeName(path).constData(), &state) != 0)
        return false;
    const auto current = fingerprint(state);
    return current.regular && (opened.identity.isEmpty() || current.identity == opened.identity);
#else
    Q_UNUSED(opened);
    const QFileInfo current(path);
    return current.isFile() && !current.isSymbolicLink();
#endif
}
bool openCompatibleFile(QFile &file, const QString &path) {
#ifdef Q_OS_UNIX
    // A path swapped to a link or FIFO must not follow/hold the open.
    int flags = O_RDONLY | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int descriptor = ::open(QFile::encodeName(path).constData(), flags);
    if (descriptor < 0)
        return false;
    if (file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
        return true;
    ::close(descriptor);
    return false;
#else
    file.setFileName(path);
    return file.open(QIODevice::ReadOnly);
#endif
}

// Only mutex acquisition holds the registry lock; long hashes lock one file.
// Remove expired entries on acquisition to keep the registry bounded by callers.
std::shared_ptr<std::mutex> fileMutex(const QString &key) {
    static std::mutex registryMutex;
    static QHash<QString, std::weak_ptr<std::mutex>> registry;
    std::lock_guard lock(registryMutex);
    for (auto it = registry.begin(); it != registry.end();)
        if (it.value().expired())
            it = registry.erase(it);
        else
            ++it;
    auto result = registry.value(key).lock();
    if (!result) {
        result = std::make_shared<std::mutex>();
        registry.insert(key, result);
    }
    return result;
}
class MemoryReceipts {
  public:
    bool contains(const QString &key, const QByteArray &fingerprint) {
        QMutexLocker lock(&mutex_);
        const auto receipt = receipts_.object(key);
        return receipt && *receipt == fingerprint;
    }
    void remember(const QString &key, const QByteArray &fingerprint) {
        QMutexLocker lock(&mutex_);
        receipts_.insert(key, new QByteArray(fingerprint));
    }
    static MemoryReceipts &instance() {
        static MemoryReceipts receipts;
        return receipts;
    }
  private:
    QMutex mutex_;
    QCache<QString, QByteArray> receipts_{256};
};

class ReceiptStore {
  public:
    explicit ReceiptStore(const QString &path) {
        if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
            return;
        const QFileInfo info(path), directory(info.absolutePath());
        const auto writeBits = QFileDevice::WriteOwner | QFileDevice::WriteUser |
                               QFileDevice::WriteGroup | QFileDevice::WriteOther;
        // A read-only/broken store must not become an alternate success path.
        if ((info.exists() && (!info.isFile() || info.isSymbolicLink() || !info.isWritable() ||
                               !(info.permissions() & writeBits))) ||
            !directory.isWritable() || !(directory.permissions() & writeBits))
            return;
        name_ = "media-verification-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        db_ = QSqlDatabase::addDatabase("QSQLITE", name_);
        db_.setDatabaseName(path);
        db_.setConnectOptions("QSQLITE_BUSY_TIMEOUT=150");
        if (!db_.open())
            return;
        QSqlQuery query(db_);
        auto scalar = [&](const QString &sql, int &value) {
            if (!query.exec(sql) || !query.next())
                return false;
            value = query.value(0).toInt();
            query.finish();
            return true;
        };
        int owner = 0, version = 0, tables = 0;
        if (!scalar("PRAGMA application_id", owner) || !scalar("PRAGMA user_version", version) ||
            !scalar("SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'", tables))
            return;
        if (!owner && !version && !tables) {
            // Claim only a truly empty DB, never migrate/recreate an unknown DB.
            if (!query.exec("BEGIN IMMEDIATE"))
                return;
            // A different file's verifier may have initialized the shared DB
            // while this short write lock was pending. Recheck under the lock.
            if (!scalar("PRAGMA application_id", owner) || !scalar("PRAGMA user_version", version) ||
                !scalar("SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'", tables)) {
                db_.rollback();
                return;
            }
            if (!owner && !version && !tables) {
                if (!query.exec("CREATE TABLE media_verification_receipts("
                                "path TEXT PRIMARY KEY, expected_sha BLOB NOT NULL, fingerprint BLOB NOT NULL) "
                                "WITHOUT ROWID") ||
                    !query.exec(QString("PRAGMA application_id=%1").arg(ApplicationId)) ||
                    !query.exec("PRAGMA user_version=1")) {
                    db_.rollback();
                    return;
                }
                owner = ApplicationId;
                version = 1;
                tables = 1;
            }
            if (!db_.commit())
                return;
        }
        usable_ = owner == ApplicationId && version == 1 && tables == 1;
    }
    ~ReceiptStore() {
        if (name_.isEmpty())
            return;
        db_.close();
        db_ = {};
        QSqlDatabase::removeDatabase(name_);
    }
    bool contains(const QString &path, const QByteArray &expected, const QByteArray &fingerprint) {
        if (!usable_)
            return false;
        QSqlQuery query(db_);
        query.prepare("SELECT expected_sha,fingerprint FROM media_verification_receipts WHERE path=?");
        query.addBindValue(path);
        return query.exec() && query.next() && query.value(0).toByteArray() == expected &&
               query.value(1).toByteArray() == fingerprint;
    }
    void remember(const QString &path, const QByteArray &expected, const QByteArray &fingerprint) {
        if (!usable_)
            return;
        QSqlQuery query(db_);
        query.prepare("INSERT INTO media_verification_receipts(path,expected_sha,fingerprint) VALUES(?,?,?) "
                      "ON CONFLICT(path) DO UPDATE SET expected_sha=excluded.expected_sha,"
                      "fingerprint=excluded.fingerprint");
        query.addBindValue(path);
        query.addBindValue(expected);
        query.addBindValue(fingerprint);
        query.exec(); // Cache write failure cannot invalidate verified playback.
    }
  private:
    static constexpr int ApplicationId = 0x59564d31;
    QString name_;
    QSqlDatabase db_;
    bool usable_ = false;
};
} // namespace

QString verifiedMediaPath(const Track &track, QString *error,
                          const std::function<void(qint64)> &progress,
                          const QString &verificationCachePath) {
    if (error)
        error->clear();
    if (track.compatiblePath.isEmpty())
        return track.absolutePath();
    if (progress)
        progress(0);
    if (!track.sourceMatches(error))
        return {};
    const auto expected = track.compatibleSha256.toLower();
    auto fail = [&] {
        if (error)
            *error = "兼容文件的完整 SHA-256 校验未通过，请重新修复原曲路径。";
        return QString();
    };
    if (expected.size() != 64 || QByteArray::fromHex(expected).toHex() != expected)
        return fail();
    const auto path = QFileInfo(track.compatiblePath).absoluteFilePath();
    const auto cachePath = verificationCachePath.isEmpty()
                               ? QString() : QFileInfo(verificationCachePath).absoluteFilePath();
    const auto key = path + QChar(0) + QString::fromLatin1(expected);
    const auto mutex = fileMutex(cachePath + QChar(0) + key);
    std::lock_guard fileLock(*mutex);
    std::unique_ptr<QLockFile> processLock;
    if (!cachePath.isEmpty() && QDir().mkpath(cachePath + ".locks")) {
        const auto lockName = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex();
        processLock = std::make_unique<QLockFile>(cachePath + ".locks/" + QString::fromLatin1(lockName) + ".lock");
        processLock->setStaleLockTime(0); // A live SMB hash may legitimately take minutes.
        while (!processLock->tryLock(1000)) {
            qint64 owner = 0;
            QString host, application;
            if (processLock->error() != QLockFile::LockFailedError ||
                !processLock->getLockInfo(&owner, &host, &application) || owner <= 0 ||
                host != QSysInfo::machineHostName()) {
                processLock.reset(); // A broken/unwritable lock cannot block playback.
                break;
            }
            // Wait only for this file's first verifier, including slow SMB reads.
            // A zero heartbeat never claims that this caller read audio content.
            if (progress)
                progress(0);
        }
    }
    if (!track.sourceMatches(error))
        return {};
    QFile file;
    if (!openCompatibleFile(file, path))
        return fail();
    const auto before = openedFingerprint(file);
    if (!before.regular || !pathStillMatches(path, before))
        return fail();
    ReceiptStore store(cachePath);
    const bool reliable = !before.identity.isEmpty();
    const bool hit = reliable &&
        (cachePath.isEmpty() ? MemoryReceipts::instance().contains(key, before.identity)
                            : store.contains(path, expected, before.identity));
    auto unchanged = [&] {
        const auto after = openedFingerprint(file);
        return after.regular && (!reliable || after.identity == before.identity) &&
               pathStillMatches(path, before);
    };
    if (!hit) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 read = 0;
        while (!file.atEnd()) {
            const auto chunk = file.read(256 * 1024);
            if (chunk.isEmpty() || file.error() != QFileDevice::NoError)
                return fail();
            hash.addData(chunk);
            read += chunk.size();
            if (progress)
                progress(read);
        }
        if (file.error() != QFileDevice::NoError || read != track.compatibleSize ||
            hash.result().toHex() != expected)
            return fail();
    }
    if (!unchanged())
        return fail();
    if (!track.sourceMatches(error))
        return {};
    if (!hit && reliable) {
        if (cachePath.isEmpty())
            MemoryReceipts::instance().remember(key, before.identity);
        else
            store.remember(path, expected, before.identity);
    }
    return track.sourcePath();
}
