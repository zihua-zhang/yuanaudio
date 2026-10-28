#include "path_repair.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>

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
private slots:
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
