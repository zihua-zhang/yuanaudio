#include "media_source.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>
#include <condition_variable>
#include <future>
#include <mutex>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
const QByteArray abcSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
using Progress = std::function<void(qint64)>;

QString verify(const Track &track, QString *error, const Progress &progress,
               const QString &cache = {}) {
    return verifiedMediaPath(track, error, progress, cache);
}
bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
Track mappedTrack(const QString &path, const QByteArray &sha = abcSha) {
    Track track;
    track.root = QFileInfo(path).absolutePath();
    track.relativePath = "unavailable-original.wav";
    track.compatiblePath = path;
    track.compatibleSize = QFileInfo(path).size();
    track.compatibleModified = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    track.compatibleSha256 = sha;
    return track;
}
struct Result {
    QString path, error;
    qint64 read = 0;
};
Result checked(const Track &track, const QString &cache = {}) {
    Result result;
    result.path = verify(track, &result.error, [&](qint64 bytes) { result.read = qMax(result.read, bytes); },
                         cache);
    return result;
}
QStringList childArguments(const Track &track, const QString &cache, int delay = 0) {
    return {"--verify-child", track.compatiblePath, QString::fromLatin1(track.compatibleSha256),
            QString::number(track.compatibleSize), QString::number(track.compatibleModified), cache,
            QString::number(delay)};
}
Result childResult(QProcess &child, int timeout = 15000) {
    if (!child.waitForFinished(timeout)) {
        child.kill();
        child.waitForFinished(5000);
        return {{}, "verification child timed out", -1};
    }
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode())
        return {{}, QString::fromUtf8(child.readAllStandardError()), -1};
    const auto json = QJsonDocument::fromJson(child.readAllStandardOutput()).object();
    if (json.isEmpty())
        return {{}, "invalid child result", -1};
    return {json.value("path").toString(), json.value("error").toString(),
            qint64(json.value("read").toDouble())};
}
Result childCheck(const Track &track, const QString &cache) {
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(), childArguments(track, cache));
    return childResult(child);
}
#ifdef Q_OS_UNIX
bool restoreMtime(const QString &path, const struct stat &before) {
#ifdef Q_OS_MACOS
    const struct timespec times[] = {before.st_atimespec, before.st_mtimespec};
#else
    const struct timespec times[] = {before.st_atim, before.st_mtim};
#endif
    return ::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, 0) == 0;
}
#endif
} // namespace

class MediaVerificationTests : public QObject {
    Q_OBJECT
  private slots:
    void firstFullHashThenUnchangedInProcessReuse() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        auto first = checked(track);
        QCOMPARE(first.path, path);
        QCOMPARE(first.read, qint64(3));
        const auto second = checked(track);
        QCOMPARE(second.path, path);
        QVERIFY(second.error.isEmpty());
        QCOMPARE(second.read, qint64(0));
    }
    void receiptSurvivesProcessRestart() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav");
        const auto cache = temp.filePath("media-verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        const auto first = childCheck(track, cache);
        QCOMPARE(first.path, path);
        QCOMPARE(first.read, qint64(3));
        const auto second = childCheck(track, cache);
        QCOMPARE(second.path, path);
        QCOMPARE(second.read, qint64(0));
    }
    void concurrentProcessesShareOnlyTheFirstFullHash() {
        QTemporaryDir temp;
        const QByteArray bytes(4 * 1024 * 1024, 'a');
        const auto path = temp.filePath("copy.wav");
        const auto cache = temp.filePath("media-verification.sqlite");
        QVERIFY(writeFile(path, bytes));
        const auto track = mappedTrack(path, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        QProcess children[4];
        for (auto &child : children)
            child.start(QCoreApplication::applicationFilePath(), childArguments(track, cache, 5));
        qint64 totalRead = 0;
        for (auto &child : children) {
            const auto result = childResult(child);
            QCOMPARE(result.path, path);
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            totalRead += result.read;
        }
        QCOMPARE(totalRead, qint64(bytes.size()));
    }
    void concurrentThreadsShareOnlyTheFirstFullHash() {
        QTemporaryDir temp;
        const QByteArray bytes(1024 * 1024, 'a');
        const auto path = temp.filePath("copy.wav");
        QVERIFY(writeFile(path, bytes));
        const auto track = mappedTrack(path, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        std::future<Result> calls[4];
        for (auto &call : calls)
            call = std::async(std::launch::async, [track] { return checked(track); });
        qint64 totalRead = 0;
        for (auto &call : calls) {
            const auto result = call.get();
            QCOMPARE(result.path, path);
            totalRead += result.read;
        }
        QCOMPARE(totalRead, qint64(bytes.size()));
    }
    void deadOrCorruptVerifierLockCannotBlockPlayback_data() {
        QTest::addColumn<bool>("corrupt");
        QTest::newRow("dead-owner") << false;
        QTest::newRow("corrupt-lock") << true;
    }
    void deadOrCorruptVerifierLockCannotBlockPlayback() {
        QFETCH(bool, corrupt);
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        auto arguments = childArguments(track, cache);
        arguments.last() = "hold";
        QProcess owner;
        owner.start(QCoreApplication::applicationFilePath(), arguments);
        QVERIFY(owner.waitForReadyRead(5000));
        QCOMPARE(owner.readAllStandardOutput(), QByteArray("hashing\n"));
        const auto lockFiles = QDir(cache + ".locks").entryList({"*.lock"}, QDir::Files);
        QCOMPARE(lockFiles.size(), 1);
        owner.kill();
        QVERIFY(owner.waitForFinished(5000));
        if (corrupt)
            QVERIFY(writeFile(cache + ".locks/" + lockFiles.first(), "broken lock data"));
        QProcess next;
        next.start(QCoreApplication::applicationFilePath(), childArguments(track, cache));
        const auto result = childResult(next, 3000);
        QCOMPARE(result.path, path);
        QCOMPARE(result.read, qint64(3));
    }
    void unrelatedFilesCanHashConcurrently() {
        QTemporaryDir temp;
        const auto firstPath = temp.filePath("first.wav"), secondPath = temp.filePath("second.wav");
        const auto cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(firstPath, "abc"));
        QVERIFY(writeFile(secondPath, "abc"));
        std::mutex mutex;
        std::condition_variable condition;
        bool firstReading = false, secondReading = false, overlapped = false;
        const auto firstTrack = mappedTrack(firstPath), secondTrack = mappedTrack(secondPath);
        auto first = std::async(std::launch::async, [&] {
            return verify(firstTrack, nullptr, [&](qint64 bytes) {
                if (!bytes)
                    return;
                std::unique_lock lock(mutex);
                firstReading = true;
                condition.notify_all();
                overlapped = condition.wait_for(lock, std::chrono::seconds(3), [&] { return secondReading; });
            }, cache);
        });
        {
            std::unique_lock lock(mutex);
            condition.wait_for(lock, std::chrono::seconds(3), [&] { return firstReading; });
        }
        auto second = std::async(std::launch::async, [&] {
            return verify(secondTrack, nullptr, [&](qint64 bytes) {
                if (bytes) {
                    std::lock_guard lock(mutex);
                    secondReading = true;
                    condition.notify_all();
                }
            }, cache);
        });
        QCOMPARE(first.get(), firstPath);
        QCOMPARE(second.get(), secondPath);
        QVERIFY(overlapped);
    }
    void sameSizeContentChangeWithRestoredMtimeInvalidatesReceipt() {
#ifndef Q_OS_UNIX
        QSKIP("Nanosecond ctime fixture uses POSIX metadata");
#else
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        QCOMPARE(checked(track, cache).path, path);
        struct stat before {};
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &before), 0);
        QTest::qWait(2);
        QVERIFY(writeFile(path, "axc"));
        QVERIFY(restoreMtime(path, before));
        QVERIFY(track.sourceMatches());
        const auto changed = checked(track, cache);
        QVERIFY(changed.path.isEmpty());
        QVERIFY(!changed.error.isEmpty());
        QCOMPARE(changed.read, qint64(3));
        QVERIFY(checked(track, cache).path.isEmpty());
#endif
    }
    void replacedInodeWithSameSizeAndMtimeRequiresFullHash() {
#ifndef Q_OS_UNIX
        QSKIP("Inode fixture uses POSIX metadata");
#else
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        QCOMPARE(checked(track, cache).path, path);
        struct stat before {}, after {};
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &before), 0);
        QVERIFY(QFile::rename(path, temp.filePath("old.wav")));
        QVERIFY(writeFile(path, "abc"));
        QVERIFY(restoreMtime(path, before));
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &after), 0);
        QVERIFY(before.st_ino != after.st_ino);
        const auto changed = checked(track, cache);
        QCOMPARE(changed.path, path);
        QCOMPARE(changed.read, qint64(3));
        QCOMPARE(checked(track, cache).read, qint64(0));
#endif
    }
    void changedExpectedShaCannotUseOldReceipt() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        auto track = mappedTrack(path);
        QCOMPARE(checked(track, cache).path, path);
        track.compatibleSha256 = QByteArray(64, '0');
        const auto changed = checked(track, cache);
        QVERIFY(changed.path.isEmpty());
        QCOMPARE(changed.read, qint64(3));
        track.compatibleSha256 = "not-a-valid-sha";
        const auto invalid = checked(track, cache);
        QVERIFY(invalid.path.isEmpty());
        QCOMPARE(invalid.read, qint64(0));
    }
    void pathIsBoundToReceipt() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), other = temp.filePath("other.wav");
        const auto cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        QCOMPARE(checked(mappedTrack(path), cache).path, path);
        QVERIFY(QFile::rename(path, other));
        const auto result = checked(mappedTrack(other), cache);
        QCOMPARE(result.path, other);
        QCOMPARE(result.read, qint64(3));
    }
    void metadataChangeDuringFullHashCannotProduceReceipt() {
#ifndef Q_OS_UNIX
        QSKIP("Nanosecond ctime fixture uses POSIX metadata");
#else
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        struct stat before {};
        QCOMPARE(::stat(QFile::encodeName(path).constData(), &before), 0);
        bool changed = false;
        QString error;
        const auto result = verify(track, &error, [&](qint64 bytes) {
            if (bytes && !changed) {
                changed = writeFile(path, "axc") && restoreMtime(path, before);
            }
        }, cache);
        QVERIFY(changed);
        QVERIFY(result.isEmpty());
        QVERIFY(!error.isEmpty());
        const auto next = checked(track, cache);
        QVERIFY(next.path.isEmpty());
        QCOMPARE(next.read, qint64(3));
#endif
    }
    void missingReceiptRequiresFullHash() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        QCOMPARE(checked(track, cache).path, path);
        QVERIFY(QFile::remove(cache));
        const auto result = checked(track, cache);
        QCOMPARE(result.path, path);
        QCOMPARE(result.read, qint64(3));
    }
    void corruptCacheCannotBypassFullHashOrBeOverwritten() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        QCOMPARE(checked(track, cache).path, path);
        const QByteArray broken = "unrelated corrupt cache bytes";
        QVERIFY(writeFile(cache, broken));
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto result = checked(track, cache);
            QCOMPARE(result.path, path);
            QCOMPARE(result.read, qint64(3));
        }
        QFile file(cache);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), broken);
    }
    void unusableCacheDoesNotBreakVerifiedPlayback() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), parent = temp.filePath("not-a-directory");
        QVERIFY(writeFile(path, "abc"));
        QVERIFY(writeFile(parent, "parent is a file"));
        const auto track = mappedTrack(path);
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto result = checked(track, parent + "/verification.sqlite");
            QCOMPARE(result.path, path);
            QVERIFY(result.error.isEmpty());
            QCOMPARE(result.read, qint64(3));
        }
    }
    void existingReadOnlyCacheDoesNotBypassFullHash() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        QCOMPARE(checked(track, cache).path, path);
        QCOMPARE(checked(track, cache).read, qint64(0));
        QVERIFY(QFile::setPermissions(cache, QFileDevice::ReadOwner));
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto result = checked(track, cache);
            QCOMPARE(result.path, path);
            QVERIFY(result.error.isEmpty());
            QCOMPARE(result.read, qint64(3));
        }
    }
    void busyCacheFallsBackToFullHashWithoutLosingPlayback() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        QCOMPARE(checked(track, cache).path, path);
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(cache);
            QVERIFY(db.open());
            QSqlQuery lock(db);
            QVERIFY(lock.exec("BEGIN EXCLUSIVE"));
            const auto result = checked(track, cache);
            QCOMPARE(result.path, path);
            QVERIFY(result.error.isEmpty());
            QCOMPARE(result.read, qint64(3));
            QVERIFY(db.rollback());
        }
        QSqlDatabase::removeDatabase(name);
        QCOMPARE(checked(track, cache).read, qint64(0));
    }
    void profilesDoNotSilentlyShareProcessOnlyReceipts() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), firstCache = temp.filePath("first.sqlite");
        const auto secondCache = temp.filePath("second.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        QCOMPARE(checked(track, firstCache).read, qint64(3));
        QCOMPARE(checked(track, secondCache).read, qint64(3));
        QCOMPARE(checked(track, firstCache).read, qint64(0));
        QCOMPARE(checked(track, secondCache).read, qint64(0));
    }
    void foreignDatabaseIsNotClaimedOrMigrated() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), cache = temp.filePath("catalog.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto name = QUuid::createUuid().toString();
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(cache);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("CREATE TABLE private_catalog(value TEXT)"));
            QVERIFY(query.exec("INSERT INTO private_catalog VALUES('keep me')"));
            QVERIFY(query.exec("PRAGMA user_version=6"));
        }
        QSqlDatabase::removeDatabase(name);
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto result = checked(mappedTrack(path), cache);
            QCOMPARE(result.path, path);
            QCOMPARE(result.read, qint64(3));
        }
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", name);
            db.setDatabaseName(cache);
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("SELECT value FROM private_catalog"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toString(), QString("keep me"));
            QVERIFY(query.exec("PRAGMA user_version"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 6);
            QVERIFY(query.exec("SELECT count(*) FROM sqlite_master WHERE type='table'"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 1);
        }
        QSqlDatabase::removeDatabase(name);
    }
    void cachedPathStillRejectsSymlinkAndMissingFile() {
        QTemporaryDir temp;
        const auto path = temp.filePath("copy.wav"), other = temp.filePath("other.wav");
        const auto cache = temp.filePath("verification.sqlite");
        QVERIFY(writeFile(path, "abc"));
        const auto track = mappedTrack(path);
        QCOMPARE(checked(track, cache).path, path);
        QVERIFY(QFile::rename(path, other));
        QVERIFY(QFile::link(other, path));
        QVERIFY(checked(track, cache).path.isEmpty());
        QVERIFY(QFile::remove(path));
        QVERIFY(checked(track, cache).path.isEmpty());
    }
    void unchangedUnmappedOriginalKeepsItsExactPath() {
        Track track;
        track.root = "/offline-originals";
        track.relativePath = "日本語/原曲.wav";
        const auto result = checked(track);
        QCOMPARE(result.path, QString("/offline-originals/日本語/原曲.wav"));
        QCOMPARE(result.read, qint64(0));
        QVERIFY(result.error.isEmpty());
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() == 8 && args[1] == "--verify-child") {
        auto track = mappedTrack(args[2], args[3].toLatin1());
        track.compatibleSize = args[4].toLongLong();
        track.compatibleModified = args[5].toLongLong();
        Result result;
        result.path = verify(track, &result.error, [&](qint64 bytes) {
            result.read = qMax(result.read, bytes);
            if (bytes && args[7] == "hold") {
                fwrite("hashing\n", 1, 8, stdout);
                fflush(stdout);
                QThread::msleep(60000);
            }
            if (bytes && args[7].toInt())
                QThread::msleep(args[7].toUInt());
        }, args[6]);
        const auto bytes = QJsonDocument(QJsonObject{{"path", result.path}, {"error", result.error},
                                                   {"read", double(result.read)}}).toJson(QJsonDocument::Compact);
        fwrite(bytes.constData(), 1, size_t(bytes.size()), stdout);
        return 0;
    }
    MediaVerificationTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "media_verification_tests.moc"
