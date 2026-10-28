#include "cache.h"
#include "index.h"
#include <QDir>
#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

static Track track(const QString &path, qint64 size = 10) {
    Track t;
    t.libraryId = "library-a";
    t.relativePath = path;
    t.name = QFileInfo(path).fileName();
    t.folder = QFileInfo(path).path();
    t.size = size;
    t.modified = 1000;
    return t;
}
class CoreTests : public QObject {
    Q_OBJECT
  private slots:
    void exclusionRestorePreservesPersonalMetadata_data() {
        QTest::addColumn<bool>("folder");
        QTest::addColumn<bool>("changedVersion");
        QTest::newRow("single-unchanged") << false << false;
        QTest::newRow("folder-unchanged") << true << false;
        QTest::newRow("single-replaced") << false << true;
        QTest::newRow("folder-replaced") << true << true;
    }
    void exclusionRestorePreservesPersonalMetadata() {
        QFETCH(bool, folder);
        QFETCH(bool, changedVersion);
        QTemporaryDir dir;
        const auto path = dir.filePath("index.db");
        const auto song = track("剧情/回忆.wav");
        qint64 originalId = 0;
        {
            Index db(path);
            QVERIFY(db.open());
            QVERIFY(db.saveLibrary({"library-a", "A", dir.path(), ""}));
            QVERIFY(db.upsert({song}));
            const auto original = db.trackByIdentity("library-a", song.relativePath);
            QVERIFY(original);
            originalId = original->id;
            QVERIFY(db.setPersonal(originalId, true, "女主回忆 收藏标记"));
            QVERIFY(db.setDuration(*original, 96000));
            QVERIFY(folder ? db.excludeFolder("library-a", "剧情")
                           : db.excludeTrack("library-a", song.relativePath));
            QVERIFY(db.search("女主回忆").tracks.isEmpty());
            // A stale scan cannot make the excluded entry visible or replace its metadata.
            QVERIFY(db.upsert({song}));
            QCOMPARE(db.count(), qint64(0));
        }
        Index reopened(path);
        QVERIFY(reopened.open());
        QVERIFY(reopened.restoreExcludedItem("library-a", folder ? "剧情" : song.relativePath, folder));
        auto rescanned = song;
        if (changedVersion)
            ++rescanned.modified;
        QVERIFY(reopened.upsert({rescanned}));
        const auto restored = reopened.trackByIdentity("library-a", song.relativePath);
        QVERIFY(restored);
        QVERIFY(restored->favorite);
        QCOMPARE(restored->tags, QString("女主回忆 收藏标记"));
        QCOMPARE(restored->duration, changedVersion ? qint64(0) : qint64(96000));
        QCOMPARE(restored->id, originalId);
        QCOMPARE(reopened.search("女主回忆", {}, {}, true).tracks.size(), 1);
        // The archived values must not overwrite a later explicit change after another scan.
        QVERIFY(reopened.setPersonal(restored->id, false, "第二集"));
        QVERIFY(reopened.upsert({rescanned}));
        QCOMPARE(reopened.search("第二集").tracks.size(), 1);
        QVERIFY(reopened.search("女主回忆").tracks.isEmpty());
    }
    void restoredMetadataCannotOverwriteAReusedTrackId() {
        QTemporaryDir dir;
        Index db(dir.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "A", dir.path(), ""}));
        const auto song = track("hidden.wav");
        QVERIFY(db.upsert({song}));
        const auto original = db.trackByIdentity("library-a", song.relativePath);
        QVERIFY(original);
        QVERIFY(db.setPersonal(original->id, true, "隐藏收藏"));
        QVERIFY(db.excludeTrack("library-a", song.relativePath));
        QVERIFY(db.upsert({track("new.wav")}));
        const auto newer = db.trackByIdentity("library-a", "new.wav");
        QVERIFY(newer);
        QVERIFY(db.setPersonal(newer->id, false, "新音乐"));
        QVERIFY(db.restoreExcludedItem("library-a", song.relativePath, false));
        QVERIFY(db.upsert({song}));
        const auto restored = db.trackByIdentity("library-a", song.relativePath);
        QVERIFY(restored);
        QVERIFY(restored->favorite);
        QCOMPARE(restored->tags, QString("隐藏收藏"));
        QVERIFY(restored->id != newer->id);
        const auto untouched = db.trackByIdentity("library-a", "new.wav");
        QVERIFY(untouched);
        QCOMPARE(untouched->id, newer->id);
        QVERIFY(!untouched->favorite);
        QCOMPARE(untouched->tags, QString("新音乐"));
        QCOMPARE(db.search("新音乐").tracks.size(), 1);
    }
    void guardedPersonalWritesRejectStaleAndReusedIdentities() {
        QTemporaryDir dir;
        Index db(dir.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "A", dir.path(), ""}));
        QVERIFY(db.upsert({track("old.wav")}));
        const auto old = db.trackByIdentity("library-a", "old.wav");
        QVERIFY(old);
        QVERIFY(db.setPersonal(*old, true, "原标记"));
        auto changed = *old;
        ++changed.size;
        QVERIFY(db.upsert({changed}));
        QVERIFY(!db.setPersonal(*old, false, "过期写入"));
        QVERIFY(!db.lastError().isEmpty());
        QCOMPARE(db.search("原标记", {}, {}, true).tracks.size(), 1);
        auto wrong = changed;
        wrong.relativePath = "wrong.wav";
        QVERIFY(!db.setPersonal(wrong, false, "错误路径"));
        wrong = changed;
        wrong.libraryId = "different-library";
        QVERIFY(!db.setPersonal(wrong, false, "错误库"));
        QVERIFY(db.excludeTrack("library-a", "old.wav"));
        QVERIFY(db.upsert({track("new.wav")}));
        const auto newer = db.trackByIdentity("library-a", "new.wav");
        QVERIFY(newer);
        QVERIFY(db.setPersonal(*newer, false, "新歌曲标记"));
        QVERIFY(!db.setPersonal(changed, true, "串到新歌曲"));
        const auto untouched = db.trackByIdentity("library-a", "new.wav");
        QVERIFY(untouched);
        QVERIFY(!untouched->favorite);
        QCOMPARE(untouched->tags, QString("新歌曲标记"));
        QVERIFY(db.search("串到新歌曲").tracks.isEmpty());
        QCOMPARE(db.search("新歌曲标记").tracks.size(), 1);
    }
    void failedRestoreWriteKeepsArchivedMetadataForRetry() {
        QTemporaryDir dir;
        const auto path = dir.filePath("index.db");
        Index db(path);
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "A", dir.path(), ""}));
        const auto song = track("retry.wav");
        QVERIFY(db.upsert({song}));
        QVERIFY(db.setPersonal(db.trackByIdentity("library-a", song.relativePath)->id, true, "重试后保留"));
        QVERIFY(db.excludeTrack("library-a", song.relativePath));
        QVERIFY(db.restoreExcludedItem("library-a", song.relativePath, false));
        {
            auto failure = QSqlDatabase::addDatabase("QSQLITE", "restore-write-failure");
            failure.setDatabaseName(path);
            QVERIFY(failure.open());
            QSqlQuery q(failure);
            QVERIFY(q.exec("CREATE TRIGGER reject_restore BEFORE INSERT ON tracks "
                           "BEGIN SELECT RAISE(ABORT,'fixture write failure'); END"));
        }
        QSqlDatabase::removeDatabase("restore-write-failure");
        QVERIFY(!db.upsert({song}));
        QCOMPARE(db.count(), qint64(0));
        {
            auto retry = QSqlDatabase::addDatabase("QSQLITE", "restore-write-retry");
            retry.setDatabaseName(path);
            QVERIFY(retry.open());
            QSqlQuery q(retry);
            QVERIFY(q.exec("DROP TRIGGER reject_restore"));
        }
        QSqlDatabase::removeDatabase("restore-write-retry");
        QVERIFY(db.upsert({song}));
        QCOMPARE(db.search("重试后保留", {}, {}, true).tracks.size(), 1);
    }
    void schemaFiveMigrationPreservesPersonalMetadataAndExclusionRules() {
        QTemporaryDir dir;
        const auto path = dir.filePath("index.db");
        {
            Index db(path);
            QVERIFY(db.open());
            QVERIFY(db.saveLibrary({"library-a", "A", dir.path(), ""}));
            QVERIFY(db.upsert({track("music.wav")}));
            QVERIFY(db.setPersonal(db.trackByIdentity("library-a", "music.wav")->id, true, "升级前收藏"));
            QVERIFY(db.excludeFolder("library-a", "blocked"));
        }
        {
            auto legacy = QSqlDatabase::addDatabase("QSQLITE", "metadata-schema-five");
            legacy.setDatabaseName(path);
            QVERIFY(legacy.open());
            QSqlQuery q(legacy);
            QVERIFY(q.exec("DROP TABLE IF EXISTS excluded_track_metadata"));
            QVERIFY(q.exec("PRAGMA user_version=5"));
        }
        QSqlDatabase::removeDatabase("metadata-schema-five");
        Index migrated(path);
        QVERIFY(migrated.open());
        QCOMPARE(migrated.search("升级前收藏", {}, {}, true).tracks.size(), 1);
        QVERIFY(migrated.exclusions("library-a")->excludesFolder("blocked"));
        QVERIFY(migrated.excludeTrack("library-a", "music.wav"));
        QVERIFY(migrated.restoreExcludedItem("library-a", "music.wav", false));
        QVERIFY(migrated.upsert({track("music.wav")}));
        QCOMPARE(migrated.search("升级前收藏", {}, {}, true).tracks.size(), 1);
    }
    void catalogExclusionsPersistAndRespectLibraryAndFolderBoundaries() {
        QTemporaryDir dir;
        const auto database = dir.filePath("index.db");
        QList<Track> input{track("🎵100%_/曲.wav"), track("🎵100%_/子/曲.wav"),
                          track("🎵100%_more/曲.wav"), track("🎵100AA/曲.wav"),
                          track("保留/单曲.wav"), track("保留/单曲.wav.bak"), track("根曲.wav")};
        auto other = input[0];
        other.libraryId = "library-b";
        input << other;
        {
            Index db(database), staleScanner(database);
            QVERIFY(db.open());
            QVERIFY(staleScanner.open());
            QVERIFY(db.saveLibrary({"library-a", "A", dir.filePath("a"), ""}));
            QVERIFY(db.saveLibrary({"library-b", "B", dir.filePath("b"), ""}));
            QVERIFY(db.upsert(input));
            QVERIFY(db.upsertFolders("library-a", {"🎵100%_/空目录"}));
            const auto before = db.search("").generation;
            QVERIFY(db.excludeFolder("library-a", "🎵100%_"));
            QVERIFY(db.excludeTrack("library-a", "保留/单曲.wav"));
            QCOMPARE(db.countForLibrary("library-a"), qint64(4));
            QCOMPARE(db.countForLibrary("library-b"), qint64(1));
            QVERIFY(db.search("").generation > before);
            QVERIFY(!db.trackByIdentity("library-a", "🎵100%_/曲.wav"));
            QVERIFY(db.trackByIdentity("library-b", "🎵100%_/曲.wav"));
            QVERIFY(db.trackByIdentity("library-a", "🎵100%_more/曲.wav"));
            QVERIFY(db.trackByIdentity("library-a", "保留/单曲.wav.bak"));
            QVERIFY(!db.folders("library-a").contains("🎵100%_/空目录"));
            // A worker opened before removal must recheck rules inside its write transaction.
            bool changed = true;
            QVERIFY(staleScanner.upsert(input, nullptr, &changed));
            QVERIFY(!changed);
            QVERIFY(staleScanner.upsertFolders("library-a", {"🎵100%_/空目录/更深"}));
            QCOMPARE(db.count(), qint64(5));
            QVERIFY(!db.folders("library-a").contains("🎵100%_"));
            QVERIFY(db.findFolders("🎵100%_").folders.size() >= 1); // The other library remains.
        }
        Index reopened(database);
        QVERIFY(reopened.open());
        QVERIFY(reopened.upsert(input));
        QCOMPARE(reopened.count(), qint64(5));
        const auto rules = reopened.exclusions("library-a");
        QVERIFY(rules);
        QVERIFY(rules->excludesFolder("🎵100%_/new/deep"));
        QVERIFY(!rules->excludesFolder("🎵100%_more"));
        QVERIFY(rules->excludesTrack("保留/单曲.wav"));
        QVERIFY(!rules->excludesTrack("保留/单曲.wav.bak"));
    }
    void removeLibraryLeavesAudioAndOtherLibrariesUntouchedAndBlocksStaleScan() {
        QTemporaryDir dir;
        QFile original(dir.filePath("song.wav"));
        QVERIFY(original.open(QIODevice::WriteOnly));
        const QByteArray bytes("original audio remains byte-for-byte");
        QCOMPARE(original.write(bytes), bytes.size());
        original.close();
        const auto path = dir.filePath("index.db");
        Index db(path), staleScanner(path);
        QVERIFY(db.open());
        QVERIFY(staleScanner.open());
        const Library removed{"library-a", "A", dir.path(), ""};
        QVERIFY(db.saveLibrary(removed));
        QVERIFY(db.saveLibrary({"library-b", "B", dir.path(), ""}));
        auto first = track("song.wav"), second = first;
        second.libraryId = "library-b";
        QVERIFY(db.upsert({first, second}));
        const auto other = db.trackByIdentity("library-b", second.relativePath);
        QVERIFY(other);
        QVERIFY(db.setPersonal(other->id, true, "我的保留标签"));
        QVERIFY(db.excludeFolder("library-a", "旧目录"));
        const auto generation = db.search("").generation;
        QVERIFY(db.removeLibrary("library-a"));
        QCOMPARE(db.libraries().size(), 1);
        QCOMPARE(db.count(), qint64(1));
        QVERIFY(db.folders("library-a").isEmpty());
        QVERIFY(db.findFolders("A").folders.isEmpty());
        QCOMPARE(db.search("我的保留标签", "library-b", {}, true).tracks.size(), 1);
        QVERIFY(db.search("").generation > generation);
        QVERIFY(!staleScanner.upsert({first}));
        QVERIFY(!staleScanner.saveLibrary(removed, false));
        Index restarted(path);
        QVERIFY(restarted.open());
        QVERIFY(!restarted.saveLibrary(removed, false));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), bytes);
        original.close();
        // Re-adding is an explicit user action and starts with a fresh local catalog.
        QVERIFY(restarted.saveLibrary(removed));
        QVERIFY(restarted.upsert({first, track("旧目录/new.wav")}));
        QCOMPARE(restarted.countForLibrary("library-a"), qint64(2));
        QCOMPARE(restarted.countForLibrary("library-b"), qint64(1));
    }
    void restoringExclusionsOnlyChangesFutureScanEligibility() {
        QTemporaryDir dir;
        const auto path = dir.filePath("index.db");
        Index db(path);
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "A", dir.path(), ""}));
        auto keptExcluded = track("folder/still-excluded.wav"), restored = track("folder/restored.wav");
        QVERIFY(db.upsert({keptExcluded, restored}));
        QVERIFY(db.excludeTrack("library-a", keptExcluded.relativePath));
        QVERIFY(db.excludeFolder("library-a", "folder"));
        auto rules = db.excludedItems("library-a");
        QCOMPARE(rules.size(), 2);
        QVERIFY(rules[0].isFolder);
        QCOMPARE(rules[0].path, QString("folder"));
        QVERIFY(!rules[1].isFolder);
        QCOMPARE(db.count(), qint64(0));
        QVERIFY(db.restoreExcludedItem("library-a", "folder", true));
        QCOMPARE(db.count(), qint64(0)); // Restoring never pretends a scan has already run.
        QCOMPARE(db.excludedItems("library-a").size(), 1);
        Index restarted(path);
        QVERIFY(restarted.open());
        QVERIFY(restarted.upsert({keptExcluded, restored}));
        QCOMPARE(restarted.count(), qint64(1));
        QVERIFY(restarted.trackByIdentity("library-a", restored.relativePath));
        QVERIFY(!restarted.trackByIdentity("library-a", keptExcluded.relativePath));
        QVERIFY(restarted.restoreExcludedItem("library-a", keptExcluded.relativePath, false));
        QCOMPARE(restarted.count(), qint64(1));
        QVERIFY(restarted.upsert({keptExcluded}));
        QCOMPARE(restarted.count(), qint64(2));
        QVERIFY(!restarted.restoreExcludedItem("library-a", "../escape", true));
        QVERIFY(!restarted.restoreExcludedItem("missing", "folder", true));
    }
    void catalogExclusionRejectsUnsafePathsAndRollsBackWholeMutation() {
        QTemporaryDir dir;
        const auto path = dir.filePath("index.db");
        Index db(path);
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "A", dir.path(), ""}));
        QVERIFY(db.upsert({track("safe/song.wav")}));
        const auto generation = db.search("").generation;
        for (const auto &invalid : QStringList{"", ".", "..", "../safe", "safe/../safe", "/safe",
                                               "C:/safe", "safe\\song.wav", "safe/", "safe//song.wav"}) {
            QString error;
            QVERIFY(!db.excludeFolder("library-a", invalid, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(!db.excludeTrack("library-a", invalid, &error));
        }
        QVERIFY(!db.excludeFolder("missing", "safe"));
        QVERIFY(!db.removeLibrary("missing"));
        QCOMPARE(db.count(), qint64(1));
        QCOMPARE(db.search("").generation, generation);
        {
            auto broken = QSqlDatabase::addDatabase("QSQLITE", "catalog-rollback-fixture");
            broken.setDatabaseName(path);
            QVERIFY(broken.open());
            QSqlQuery q(broken);
            QVERIFY(q.exec("CREATE TRIGGER reject_catalog_delete BEFORE DELETE ON tracks "
                           "BEGIN SELECT RAISE(ABORT,'fixture write failure'); END"));
        }
        QSqlDatabase::removeDatabase("catalog-rollback-fixture");
        QVERIFY(!db.excludeFolder("library-a", "safe"));
        QCOMPARE(db.count(), qint64(1));
        QCOMPARE(db.search("song").tracks.size(), 1); // FTS deletion must roll back too.
        QVERIFY(db.exclusions("library-a")->folders.isEmpty());
        QCOMPARE(db.search("").generation, generation);
    }
    void schemaFourMigrationPreservesExistingPersonalMetadata() {
        QTemporaryDir dir;
        const auto path = dir.filePath("index.db");
        {
            Index db(path);
            QVERIFY(db.open());
            QVERIFY(db.saveLibrary({"library-a", "A", dir.path(), ""}));
            QVERIFY(db.upsert({track("song.wav")}));
            QVERIFY(db.setPersonal(db.search("").tracks[0].id, true, "保留标签"));
        }
        {
            auto legacy = QSqlDatabase::addDatabase("QSQLITE", "catalog-schema-four");
            legacy.setDatabaseName(path);
            QVERIFY(legacy.open());
            QSqlQuery q(legacy);
            QVERIFY(q.exec("DROP TABLE catalog_exclusions"));
            QVERIFY(q.exec("DROP TABLE removed_libraries"));
            QVERIFY(q.exec("PRAGMA user_version=4"));
        }
        QSqlDatabase::removeDatabase("catalog-schema-four");
        Index migrated(path);
        QVERIFY(migrated.open());
        QCOMPARE(migrated.search("保留标签", {}, {}, true).tracks.size(), 1);
        QVERIFY(migrated.excludeTrack("library-a", "song.wav"));
        QVERIFY(migrated.upsert({track("song.wav")}));
        QCOMPARE(migrated.count(), qint64(0));
    }
    void delayedDurationWritesCannotOverwriteAnotherFileVersion() {
        QTemporaryDir dir;
        Index db(dir.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "配乐", dir.path(), ""}));
        QVERIFY(db.upsert({track("雨夜.wav")}));
        const auto old = db.search("").tracks.first();
        db.setDuration(old, 12000);
        QCOMPARE(db.search("").tracks.first().duration, qint64(12000));
        auto replacement = old;
        ++replacement.size;
        QVERIFY(db.upsert({replacement}));
        // This reproduces a wave result queued before a scan replaced the indexed file.
        db.setDuration(old, 12000);
        QCOMPARE(db.search("").tracks.first().duration, qint64(0));
        db.setDuration(replacement, 24000);
        QCOMPARE(db.search("").tracks.first().duration, qint64(24000));
        auto newer = replacement;
        ++newer.modified;
        QVERIFY(db.upsert({newer}));
        db.setDuration(replacement, 24000);
        QCOMPARE(db.search("").tracks.first().duration, qint64(0));
        auto wrongIdentity = newer;
        wrongIdentity.libraryId = "other-library";
        db.setDuration(wrongIdentity, 48000);
        wrongIdentity = newer;
        wrongIdentity.relativePath = "别的音乐.wav";
        db.setDuration(wrongIdentity, 48000);
        QCOMPARE(db.search("").tracks.first().duration, qint64(0));
        db.setDuration(newer, 36000);
        QCOMPARE(db.search("").tracks.first().duration, qint64(36000));
    }
    void foldersFindDuplicateNamesLiteralPathsAndRootScope() {
        QTemporaryDir dir;
        Index db(dir.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "配乐", "Z:/offline", ""}));
        QVERIFY(db.upsert({track("雨.wav"), track("古风/伤感/雪.wav"), track("现代/伤感/风.wav"),
                           track("现代/伤感/回忆/叶.wav"), track("100%/雨.wav")}));
        QVERIFY(db.upsertFolders("library-a", {"空目录/深层", "🎵音乐/PIANO"}));
        auto matches = db.findFolders("伤感");
        QCOMPARE(matches.folders.size(), 3);
        QCOMPARE(matches.folders[0].name, QString("伤感"));
        QCOMPARE(matches.folders[1].name, QString("伤感"));
        QCOMPARE(db.findFolders("现代 伤感").folders.size(), 2);
        QCOMPARE(db.findFolders("100%").folders.size(), 1);
        QCOMPARE(db.findFolders("piano").folders.size(), 1);
        QCOMPARE(db.findFolders("空").folders.size(), 2);
        QCOMPARE(db.findFolders("\" OR * --").folders.size(), 0);
        auto chain = db.folderAncestors("library-a", "现代/伤感/回忆");
        QCOMPARE(chain.folders.size(), 4);
        QCOMPARE(chain.folders[0].path, QString(""));
        QCOMPARE(chain.folders[1].path, QString("现代"));
        QVERIFY(!db.folderAncestors("library-a", "missing").error.isEmpty());
        QCOMPARE(db.search("", "library-a", "", false, 0, 50, false).tracks.size(), 1);
        QCOMPARE(db.search("", "library-a", "现代/伤感", false, 0, 50, false).tracks.size(), 1);
        QCOMPARE(db.search("", "library-a", "现代/伤感").tracks.size(), 2);
        QVERIFY(!db.upsertFolders("library-a", {"../escape"}));
    }
    void directoryPaginationDoesNotTruncateBeyondTenThousand() {
        QTemporaryDir dir;
        Index db(dir.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "库", "Z:/offline", ""}));
        QStringList paths;
        for (int i = 0; i < 20000; ++i)
            paths << QString("目录%1").arg(i, 5, 10, QChar('0'));
        QVERIFY(db.upsertFolders("library-a", paths));
        QCOMPARE(db.folders("library-a").size(), 20001);
        const auto tail = db.childFolders("library-a", "", 19950, 100);
        QCOMPARE(tail.folders.size(), 50);
        QVERIFY(!tail.more);
        QCOMPARE(tail.folders.last().path, QString("目录19999"));
        const auto found = db.findFolders("目录19999");
        QCOMPARE(found.folders.size(), 1);
        QCOMPARE(found.folders[0].path, QString("目录19999"));
    }
    void snapshotMigratesLegacyIndexWithoutChangingSource() {
        QTemporaryDir dir;
        const auto source = dir.filePath("old.db"), target = dir.filePath("new.db");
        {
            Index db(source);
            QVERIFY(db.open());
            QVERIFY(db.saveLibrary({"library-a", "配乐", "Z:/offline", ""}));
            QVERIFY(db.upsert({track("古风/伤感/雪.wav")}));
            QVERIFY(db.setPersonal(db.search("").tracks[0].id, true, "女主"));
        }
        {
            auto legacy = QSqlDatabase::addDatabase("QSQLITE", "legacy-fixture");
            legacy.setDatabaseName(source);
            QVERIFY(legacy.open());
            QSqlQuery q(legacy);
            QVERIFY(q.exec("DROP TABLE folder_fts"));
            QVERIFY(q.exec("DROP TABLE folders"));
            QVERIFY(q.exec("PRAGMA user_version=2"));
            q.finish();
            legacy.close();
        }
        QSqlDatabase::removeDatabase("legacy-fixture");
        QString error;
        QVERIFY2(Index::importSnapshot(source, target, &error), qPrintable(error));
        Index migrated(target);
        QVERIFY2(migrated.open(&error), qPrintable(error));
        QCOMPARE(migrated.findFolders("古风").folders.size(), 2);
        QCOMPARE(migrated.search("女主", {}, {}, true).tracks.size(), 1);
        QVERIFY(Index::importSnapshot(source, target, &error)); // must not overwrite a newer profile
        {
            auto legacy = QSqlDatabase::addDatabase("QSQLITE", "legacy-readback");
            legacy.setDatabaseName(source);
            QVERIFY(legacy.open());
            QSqlQuery q(legacy);
            QVERIFY(q.exec("PRAGMA user_version"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 2);
            q.finish();
            legacy.close();
        }
        QSqlDatabase::removeDatabase("legacy-readback");
    }
    void directoryAncestorsAreIndexedWithoutNetwork() {
        QTemporaryDir dir;
        Index db(dir.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "配乐", "Z:/not-connected", ""}));
        QVERIFY(db.upsert({track("短剧/古风/伤感/雨.wav"), track("短剧/现代/伤感/雪.wav")}));
        const auto paths = db.folders("library-a");
        QVERIFY(paths.contains("短剧"));
        QVERIFY(paths.contains("短剧/古风"));
        QVERIFY(paths.contains("短剧/现代/伤感"));
    }
    void chineseFolderAndFilenameSearchPersistsOffline() {
        QTemporaryDir dir;
        auto path = dir.filePath("index.db");
        {
            Index db(path);
            QString error;
            QVERIFY2(db.open(&error), qPrintable(error));
            QVERIFY(db.saveLibrary({"library-a", "配乐", "Z:/not-connected", ""}));
            QVERIFY(db.upsert(
                {track("短剧/伤感/雨夜 Piano.wav"), track("短剧/紧张/雨夜.wav"), track("喜剧/晴天.mp3")}));
            QCOMPARE(db.search("伤感").tracks.size(), 1);
            QCOMPARE(db.search("伤感 雨夜").tracks[0].name, QString("雨夜 Piano.wav"));
            QCOMPARE(db.search("PIANO").tracks.size(), 1);
            QCOMPARE(db.search("雨").tracks.size(), 2);
            QCOMPARE(db.search("短剧").tracks.size(), 2);
            QCOMPARE(db.search("雨夜 紧张").tracks.size(), 1);
            QCOMPARE(db.search("\" OR * --").tracks.size(), 0);
            QCOMPARE(db.search("", "library-a", "短剧/伤感").tracks.size(), 1);
        }
        Index reopened(path);
        QVERIFY(reopened.open());
        QCOMPARE(reopened.search("伤感").tracks.size(), 1);
        QCOMPARE(reopened.count(), 3);
    }
    void personalMetadataSurvivesRescanAndSupportsSearch() {
        QTemporaryDir dir;
        Index db(dir.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "配乐", dir.path(), ""}));
        QVERIFY(db.upsert({track("伤感/夜.wav")}));
        auto rows = db.search("").tracks;
        QCOMPARE(rows.size(), 1);
        QVERIFY(db.setPersonal(rows[0].id, true, "回忆 女主"));
        QVERIFY(db.upsert({track("伤感/夜.wav", 99)}));
        auto result = db.search("女主", {}, {}, true).tracks;
        QCOMPARE(result.size(), 1);
        QCOMPARE(result[0].size, 99);
        QCOMPARE(result[0].tags, QString("回忆 女主"));
    }
    void paginationDoesNotSkipOrRepeatAndScopeIsLiteral() {
        QTemporaryDir dir;
        Index db(dir.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "配乐", dir.path(), ""}));
        QList<Track> input;
        for (int i = 0; i < 125; ++i)
            input << track(QString("100%/片段%1.wav").arg(i, 3, 10, QChar('0')));
        input << track("100abc/不该出现.wav");
        QVERIFY(db.upsert(input));
        QSet<qint64> ids;
        for (int offset = 0; offset < 125; offset += 50) {
            auto page = db.search("", {}, "100%", false, offset);
            QCOMPARE(page.more, offset < 100);
            for (const auto &t : page.tracks) {
                QVERIFY(!ids.contains(t.id));
                ids.insert(t.id);
            }
        }
        QCOMPARE(ids.size(), 125);
    }
    void stereoPeaksDoNotCancelAndMemoryIsBounded() {
        PeakBuilder builder(48000, 2);
        QVector<float> samples(48000 * 2);
        for (int i = 0; i < 48000; ++i) {
            samples[2 * i] = 0.75f;
            samples[2 * i + 1] = -0.75f;
        }
        for (int i = 0; i < 40; ++i)
            builder.append(samples.constData(), 48000);
        auto w = builder.finish();
        QCOMPARE(w.frames, qint64(1920000));
        QCOMPARE(w.channels, 2);
        QVERIFY(w.buckets.size() <= 2048);
        QVERIFY(!w.buckets.isEmpty());
        QVERIFY(w.buckets[0][0].maximum > 0.7f);
        QVERIFY(w.buckets[0][1].minimum < -0.7f);
        auto bytes = encodeWave(w);
        WaveData decoded;
        QVERIFY(decodeWave(bytes, &decoded));
        QCOMPARE(decoded.frames, w.frames);
        bytes[bytes.size() / 2] = char(bytes[bytes.size() / 2] ^ 0x01);
        QVERIFY(!decodeWave(bytes, &decoded));
        QVERIFY(!decodeWave(QByteArray("broken"), &decoded));
    }
    void sharedCacheSurvivesDuplicatePublishAndIgnoresPartialFiles() {
        QTemporaryDir dir;
        const auto library = ensureLibraryIdentity(dir.path());
        QVERIFY(!library.isEmpty());
        QCOMPARE(ensureLibraryIdentity(dir.path()), library);
        const auto key = waveKey(library, "情绪/雨.wav", 10, 20);
        QVERIFY(key != waveKey(library, "情绪/雨.wav", 10, 21));
        QVERIFY(key != waveKey(library, "情绪/雨.wav", 11, 20));
        PeakBuilder b(48000, 1);
        float samples[] = {-0.5f, 0.5f};
        b.append(samples, 2);
        auto wave = b.finish();
        QVERIFY(publishWave(dir.path(), key, wave));
        QVERIFY(publishWave(dir.path(), key, wave));
        WaveData result;
        QVERIFY(loadWave(dir.path(), key, &result));
        QCOMPARE(result.frames, 2);
        QVERIFY(!loadWave(dir.path(), waveKey(library, "情绪/雨.wav", 99, 20), &result));
        QFile broken(dir.filePath("library.json"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("bad");
        broken.close();
        QVERIFY(ensureLibraryIdentity(dir.path()).isEmpty());
    }
    void diagnosticViewsPreserveOlderCaches() {
        QTemporaryDir dir;
        PeakBuilder builder(48000, 1);
        float samples[] = {-0.25f, 0.25f};
        builder.append(samples, 2);
        const auto wave = builder.finish();
        const auto key = waveKey("a", "music.mp3", 100, 200);
        const QJsonObject expected{{"state", "decoded_with_warnings"}, {"issues", 120},
                                   {"message", "已解出可用音频"}, {"declaredMs", 37080}};
        QVERIFY(saveWaveView(dir.path(), key, wave, true, expected));
        bool reference = false;
        WaveData read;
        QJsonObject actual;
        QVERIFY(loadWaveView(dir.path(), key, &read, nullptr, &reference, &actual));
        QCOMPARE(actual, expected);
        QVERIFY(reference);
        QCOMPARE(read.frames, wave.frames);
        // Existing call sites can read new views without requesting diagnostics.
        QVERIFY(loadWaveView(dir.path(), key, &read, nullptr, &reference));
        for (bool oldReference : {true, false}) {
            QVERIFY(saveWaveView(dir.path(), key, wave, oldReference));
            actual = expected;
            QVERIFY(loadWaveView(dir.path(), key, &read, nullptr, &reference, &actual));
            QCOMPARE(reference, oldReference);
            QVERIFY(actual.isEmpty());
        }
    }
    void dragExportsOriginalWholeFileEvenWithoutCache() {
        auto t = track("伤感/雨 夜.wav");
#ifdef Q_OS_WIN
        t.root = "Z:/Music";
        auto urls = originalFileUrls({t});
        QCOMPARE(urls.size(), 1);
        QVERIFY(urls[0].isLocalFile());
        QCOMPARE(urls[0].toLocalFile(), QString("Z:/Music/伤感/雨 夜.wav"));
#else
        QList<QUrl> urls;
#endif
        t.root = "/Volumes/Music";
        urls = originalFileUrls({t});
        QCOMPARE(urls.size(), 1);
        QVERIFY(urls[0].isLocalFile());
        QCOMPARE(urls[0].toLocalFile(), QString("/Volumes/Music/伤感/雨 夜.wav"));
#ifdef Q_OS_WIN
        t.root = "//nas/music";
        urls = originalFileUrls({t});
        QCOMPARE(urls[0].host(), QString("nas"));
#endif
    }
    void updatedIndexSignalsPaginationRestartAndUnicodeScopeWorks() {
        QTemporaryDir dir;
        Index db(dir.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "配乐", dir.path(), ""}));
        QVERIFY(db.upsert({track("🎵音乐/伤感/雨.wav")}));
        const auto page = db.search("", {}, "🎵音乐");
        QCOMPARE(page.tracks.size(), 1);
        QVERIFY(db.upsert({track("🎵音乐/伤感/雪.wav")}));
        QVERIFY(db.search("").generation > page.generation);
    }
    void conflictingLibraryRootCannotRetargetOriginalFiles() {
        QTemporaryDir dir;
        Index db(dir.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library-a", "A", "Z:/Original-A", "Z:/Cache"}));
        QVERIFY(db.upsert({track("曲目/同名.wav")}));
        QVERIFY(!db.saveLibrary({"library-a", "B", "Z:/Unrelated-B", "Z:/Cache"}));
        QCOMPARE(db.search("").tracks[0].absolutePath(), QString("Z:/Original-A/曲目/同名.wav"));
    }
};
QTEST_GUILESS_MAIN(CoreTests)
#include "core_tests.moc"
