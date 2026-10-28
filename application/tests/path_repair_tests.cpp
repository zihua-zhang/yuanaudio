#include "path_repair.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#ifndef Q_OS_WIN
#include <sys/stat.h>
#include <unistd.h>

class PosixNas : public PathRepairBackend {
public:
    QString mount;
    int downloads = 0, hardlinks = 0, stats = 0;
    QString linkError;
    bool storage = false;
    bool disconnected = false, loseOnLink = false;
    bool sessionUnavailable() const override { return disconnected; }
    bool storageFailure() const override { return storage; }
    QString local(const QString &remote) const { return QDir(mount).filePath(remote.mid(QString("/share/").size())); }
    QList<NasRepairFile> list(const QString &path, QString *error) override {
        error->clear();
        const auto folder = path == "/share" ? mount : local(path);
        if (!QFileInfo(folder).isDir()) { *error = "missing directory"; return {}; }
        QList<NasRepairFile> result;
        for (const auto &file : QDir(folder).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot))
            result.append({path + '/' + file.fileName(), file.fileName(), file.size(),
                           file.lastModified().toMSecsSinceEpoch(), file.isDir(), 0, 0});
        return result; // Directory listings deliberately omit inode/link metadata.
    }
    std::optional<NasRepairFile> stat(const QString &path, QString *error) override {
        ++stats;
        error->clear();
        const auto filePath = local(path);
        struct ::stat status{};
        if (::stat(QFile::encodeName(filePath).constData(), &status) != 0) { *error = "stat failed"; return {}; }
        const QFileInfo file(filePath);
        return NasRepairFile{path, file.fileName(), file.size(), file.lastModified().toMSecsSinceEpoch(),
                             file.isDir(), quint64(status.st_ino), quint32(status.st_nlink)};
    }
    bool createHardLink(const QString &source, const QString &destination, QString *error) override {
        ++hardlinks;
        error->clear();
        if (loseOnLink) { disconnected = true; *error = "session lost"; return false; }
        if (!linkError.isEmpty()) { *error = linkError; return false; }
        if (::link(QFile::encodeName(local(source)).constData(), QFile::encodeName(local(destination)).constData()) != 0) {
            *error = "link failed"; return false;
        }
        return true;
    }
    bool download(const QString &path, const QString &destination, QByteArray *sha, QString *error) override {
        ++downloads;
        if (storage) { *error = "local disk write failed"; return false; }
        QFile source(local(path)), target(destination);
        if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            *error = "download fixture failed"; return false;
        }
        const auto data = source.readAll();
        if (target.write(data) != data.size()) { *error = "local write failed"; return false; }
        target.close();
        *sha = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
        return true;
    }
};
#endif

class FixtureNas : public PathRepairBackend {
public:
    QHash<QString, QList<NasRepairFile>> directories;
    QHash<QString, QByteArray> audio;
    int downloads = 0;
    bool storage = false;
    bool storageFailure() const override { return storage; }
    QList<NasRepairFile> list(const QString &path, QString *error) override {
        if (!directories.contains(path)) *error = "missing directory";
        return directories.value(path);
    }
    bool download(const QString &path, const QString &destination, QByteArray *sha, QString *error) override {
        ++downloads;
        if (storage) { *error = "destination write failed"; return false; }
        if (!audio.contains(path)) { *error = "unavailable"; return false; }
        QFile out(destination);
        if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { *error = out.errorString(); return false; }
        const auto bytes = audio[path];
        if (out.write(bytes) != bytes.size()) return false;
        out.close();
        *sha = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        return true;
    }
};
class PathRepairTests : public QObject {
    Q_OBJECT
    struct Context {
        QTemporaryDir temp;
        PathRepairOptions options;
        Track track;
        FixtureNas nas;
        Context() {
            const QDir base(QFileInfo(temp.path()).canonicalFilePath());
            options.mountRoot = base.filePath("mount");
            options.serverRoot = "/volume2/share";
            options.outputRoot = base.filePath("mount/copies");
            options.checkpointPath = temp.filePath("repair.json");
            options.indexPath = temp.filePath("catalog.sqlite");
            const auto originalRoot = base.filePath("mount/original");
            QDir().mkpath(originalRoot);
            options.libraries = {{"lib", "library", originalRoot, ""}};
            Index index(options.indexPath);
            index.open(); index.saveLibrary(options.libraries.first());
            track.libraryId = "lib"; track.root = originalRoot;
            track.name = track.relativePath = QString::fromUtf8("ヒナギク.flac");
            track.folder = "."; track.size = 3; track.modified = 1000;
            index.upsert({track}); track = *index.trackByIdentity("lib", track.relativePath);
            const auto name = track.name.normalized(QString::NormalizationForm_D);
            const auto source = "/volume2/share/original/" + name;
            nas.directories["/volume2/share"] = {{"/volume2/share/original", "original", 0, 0, true}};
            nas.directories["/volume2/share/original"] = {{source, name, 3, 1000, false}};
            nas.audio[source] = "abc";
        }
    };
#ifndef Q_OS_WIN
    struct LinkContext {
        QTemporaryDir temp;
        PathRepairOptions options;
        Track track;
        PosixNas nas;
        QString original;
        LinkContext() {
            const QDir base(QFileInfo(temp.path()).canonicalFilePath());
            options.mountRoot = base.filePath("mount"); options.serverRoot = "/share";
            options.outputRoot = base.filePath("mount/aliases");
            options.localCacheRoot = base.filePath("local-cache");
            options.checkpointPath = base.filePath("checkpoint.json");
            options.indexPath = base.filePath("catalog.sqlite"); options.preferHardLinks = true;
            const auto originalRoot = base.filePath("mount/originals");
            QDir().mkpath(originalRoot);
            original = QDir(originalRoot).filePath(QString::fromUtf8("ヒナギク.flac").normalized(QString::NormalizationForm_D));
            QFile file(original);
            if (file.open(QIODevice::WriteOnly)) { file.write("abc"); file.close(); }
            options.libraries = {{"lib", "Music", originalRoot, ""}};
            Index index(options.indexPath); index.open(); index.saveLibrary(options.libraries.first());
            track.libraryId = "lib"; track.root = originalRoot;
            track.name = track.relativePath = QString::fromUtf8("ヒナギク.flac"); track.folder = ".";
            track.size = 3; track.modified = QFileInfo(original).lastModified().toMSecsSinceEpoch();
            index.upsert({track}); track = *index.trackByIdentity("lib", track.relativePath);
            nas.mount = options.mountRoot;
        }
        Track mapped() {
            Index index(options.indexPath); index.open();
            return index.trackByIdentity("lib", track.relativePath).value_or(Track{});
        }
    };
#endif
private slots:
#ifndef Q_OS_WIN
    void hardlinkUsesFreshInodesAndNeverDownloads() {
        LinkContext c; std::atomic_bool pause{false};
        QString statError;
        const auto before = c.nas.stat("/share/originals/" + QFileInfo(c.original).fileName(), &statError);
        QVERIFY(before);
        auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY2(result.lastError.isEmpty(), qPrintable(result.lastError));
        QCOMPARE(result.succeeded, 1); QCOMPARE(result.hardlinked, 1); QCOMPARE(result.cached, 0);
        QCOMPARE(c.nas.downloads, 0); QCOMPARE(c.nas.hardlinks, 1);
        const auto mapped = c.mapped(); QVERIFY(mapped.compatiblePath.startsWith(c.options.outputRoot + '/'));
        struct ::stat original{}, alias{};
        QCOMPARE(::stat(QFile::encodeName(c.original).constData(), &original), 0);
        QCOMPARE(::stat(QFile::encodeName(mapped.compatiblePath).constData(), &alias), 0);
        QCOMPARE(quint64(original.st_ino), quint64(alias.st_ino));
        QVERIFY(alias.st_nlink >= 2); QCOMPARE(quint64(original.st_ino), before->fileId);
        QCOMPARE(QFileInfo(mapped.compatiblePath).fileName().normalized(QString::NormalizationForm_C), c.track.name);
        result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QCOMPARE(result.succeeded, 1); QCOMPARE(result.reused, 1); QCOMPARE(c.nas.hardlinks, 1);
        QCOMPARE(c.nas.downloads, 0);
    }
    void unsupportedAndCrossFilesystemLinksFallBackLocally_data() {
        QTest::addColumn<QString>("reason");
        QTest::newRow("unsupported") << "not supported";
        QTest::newRow("cross-filesystem") << "EXDEV cross filesystem";
    }
    void unsupportedAndCrossFilesystemLinksFallBackLocally() {
        QFETCH(QString, reason);
        LinkContext c; c.nas.linkError = reason; std::atomic_bool pause{false};
        const auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY2(result.lastError.isEmpty(), qPrintable(result.lastError));
        QCOMPARE(result.succeeded, 1); QCOMPARE(result.cached, 1); QCOMPARE(result.hardlinked, 0);
        QCOMPARE(c.nas.downloads, 1);
        QVERIFY(c.mapped().compatiblePath.startsWith(c.options.localCacheRoot + '/'));
        QFile original(c.original); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("abc"));
    }
    void unwritableNasAliasRootFallsBackLocally() {
        LinkContext c; std::atomic_bool pause{false};
        QFile blocker(c.options.outputRoot); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.write("keep"); blocker.close();
        const auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.succeeded, 1); QCOMPARE(result.cached, 1); QCOMPARE(c.nas.downloads, 1);
        QVERIFY(c.mapped().compatiblePath.startsWith(c.options.localCacheRoot + '/'));
        QVERIFY(blocker.open(QIODevice::ReadOnly)); QCOMPARE(blocker.readAll(), QByteArray("keep"));
    }
    void brokenManagedAliasIsKeptWhileNewLinkRepairsMapping() {
        LinkContext c; std::atomic_bool pause{false};
        QCOMPARE(runPathRepair(c.options, {c.track}, c.nas, &pause, {}).succeeded, 1);
        const auto old = c.mapped().compatiblePath;
        // Break only our test alias, without writing through the hard link to the original.
        QVERIFY(QFile::remove(old));
        QFile replacement(old); QVERIFY(replacement.open(QIODevice::WriteOnly | QIODevice::NewOnly));
        replacement.write("axc"); replacement.close();
        const auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QCOMPARE(result.succeeded, 1); QCOMPARE(result.hardlinked, 1); QCOMPARE(result.reused, 0);
        QVERIFY(c.mapped().compatiblePath != old); QCOMPARE(c.nas.downloads, 0);
        QVERIFY(replacement.open(QIODevice::ReadOnly)); QCOMPARE(replacement.readAll(), QByteArray("axc"));
        QFile original(c.original); QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("abc"));
    }
    void corruptedLocalCachePausesThenExplicitResumeKeepsEvidence() {
        LinkContext c; c.nas.linkError = "unsupported"; std::atomic_bool pause{false};
        QCOMPARE(runPathRepair(c.options, {c.track}, c.nas, &pause, {}).cached, 1);
        const auto old = c.mapped().compatiblePath;
        QFile replacement(old); QVERIFY(replacement.open(QIODevice::WriteOnly));
        replacement.write("axc"); replacement.close();
        auto result = runPathRepair(c.options, {c.track, c.track}, c.nas, &pause, {});
        QVERIFY(result.paused); QCOMPARE(c.nas.downloads, 1);
        QCOMPARE(c.mapped().compatiblePath, old);
        QFile evidence(c.options.checkpointPath); QVERIFY(evidence.open(QIODevice::ReadOnly));
        QVERIFY(evidence.readAll().contains(old.toUtf8())); evidence.close();
        result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QCOMPARE(result.succeeded, 1); QCOMPARE(result.cached, 1); QCOMPARE(result.reused, 0);
        QVERIFY(c.mapped().compatiblePath != old); QCOMPARE(c.nas.downloads, 2);
        QVERIFY(evidence.open(QIODevice::ReadOnly)); QVERIFY(evidence.readAll().contains(old.toUtf8()));
        QVERIFY(replacement.open(QIODevice::ReadOnly)); QCOMPARE(replacement.readAll(), QByteArray("axc"));
    }
    void lostSessionPausesWithoutDownloadingRemainingBatch() {
        LinkContext c; c.nas.loseOnLink = true; std::atomic_bool pause{false};
        const auto result = runPathRepair(c.options, {c.track, c.track}, c.nas, &pause, {});
        QVERIFY(result.paused); QCOMPARE(result.done, 1); QCOMPARE(c.nas.downloads, 0);
        QVERIFY(QFile::exists(c.options.checkpointPath));
    }
    void replacementInodeWithSameSizeAndTimeKeepsOriginalReceipt() {
        LinkContext c; std::atomic_bool pause{false};
        QCOMPARE(runPathRepair(c.options, {c.track}, c.nas, &pause, {}).hardlinked, 1);
        const auto old = c.mapped().compatiblePath;
        QVERIFY(QFile::remove(c.original));
        QFile replacement(c.original); QVERIFY(replacement.open(QIODevice::WriteOnly));
        QCOMPARE(replacement.write("xyz"), 3); replacement.close();
        QVERIFY(replacement.open(QIODevice::ReadOnly));
        QVERIFY(replacement.setFileTime(QDateTime::fromMSecsSinceEpoch(c.track.modified), QFileDevice::FileModificationTime));
        replacement.close();
        const auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QCOMPARE(result.succeeded, 0); QCOMPARE(result.failed, 1);
        QCOMPARE(c.nas.hardlinks, 1); QCOMPARE(c.nas.downloads, 0);
        QCOMPARE(c.mapped().compatiblePath, old);
        QFile checkpoint(c.options.checkpointPath); QVERIFY(checkpoint.open(QIODevice::ReadOnly));
        QVERIFY(checkpoint.readAll().contains(old.toUtf8()));
    }
    void rejectsLocalCacheInsideNasOrThroughLinksOrUnownedDirectory() {
        LinkContext c; std::atomic_bool pause{false};
        auto options = c.options; options.localCacheRoot = options.mountRoot + "/local-cache";
        QVERIFY(!runPathRepair(options, {c.track}, c.nas, &pause, {}).error.isEmpty());
        const auto target = c.temp.filePath("linked-target"); QVERIFY(QDir().mkpath(target));
        QVERIFY(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(c.options.localCacheRoot).constData()) == 0);
        QVERIFY(!runPathRepair(c.options, {c.track}, c.nas, &pause, {}).error.isEmpty());
        QVERIFY(QFile::remove(c.options.localCacheRoot));
        QVERIFY(QDir().mkpath(c.options.localCacheRoot));
        QFile keep(QDir(c.options.localCacheRoot).filePath("keep.txt"));
        QVERIFY(keep.open(QIODevice::WriteOnly)); keep.write("keep"); keep.close();
        const auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QVERIFY(!result.error.isEmpty()); QCOMPARE(c.nas.hardlinks, 0); QCOMPARE(c.nas.downloads, 0);
        QVERIFY(keep.open(QIODevice::ReadOnly)); QCOMPARE(keep.readAll(), QByteArray("keep"));
    }
    void localWriteFailureCheckpointsAndResumeReusesWithoutCredentials() {
        LinkContext c; c.nas.linkError = "unsupported"; c.nas.storage = true; std::atomic_bool pause{false};
        auto result = runPathRepair(c.options, {c.track, c.track}, c.nas, &pause, {});
        QVERIFY(result.paused); QCOMPARE(result.done, 1); QVERIFY(QFile::exists(c.options.checkpointPath));
        c.nas.storage = false;
        result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QCOMPARE(result.succeeded, 1); QCOMPARE(result.cached, 1);
        const auto downloads = c.nas.downloads;
        result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QCOMPARE(result.succeeded, 1); QCOMPARE(result.reused, 1); QCOMPARE(c.nas.downloads, downloads);
        QFile checkpoint(c.options.checkpointPath); QVERIFY(checkpoint.open(QIODevice::ReadOnly));
        const auto raw = checkpoint.readAll(); QVERIFY(raw.contains("local_cache"));
        QVERIFY(!raw.contains("password")); QVERIFY(!raw.contains("token"));
        auto changed = c.options; changed.localCacheRoot += "-different";
        QVERIFY(!runPathRepair(changed, {c.track}, c.nas, &pause, {}).error.isEmpty());
        changed = c.options; changed.serverIdentity = "different-server/share";
        QVERIFY(!runPathRepair(changed, {c.track}, c.nas, &pause, {}).error.isEmpty());
    }
#endif
    void repairsOnlyCopyAndResumeDoesNotDownloadAgain() {
        Context c;
        std::atomic_bool pause{false};
        auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY2(result.lastError.isEmpty(), qPrintable(result.lastError));
        QCOMPARE(result.succeeded, 1); QCOMPARE(result.failed, 0);
        Index index(c.options.indexPath); QVERIFY(index.open());
        auto repaired = *index.trackByIdentity("lib", c.track.relativePath);
        QVERIFY(!repaired.compatiblePath.isEmpty());
        QVERIFY(repaired.compatiblePath.startsWith(c.options.outputRoot + '/'));
        QVERIFY(!QFile::exists(c.track.absolutePath()));
        QFile copy(repaired.sourcePath()); QVERIFY(copy.open(QIODevice::ReadOnly)); QCOMPARE(copy.readAll(), QByteArray("abc"));
        auto again = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QCOMPARE(again.succeeded, 1); QCOMPARE(c.nas.downloads, 1);
        QFile checkpoint(c.options.checkpointPath); QVERIFY(checkpoint.open(QIODevice::ReadOnly));
        const auto raw = checkpoint.readAll(); QVERIFY(!raw.contains("password")); QVERIFY(!raw.contains("token"));
    }
    void rejectsAmbiguousNamesAndContinuesNextItem() {
        Context c;
        const auto folder = QString("/volume2/share/original");
        c.nas.directories[folder].append({folder + '/' + c.track.name, c.track.name, 3, 1000, false});
        auto good = c.track; good.id = 0; good.name = good.relativePath = "valid.flac";
        Index index(c.options.indexPath); QVERIFY(index.open()); QVERIFY(index.upsert({good}));
        good = *index.trackByIdentity("lib", good.relativePath);
        c.nas.directories[folder].append({folder + "/valid.flac", "valid.flac", 3, 1000, false});
        c.nas.audio[folder + "/valid.flac"] = "abc";
        std::atomic_bool pause{false};
        auto result = runPathRepair(c.options, {c.track, good}, c.nas, &pause, {});
        QCOMPARE(result.failed, 1); QCOMPARE(result.succeeded, 1); QCOMPARE(c.nas.downloads, 1);
        QVERIFY(result.lastError.contains("多") || result.lastError.contains("冲突"));
    }
    void resumeReplacesBrokenMappingWithoutOverwritingOldCopy() {
        Context c; std::atomic_bool pause{false};
        QCOMPARE(runPathRepair(c.options, {c.track}, c.nas, &pause, {}).succeeded, 1);
        Index index(c.options.indexPath); QVERIFY(index.open());
        const auto track = *index.trackByIdentity("lib", c.track.relativePath);
        QFile copy(track.compatiblePath); QVERIFY(copy.open(QIODevice::WriteOnly)); copy.write("axc"); copy.close();
        const auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QCOMPARE(result.failed, 0); QCOMPARE(result.succeeded, 1); QCOMPARE(c.nas.downloads, 2);
        QVERIFY(copy.open(QIODevice::ReadOnly)); QCOMPARE(copy.readAll(), QByteArray("axc"));
        const auto repaired = *index.trackByIdentity("lib", c.track.relativePath);
        QVERIFY(repaired.compatiblePath != track.compatiblePath);
    }
    void outputWriteFailurePausesAfterCheckpoint() {
        Context c; std::atomic_bool pause{false}; c.nas.storage = true;
        auto result = runPathRepair(c.options, {c.track, c.track}, c.nas, &pause, {});
        QVERIFY(result.paused); QCOMPARE(result.done, 1); QCOMPARE(c.nas.downloads, 1);
        QVERIFY(QFile::exists(c.options.checkpointPath));
    }
    void rejectsOutputInsideOriginalAndTraversal() {
        Context c;
        std::atomic_bool pause{false};
        c.options.outputRoot = c.track.root + "/copies";
        auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QVERIFY(!result.error.isEmpty()); QCOMPARE(c.nas.downloads, 0);
        c.options.outputRoot = c.options.mountRoot + "/copies";
        c.track.relativePath = "../escape.flac";
        result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QCOMPARE(result.failed, 1); QCOMPARE(c.nas.downloads, 0);
    }
    void protectsOriginalEvenWithDifferentCaseSpelling() {
        Context c; std::atomic_bool pause{false};
        c.options.outputRoot = c.options.mountRoot + "/ORIGINAL/copies";
        const auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QVERIFY(!result.error.isEmpty()); QCOMPARE(c.nas.downloads, 0);
        QVERIFY(!QFile::exists(c.track.root + "/copies/.yuanaudio-compatible.json"));
    }
    void pauseCheckpointAndChangedOriginalAreHonest() {
        Context c;
        std::atomic_bool pause{true};
        auto result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QVERIFY(result.paused); QCOMPARE(c.nas.downloads, 0);
        pause = false;
        c.nas.directories["/volume2/share/original"].first().modified = 9000;
        result = runPathRepair(c.options, {c.track}, c.nas, &pause, {});
        QCOMPARE(result.failed, 1); QCOMPARE(c.nas.downloads, 0);
    }
};
QTEST_GUILESS_MAIN(PathRepairTests)
#include "path_repair_tests.moc"
