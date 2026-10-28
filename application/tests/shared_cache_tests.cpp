#include "cache.h"
#include "file_publication.h"
#include "index.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSemaphore>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>
#include <array>

class SharedCacheTests : public QObject {
    Q_OBJECT
  private slots:
    void hardLinkFallbackNeverOverwritesWinner() {
#ifdef Q_OS_WIN
        QSKIP("POSIX hard-link publication is exercised on macOS/Linux");
#else
        QTemporaryDir folder;
        QVERIFY(folder.isValid());
        const auto target = folder.filePath("library.json");
        QSemaphore ready, start;
        std::array<bool, 6> published{};
        std::array<QByteArray, 6> contents;
        std::array<QThread *, 6> threads;
        for (int i = 0; i < 6; ++i) {
            contents[i] = QByteArray(64 * 1024, char('a' + i));
            const auto source = target + QString(".pending-%1").arg(i);
            QFile stage(source);
            QVERIFY(stage.open(QIODevice::WriteOnly));
            QCOMPARE(stage.write(contents[i]), contents[i].size());
            stage.close();
            threads[i] = QThread::create([&, i, source] {
                ready.release();
                start.acquire();
                published[i] = file_publication_detail::publishByHardLinkExclusive(source, target);
            });
            threads[i]->start();
        }
        ready.acquire(6);
        start.release(6);
        for (auto *thread : threads) {
            QVERIFY(thread->wait());
            delete thread;
        }
        int winner = -1;
        int winners = 0;
        for (int i = 0; i < 6; ++i) {
            if (published[i]) {
                winner = i;
                ++winners;
            }
        }
        QCOMPARE(winners, 1);
        QFile result(target);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(result.readAll(), contents[winner]);
        result.close();
        const auto loser = target + ".late-pending";
        QFile lateStage(loser);
        QVERIFY(lateStage.open(QIODevice::WriteOnly));
        QCOMPARE(lateStage.write("late"), qint64(4));
        lateStage.close();
        QVERIFY(!file_publication_detail::publishByHardLinkExclusive(loser, target));
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(result.readAll(), contents[winner]);
#endif
    }
    void concurrentPublicationNeverModifiesOrRemovesWinner() {
        QTemporaryDir folder;
        for (int round = 0; round < 100; ++round) {
            const auto target = folder.filePath(QString("published-%1").arg(round));
            QSemaphore ready, start;
            std::array<bool, 6> published;
            std::array<QByteArray, 6> contents;
            std::array<QThread *, 6> threads;
            for (int i = 0; i < 6; ++i) {
                contents[i] = QByteArray(256 * 1024, char('a' + i));
                const auto source = target + QString(".pending-%1").arg(i);
                QFile stage(source);
                QVERIFY(stage.open(QIODevice::WriteOnly));
                QCOMPARE(stage.write(contents[i]), contents[i].size());
                stage.close();
                threads[i] = QThread::create([&, i, source] {
                    ready.release();
                    start.acquire();
                    published[i] = publishFileExclusive(source, target);
                });
                threads[i]->start();
            }
            ready.acquire(6);
            start.release(6);
            for (auto *thread : threads) {
                thread->wait();
                delete thread;
            }
            int winner = -1, winners = 0;
            for (int i = 0; i < 6; ++i)
                if (published[i]) {
                    ++winners;
                    winner = i;
                }
            QCOMPARE(winners, 1);
            QFile result(target);
            QVERIFY2(result.open(QIODevice::ReadOnly), qPrintable(QString("winner removed in round %1").arg(round)));
            QCOMPARE(result.readAll(), contents[winner]);
            result.close();
            // A known existing winner must survive both failed publication and loser cleanup.
            QFile rejected(target + ".rejected");
            QVERIFY(rejected.open(QIODevice::WriteOnly));
            QCOMPARE(rejected.write("not the winner"), qint64(14));
            rejected.close();
            QVERIFY(!publishFileExclusive(rejected.fileName(), target));
            QVERIFY(rejected.remove());
            QVERIFY(result.open(QIODevice::ReadOnly));
            QCOMPARE(result.readAll(), contents[winner]);
        }
    }
    void concurrentMigrationPreservesOneCompleteOriginalBackup() {
        QTemporaryDir folder;
        for (int round = 0; round < 30; ++round) {
            const auto path = folder.filePath(QString("library-%1.sqlite").arg(round));
            {
                Index original(path);
                QVERIFY(original.open());
                QVERIFY(original.saveLibrary({"original", "配乐", folder.filePath("music"), ""}));
                Track track;
                track.libraryId = "original";
                track.name = "song.wav";
                track.relativePath = track.name;
                track.folder = ".";
                QVERIFY(original.upsert({track}));
                QVERIFY(original.setPersonal(original.search("").tracks[0].id, true, "个人标签"));
            }
            const auto connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
                db.setDatabaseName(path);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec("ALTER TABLE libraries DROP COLUMN shared_id"));
                QVERIFY(q.exec("PRAGMA user_version=3"));
            }
            QSqlDatabase::removeDatabase(connection);
            QSemaphore ready, start;
            std::array<bool, 6> opened;
            std::array<QString, 6> errors;
            std::array<QThread *, 6> threads;
            for (int i = 0; i < 6; ++i) {
                threads[i] = QThread::create([&, i] {
                    Index db(path);
                    ready.release();
                    start.acquire();
                    opened[i] = db.open(&errors[i]);
                });
                threads[i]->start();
            }
            ready.acquire(6);
            start.release(6);
            for (auto *thread : threads) {
                thread->wait();
                delete thread;
            }
            for (int i = 0; i < 6; ++i)
                QVERIFY2(opened[i], qPrintable(QString("round %1: %2").arg(round).arg(errors[i])));
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
                db.setDatabaseName(folder.filePath(QString("library-%1.pre-0.8.sqlite").arg(round)));
                db.setConnectOptions("QSQLITE_OPEN_READONLY");
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec("PRAGMA quick_check") && q.next());
                QCOMPARE(q.value(0).toString(), QString("ok"));
                QVERIFY(q.exec("PRAGMA user_version") && q.next());
                QCOMPARE(q.value(0).toInt(), 3);
                QVERIFY(q.exec("SELECT favorite,tags FROM tracks") && q.next());
                QCOMPARE(q.value(0).toInt(), 1);
                QCOMPARE(q.value(1).toString(), QString("个人标签"));
                QVERIFY(q.exec("PRAGMA table_info(libraries)"));
                while (q.next())
                    QVERIFY(q.value(1).toString() != "shared_id");
            }
            QSqlDatabase::removeDatabase(connection);
        }
    }
    void concurrentFirstLeaseCreationNeverMistakesParentRaceForUnavailable_data() {
        QTest::addColumn<bool>("precreated");
        QTest::newRow("existing-directories") << true;
        QTest::newRow("new-directories") << false;
    }
    void concurrentFirstLeaseCreationNeverMistakesParentRaceForUnavailable() {
        QFETCH(bool, precreated);
        QTemporaryDir folder;
        const auto key = waveKey("shared", "first.wav", 1, 2);
        for (int round = 0; round < 100; ++round) {
            const auto cache = folder.filePath(QString("fresh-%1").arg(round));
            if (precreated)
                QVERIFY(QDir().mkpath(QDir(cache).filePath("generation-v1/" + key.left(2))));
            QSemaphore ready, start, acquired, release;
            std::array<SharedWaveLease::State, 6> states;
            std::array<QThread *, 6> threads;
            for (int i = 0; i < 6; ++i) {
                threads[i] = QThread::create([&, i] {
                    SharedWaveLease lease(cache, key);
                    ready.release();
                    start.acquire();
                    states[i] = lease.acquireOrObserve();
                    acquired.release();
                    release.acquire(); // Owner cannot disappear during a peer's first acquisition.
                });
                threads[i]->start();
            }
            ready.acquire(6);
            start.release(6);
            acquired.acquire(6);
            release.release(6);
            for (auto *thread : threads) {
                thread->wait();
                delete thread;
            }
            int owners = 0;
            for (const auto state : states) {
                QVERIFY2(state != SharedWaveLease::State::Unavailable,
                         qPrintable(QString("new-directory race in round %1").arg(round)));
                QVERIFY(state == SharedWaveLease::State::Owned || state == SharedWaveLease::State::Waiting);
                owners += state == SharedWaveLease::State::Owned;
            }
            QCOMPARE(owners, 1);
        }
    }
    void portableViewRequiresExactIndexedIdentityAndNeverPromotesToFullArtifact() {
        QTemporaryDir folder;
        PeakBuilder builder(48000, 1);
        const float sample = .5f;
        builder.append(&sample, 1);
        const auto wave = builder.finish();
        const auto shared = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(publishPortableWave(folder.path(), shared, "目录/song.wav", 100, 200, wave, false));
        WaveData loaded;
        QVERIFY(loadPortableWave(folder.path(), shared, "目录/song.wav", 100, 200, &loaded));
        QCOMPARE(loaded.frames, qint64(1));
        QVERIFY(!loadPortableWave(folder.path(), shared, "目录/song.wav", 101, 200, &loaded));
        QVERIFY(!loadPortableWave(folder.path(), shared, "目录/song.wav", 100, 201, &loaded));
        QVERIFY(!loadPortableWave(folder.path(), shared, "other/song.wav", 100, 200, &loaded));
        QVERIFY(!loadPortableWave(folder.path(), "different", "目录/song.wav", 100, 200, &loaded));
        QVERIFY(!loadWave(folder.path(), waveKey(shared, "目录/song.wav", 100, 200), &loaded));
        QVERIFY(!QDir(folder.filePath("peaks-v1")).exists());
    }
    void approximatePortableViewIsRejectedBeforeCreatingAnySharedFiles() {
        QTemporaryDir folder;
        PeakBuilder builder(48000, 1);
        const float sample = .5f;
        builder.append(&sample, 1);
        const auto cache = folder.filePath("uncreated");
        QString error;
        QVERIFY(!publishPortableWave(cache, "shared", "song.wav", 100, 200, builder.finish(), true, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QDir(cache).exists());
    }
    void exactTrackLookupUsesLibraryAndRelativePath() {
        QTemporaryDir folder;
        Index db(folder.filePath("index.sqlite"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"one", "A", folder.filePath("A"), ""}));
        QVERIFY(db.saveLibrary({"two", "B", folder.filePath("B"), ""}));
        Track first, second;
        first.libraryId = "one";
        first.name = "same.wav";
        first.folder = "first";
        first.relativePath = "first/same.wav";
        first.size = 100;
        first.modified = 200;
        second = first;
        second.libraryId = "two";
        QVERIFY(db.upsert({first, second}));
        const auto found = db.trackByIdentity("two", "first/same.wav");
        QVERIFY(found);
        QCOMPARE(found->libraryId, QString("two"));
        QCOMPARE(found->root, folder.filePath("B"));
        QVERIFY(!db.trackByIdentity("one", "same.wav"));
        QVERIFY(!db.trackByIdentity("missing", "first/same.wav"));
    }
    void schemaThreeMigrationPreservesVerifiedBackupAndPersonalRows() {
        QTemporaryDir folder;
        const auto path = folder.filePath("library.sqlite");
        {
            Index db(path);
            QVERIFY(db.open());
            QVERIFY(db.saveLibrary({"original", "配乐", folder.filePath("music"), ""}));
            Track t;
            t.libraryId = "original";
            t.name = "song.wav";
            t.relativePath = t.name;
            t.folder = ".";
            QVERIFY(db.upsert({t}));
            QVERIFY(db.setPersonal(db.search("").tracks[0].id, true, "个人标签"));
        }
        const auto connection = QString("pre-shared-schema");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("ALTER TABLE libraries DROP COLUMN shared_id"));
            QVERIFY(q.exec("PRAGMA user_version=3"));
        }
        QSqlDatabase::removeDatabase(connection);
        Index migrated(path);
        QVERIFY(migrated.open());
        const auto backup = folder.filePath("library.pre-0.8.sqlite");
        QVERIFY(QFileInfo::exists(backup));
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(backup);
            db.setConnectOptions("QSQLITE_OPEN_READONLY");
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("SELECT favorite,tags FROM tracks"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 1);
            QCOMPARE(q.value(1).toString(), QString("个人标签"));
            QVERIFY(q.exec("PRAGMA user_version"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 3);
        }
        QSqlDatabase::removeDatabase(connection);
    }
    void unsafeExistingBackupBlocksMigrationWithoutOverwritingIt() {
        QTemporaryDir folder;
        const auto path = folder.filePath("library.sqlite");
        {
            Index db(path);
            QVERIFY(db.open());
        }
        const auto connection = QString("blocked-shared-schema");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("PRAGMA user_version=3"));
        }
        QSqlDatabase::removeDatabase(connection);
        QFile backup(folder.filePath("library.pre-0.8.sqlite"));
        QVERIFY(backup.open(QIODevice::WriteOnly));
        backup.write("keep this file");
        backup.close();
        Index db(path);
        QString error;
        QVERIFY(!db.open(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(backup.open(QIODevice::ReadOnly));
        QCOMPARE(backup.readAll(), QByteArray("keep this file"));
    }
    void attachRetainsLocalIdentityTracksPersonalDataAndOfflineWaveView() {
        QTemporaryDir folder;
        Index db(folder.filePath("index.sqlite"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"original-local-id", "配乐", folder.filePath("music"), ""}));
        Track t;
        t.libraryId = "original-local-id";
        t.name = "雨夜.wav";
        t.folder = ".";
        t.relativePath = t.name;
        t.size = 100;
        t.modified = 200;
        QVERIFY(db.upsert({t}));
        const auto id = db.search("").tracks[0].id;
        QVERIFY(db.setPersonal(id, true, "女主 回忆"));
        PeakBuilder builder(48000, 1);
        const float sample = .25;
        builder.append(&sample, 1);
        const auto view = waveKey(t.libraryId, t.relativePath, t.size, t.modified);
        QVERIFY(saveWaveView(folder.path(), view, builder.finish()));
        const auto sharedId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(db.attachSharedCache(t.libraryId, folder.filePath("shared"), sharedId));
        const auto library = db.libraries()[0];
        QCOMPARE(library.id, t.libraryId);
        QCOMPARE(library.sharedId, sharedId);
        QCOMPARE(db.search("女主", {}, {}, true).tracks[0].id, id);
        WaveData cached;
        QVERIFY(loadWaveView(folder.path(), view, &cached));
        QCOMPARE(cached.frames, qint64(1));
        Index reopened(folder.filePath("index.sqlite"));
        QVERIFY(reopened.open());
        QCOMPARE(reopened.libraries()[0].sharedId, sharedId);
    }
    void sameSharedIdentityCannotRetargetAnotherLocalLibrary() {
        QTemporaryDir folder;
        Index db(folder.filePath("index.sqlite"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"one", "A", folder.filePath("A"), ""}));
        QVERIFY(db.saveLibrary({"two", "B", folder.filePath("B"), ""}));
        const auto sharedId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(db.attachSharedCache("one", folder.filePath("shared"), sharedId));
        QString error;
        QVERIFY(!db.attachSharedCache("two", folder.filePath("shared"), sharedId, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(db.libraries()[1].sharedCache, QString());
        QVERIFY(!db.attachSharedCache("missing", folder.filePath("shared"), sharedId));
    }
    void dedicatedNestedCacheAllowedButMusicRootAndParentRejected() {
        QTemporaryDir folder;
        const auto music = folder.filePath("音乐");
        QVERIFY(QDir().mkpath(music));
        QString cache, id, error;
        QVERIFY(!prepareSharedCache(music, music, &cache, &id, &error));
        QVERIFY(!prepareSharedCache(music, folder.path(), &cache, &id, &error));
        QVERIFY(prepareSharedCache(music, QDir(music).filePath("音频缓存"), &cache, &id, &error));
        QVERIFY(!QUuid(id).isNull());
        const auto original = id;
        QVERIFY(prepareSharedCache(music, cache, &cache, &id, &error));
        QCOMPARE(id, original);
        QFile foreign(folder.filePath("foreign.txt"));
        QVERIFY(foreign.open(QIODevice::WriteOnly));
        foreign.write("not a cache");
        foreign.close();
        QVERIFY(!prepareSharedCache(music, folder.path(), &cache, &id, &error));
    }
    void activeOwnerIsNeverRemovedAndExpiredLeaseAllowsBoundedFallback() {
        QTemporaryDir folder;
        const auto key = waveKey("shared", "a.wav", 1, 2);
        SharedWaveLease owner(folder.path(), key, 500);
        QCOMPARE(owner.acquireOrObserve(), SharedWaveLease::State::Owned);
        {
            SharedWaveLease waiter(folder.path(), key, 500);
            QCOMPARE(waiter.acquireOrObserve(), SharedWaveLease::State::Waiting);
            QTest::qWait(125);
            QVERIFY(owner.renew());
            QCOMPARE(waiter.acquireOrObserve(), SharedWaveLease::State::Waiting);
            QTest::qWait(125);
            QCOMPARE(waiter.acquireOrObserve(), SharedWaveLease::State::Waiting);
            QTest::qWait(450);
            QCOMPARE(waiter.acquireOrObserve(), SharedWaveLease::State::Expired);
        }
        QVERIFY(owner.renew()); // Even an expired observer must not remove another owner's file.
        SharedWaveLease later(folder.path(), key, 100);
        QCOMPARE(later.acquireOrObserve(), SharedWaveLease::State::Waiting);
    }
    void releasedOwnerAllowsNextClientToAcquire() {
        QTemporaryDir folder;
        const auto key = waveKey("shared", "a.wav", 1, 2);
        {
            SharedWaveLease owner(folder.path(), key);
            QCOMPARE(owner.acquireOrObserve(), SharedWaveLease::State::Owned);
        }
        SharedWaveLease next(folder.path(), key);
        QCOMPARE(next.acquireOrObserve(), SharedWaveLease::State::Owned);
    }
};
QTEST_GUILESS_MAIN(SharedCacheTests)
#include "shared_cache_tests.moc"
