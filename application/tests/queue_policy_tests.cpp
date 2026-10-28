#include "wave_queue.h"
#include "workers.h"
#include <QApplication>
#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSaveFile>
#include <algorithm>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

class QueuePolicyTests : public QObject {
    Q_OBJECT
    static bool writeWav(const QString &path, int frames = 4800) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        QDataStream out(&f);
        out.setByteOrder(QDataStream::LittleEndian);
        out.writeRawData("RIFF", 4);
        out << quint32(36 + frames * 2);
        out.writeRawData("WAVEfmt ", 8);
        out << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000) << quint16(2)
            << quint16(16);
        out.writeRawData("data", 4);
        out << quint32(frames * 2);
        for (int i = 0; i < frames; ++i)
            out << qint16(i % 100 < 50 ? 4000 : -4000);
        return out.status() == QDataStream::Ok;
    }
    static QList<Track> seed(const QString &root, const QStringList &names) {
        Index db(QDir(root).filePath("library.sqlite"));
        if (!db.open() || !db.saveLibrary({"qa", "测试", root, ""}))
            return {};
        QList<Track> tracks;
        for (const auto &name : names) {
            Track t;
            t.libraryId = "qa";
            t.name = t.relativePath = name;
            t.folder = ".";
            QFileInfo info(QDir(root).filePath(name));
            t.size = info.exists() ? info.size() : 9644;
            t.modified = info.exists() ? info.lastModified().toMSecsSinceEpoch() : 1000;
            tracks << t;
        }
        if (!db.upsert(tracks))
            return {};
        return db.search("", {}, {}, false, 0, 200).tracks;
    }
    static QString storePath(const QString &root) {
        return QDir(root).filePath("waveform-failures.sqlite");
    }
    static bool failures(const QString &root, const QList<Track> &tracks) {
        FailureStore store(storePath(root), QDir(root).filePath("library.sqlite"));
        QString error;
        if (!store.open(&error))
            return false;
        QList<FailureOutcome> outcomes;
        for (const auto &t : tracks)
            outcomes.append({t, false, false, "source_unavailable", "原件暂不可访问", 0});
        return store.apply(outcomes, &error);
    }
    static bool ledgerSql(const QString &root, const QString &sql) {
        const QString connection = "queue-policy-inject";
        bool ok = false;
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(storePath(root));
            ok = db.open() && QSqlQuery(db).exec(sql);
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        return ok;
    }
    static bool references(const QString &root, const QList<Track> &tracks) {
        PeakBuilder builder(48000, 1);
        const float samples[256] = {.2f};
        builder.append(samples, 256);
        QList<FailureOutcome> outcomes;
        for (const auto &track : tracks) {
            if (!saveWaveView(QDir(root).filePath("peaks"), WaveQueue::key(track), builder.finish(), true))
                return false;
            outcomes.append({track, true, true, "reference", "原参考波形", 0});
        }
        FailureStore store(storePath(root), QDir(root).filePath("library.sqlite"));
        QString error;
        return store.open(&error) && store.apply(outcomes, &error);
    }
    static QJsonObject checkpoint(const QString &root) {
        QFile file(QDir(root).filePath("progress.json"));
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(file.readAll()).object();
    }
  private slots:
    void cachedWarningsRestoreDetailsAndDurationWithoutSourceReads() {
        QTemporaryDir data;
        Track track;
        track.id = 1;
        track.libraryId = "diagnostics";
        track.root = data.filePath("unavailable-source");
        track.relativePath = track.name = "warning.mp3";
        track.size = 100;
        track.modified = 200;
        PeakBuilder builder(48000, 1);
        float samples[] = {-0.25f, 0.25f};
        builder.append(samples, 2);
        const QJsonObject metadata{{"declaredMs", 37080}, {"decodedMs", 37049},
                                   {"message", "已解出可用音频（有异常）"}, {"issues", 3}};
        const auto cache = data.filePath("peaks");
        QVERIFY(saveWaveView(cache, WaveQueue::key(track), builder.finish(), true, metadata));
        WaveQueue queue(cache);
        queue.setWorkContext(true, false, false);
        QSignalSpy jobs(&queue, &WaveQueue::jobFinished);
        queue.setVisible({track});
        QTRY_VERIFY(queue.find(track) && queue.find(track)->stored);
        const auto *entry = queue.find(track);
        QCOMPARE(entry->message, metadata["message"].toString());
        QCOMPARE(entry->estimatedDuration, 37080);
        QCOMPARE(entry->failureKind, QString("decode_warning"));
        QVERIFY(entry->approximate && !entry->failed);
        QCOMPARE(jobs.count(), 0);
    }
    void parallelCompletionCommitsInOrderAndPreservesFailures() {
        QTemporaryDir data;
        const auto tracks = seed(data.path(), {"A.wav", "B.wav", "C-fail.wav", "D.wav", "E.wav"});
        QCOMPARE(tracks.size(), 5);
        qputenv("AUDIO_TEST_PARALLEL_GATE", data.path().toUtf8());
        auto cleanup = qScopeGuard([] { qunsetenv("AUDIO_TEST_PARALLEL_GATE"); });
        auto release = [&](const QString &name) {
            QFile file(data.filePath(name + ".release"));
            return file.open(QIODevice::WriteOnly);
        };
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.setPreparationConcurrency(4);
        queue.prepareAll(10 * 1048576);
        QTRY_COMPARE(queue.parallelActiveCount(), 4);
        QSet<qint64> pids;
        for (int i = 0; i < 4; ++i) {
            QFile config(data.filePath(tracks[i].name + ".json"));
            QTRY_VERIFY(config.exists());
            QVERIFY(config.open(QIODevice::ReadOnly));
            const auto job = QJsonDocument::fromJson(config.readAll()).object();
            QCOMPARE(job["readBytesPerSecond"].toInteger(), 10 * 1048576 / 4);
            QVERIFY(job["background"].toBool());
            pids.insert(job["pid"].toInteger());
        }
        QCOMPARE(pids.size(), 4);
        for (const auto &name : {"B.wav", "C-fail.wav", "D.wav"})
            QVERIFY(release(name));
        QTRY_COMPARE(queue.parallelActiveCount(), 1);
        QCOMPARE(queue.preparation()["cursor"].toInteger(), 0);
        QCOMPARE(queue.preparation()["done"].toInteger(), 0);
        QCOMPARE(checkpoint(data.path())["cursor"].toInteger(), 0);
        QVERIFY(!QFileInfo::exists(data.filePath("E.wav.json")));
        QVERIFY(release("A.wav"));
        QTRY_VERIFY(QFileInfo::exists(data.filePath("E.wav.json")));
        QVERIFY(release("E.wav"));
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 4);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 1);
        QTRY_COMPARE(checkpoint(data.path())["cursor"].toInteger(), tracks.last().id);
    }

    void parallelPauseAndAuditionResumeWithoutLosingTracks() {
        QTemporaryDir data;
        const auto tracks = seed(data.path(), {"A.wav", "B.wav", "C.wav", "D.wav"});
        QCOMPARE(tracks.size(), 4);
        qputenv("AUDIO_TEST_PARALLEL_GATE", data.path().toUtf8());
        auto cleanup = qScopeGuard([] { qunsetenv("AUDIO_TEST_PARALLEL_GATE"); });
        {
            WaveQueue queue(data.filePath("peaks"));
            queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
            queue.setPreparationConcurrency(4);
            queue.prepareAll(0);
            QTRY_COMPARE(queue.parallelActiveCount(), 4);
            queue.setWorkContext(true, false, false);
            QTRY_VERIFY(std::all_of(queue.parallel_.cbegin(), queue.parallel_.cend(),
                                   [](const auto &slot) { return !slot.worker || !slot.worker->active_.id; }));
            QCOMPARE(queue.parallelActiveCount(), 0);
            QVERIFY(queue.preparation()["running"].toBool());
            queue.select(tracks[0]);
            QTRY_VERIFY(queue.active_.id == tracks[0].id);
            QVERIFY(!queue.background_);
            QCOMPARE(queue.activeRate_, 0);
            QFile release(data.filePath("A.wav.release"));
            QVERIFY(release.open(QIODevice::WriteOnly));
            release.close();
            QTRY_VERIFY(queue.find(tracks[0]) && queue.find(tracks[0])->complete);
            queue.setWorkContext(false, false, false);
            QTRY_VERIFY(queue.parallelActiveCount() >= 2);
            queue.pausePreparation();
            QTRY_VERIFY(std::all_of(queue.parallel_.cbegin(), queue.parallel_.cend(),
                                   [](const auto &slot) { return !slot.worker || !slot.worker->active_.id; }));
            QVERIFY(!queue.preparation()["running"].toBool());
            QTRY_VERIFY(!queue.failureWriting_ && !queue.checkpointDirty_);
        }
        WaveQueue reopened(data.filePath("peaks"));
        reopened.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QVERIFY(!reopened.preparation()["running"].toBool());
        QCOMPARE(reopened.preparation()["rate"].toInteger(), 0);
        reopened.setPreparationConcurrency(2);
        QFile releaseAll(data.filePath("release-all"));
        QVERIFY(releaseAll.open(QIODevice::WriteOnly));
        releaseAll.close();
        reopened.prepareAll(0);
        QTRY_VERIFY(reopened.preparation()["complete"].toBool());
        QCOMPARE(reopened.preparation()["done"].toInteger(), 4);
        QCOMPARE(reopened.preparation()["failed"].toInteger(), 0);
    }

    void realParallelDecodingMatchesSerialCache() {
        QTemporaryDir data;
        for (const auto &name : {"A.wav", "B.wav", "C.wav", "D.wav"})
            QVERIFY(writeWav(data.filePath(name), 480000));
        const auto tracks = seed(data.path(), {"A.wav", "B.wav", "C.wav", "D.wav"});
        int peakJobs = 0;
        {
            WaveQueue queue(data.filePath("parallel-peaks"));
            queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
            queue.setPreparationConcurrency(4);
            connect(&queue, &WaveQueue::preparationChanged, this, [&] {
                peakJobs = qMax(peakJobs, queue.preparation()["activeJobs"].toInt());
            });
            queue.prepareAll(0);
            QTRY_VERIFY(queue.preparation()["complete"].toBool());
            QCOMPARE(queue.preparation()["done"].toInteger(), 4);
            QCOMPARE(queue.preparation()["failed"].toInteger(), 0);
            QCOMPARE(peakJobs, 4);
            QTRY_VERIFY(std::all_of(queue.parallel_.cbegin(), queue.parallel_.cend(),
                                   [](const auto &slot) { return !slot.worker || !slot.worker->process_; }));
        }
        WaveQueue serial(data.filePath("serial-peaks"));
        serial.configurePreparation(data.filePath("library.sqlite"), data.filePath("serial-progress.json"));
        serial.prepareAll(0);
        QTRY_VERIFY(serial.preparation()["complete"].toBool());
        for (const auto &track : tracks) {
            WaveData parallel, single;
            QVERIFY(loadWaveView(data.filePath("parallel-peaks"), WaveQueue::key(track), &parallel));
            QVERIFY(loadWaveView(data.filePath("serial-peaks"), WaveQueue::key(track), &single));
            QCOMPARE(encodeWave(parallel), encodeWave(single));
        }
    }

    void visiblePriorityNeverAddsAFifthDecoder() {
        QTemporaryDir data;
        const auto tracks = seed(data.path(), {"A.wav", "B.wav", "C.wav", "D.wav", "E.wav"});
        qputenv("AUDIO_TEST_PARALLEL_GATE", data.path().toUtf8());
        auto cleanup = qScopeGuard([] { qunsetenv("AUDIO_TEST_PARALLEL_GATE"); });
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.setPreparationConcurrency(4);
        int peak = 0;
        connect(&queue, &WaveQueue::preparationChanged, this, [&] {
            peak = qMax(peak, queue.preparation()["activeJobs"].toInt());
        });
        queue.prepareAll(0);
        QTRY_COMPARE(queue.parallelActiveCount(), 4);
        queue.setVisible({tracks.last()});
        QTRY_VERIFY(queue.active_.id == tracks.last().id);
        QVERIFY2(peak <= 4, qPrintable(QString("Started %1 simultaneous jobs").arg(peak)));
        queue.pausePreparation();
    }

    void parallelControlsKeepTheWholeCatalog_data() {
        QTest::addColumn<QString>("action");
        for (const auto *action : {"two-jobs", "limited", "refresh", "skip"})
            QTest::newRow(action) << QString(action);
    }
    void parallelControlsKeepTheWholeCatalog() {
        QFETCH(QString, action);
        QTemporaryDir data;
        const auto tracks = seed(data.path(), {"A.wav", "B.wav", "C.wav", "D.wav"});
        qputenv("AUDIO_TEST_PARALLEL_GATE", data.path().toUtf8());
        auto cleanup = qScopeGuard([] { qunsetenv("AUDIO_TEST_PARALLEL_GATE"); });
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.setPreparationConcurrency(4);
        queue.prepareAll(0);
        QTRY_COMPARE(queue.parallelActiveCount(), 4);
        if (action == "two-jobs") {
            queue.setPreparationConcurrency(2);
            QTRY_COMPARE(queue.parallelActiveCount(), 2);
        } else if (action == "limited") {
            queue.prepareAll(1048576);
            QTRY_COMPARE(queue.parallelActiveCount(), 4);
            for (const auto &slot : queue.parallel_)
                if (slot.track.id)
                    QCOMPARE(slot.worker->activeRate_, 1048576 / 4);
        } else if (action == "refresh") {
            queue.refreshIndex();
            QVERIFY(!queue.preparation()["running"].toBool());
            QCOMPARE(queue.parallelActiveCount(), 0);
            QVERIFY(queue.preparation()["catalogNeedsReview"].toBool());
            queue.prepareAll(0);
        } else {
            queue.skipCurrent();
        }
        QFile release(data.filePath("release-all"));
        QVERIFY(release.open(QIODevice::WriteOnly));
        release.close();
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), action == "skip" ? 3 : 4);
        QCOMPARE(queue.preparation()["failed"].toInteger(), action == "skip" ? 1 : 0);
        QTRY_COMPARE(checkpoint(data.path())["cursor"].toInteger(), tracks.last().id);
    }

    void unlimitedRateSurvivesCheckpointAndRetryModes() {
        QTemporaryDir data;
        QVERIFY(!seed(data.path(), {"pending.wav"}).isEmpty());
        {
            WaveQueue queue(data.filePath("peaks"));
            queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
            queue.setWorkContext(true, false, false);
            queue.prepareAll(0);
            QCOMPARE(queue.preparation()["rate"].toInteger(), 0);
            QTRY_COMPARE(checkpoint(data.path())["rate"].toInteger(-1), 0);
        }
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.setWorkContext(true, false, false);
        QCOMPARE(queue.preparation()["rate"].toInteger(-1), 0);
        queue.retryFailed(1048576);
        QCOMPARE(queue.preparation()["rate"].toInteger(), 1048576);
        queue.retryFailed(0);
        QCOMPARE(queue.preparation()["rate"].toInteger(), 0);
        queue.retryReferences(10 * 1048576);
        queue.retryReferences(0);
        QCOMPARE(queue.preparation()["rate"].toInteger(), 0);
        queue.retryReferences();
        QCOMPARE(queue.preparation()["rate"].toInteger(), 0);
    }

    void referenceRetryPaginationDoesNotRevisitRetainedReferences() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 135; ++i)
            names << QString("reference%1.wav").arg(i, 3, 10, QChar('0'));
        const auto tracks = seed(data.path(), names);
        QCOMPARE(tracks.size(), 135);
        QVERIFY(references(data.path(), tracks));
        FailureStore store(storePath(data.path()), data.filePath("library.sqlite"));
        QString error;
        QVERIFY(store.open(&error));
        const auto upper = store.retryUpper(&error, true);
        const auto first = store.retryPage(0, upper, 100, &error, true);
        QCOMPARE(first.size(), 100);
        QList<FailureOutcome> retained;
        for (const auto &record : first) {
            QVERIFY(record.reference);
            retained.append({record.track, true, true, "reference", "仍是参考波形", record.id});
        }
        QVERIFY(store.apply(retained, &error));
        const auto tail = store.retryPage(first.last().id, upper, 100, &error, true);
        QCOMPARE(tail.size(), 35);
        QCOMPARE(tail.last().id, upper);
        QVERIFY(store.retryPage(upper, upper, 100, &error, true).isEmpty());
        QCOMPARE(store.summary()["knownReferences"].toInteger(), 135);
        QVERIFY(store.retryPage(0, store.retryUpper(&error), 100, &error).isEmpty());
    }
    void referenceRetryOnlyVisitsReferencesAndPreservesOriginalFiles() {
        QTemporaryDir data;
        for (const auto &name : {"accurate.wav", "failed.wav", "reference.wav"})
            QVERIFY(writeWav(data.filePath(name)));
        const auto tracks = seed(data.path(), {"accurate.wav", "failed.wav", "reference.wav"});
        QCOMPARE(tracks.size(), 3);
        QVERIFY(failures(data.path(), {tracks[1]}));
        QVERIFY(references(data.path(), {tracks[2]}));
        QFile source(tracks[2].absolutePath());
        QVERIFY(source.open(QIODevice::ReadOnly));
        const auto original = source.readAll();
        source.close();
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QTRY_COMPARE(queue.preparation()["knownReferences"].toInteger(), 1);
        queue.batchReference_ = 1;
        QSet<qint64> visited;
        connect(&queue, &WaveQueue::changed, this, [&](const Track &t) { visited.insert(t.id); });
        queue.retryReferences(1048576);
        QTRY_VERIFY(queue.preparation()["retryComplete"].toBool());
        QTRY_COMPARE(queue.preparation()["knownReferences"].toInteger(), 0);
        QCOMPARE(queue.preparation()["knownFailures"].toInteger(), 1);
        QCOMPARE(queue.preparation()["retryConverted"].toInteger(), 1);
        QCOMPARE(queue.preparation()["retryRemaining"].toInteger(), 0);
        QCOMPARE(queue.preparation()["reference"].toInteger(), 0);
        QCOMPARE(visited, QSet<qint64>{tracks[2].id});
        WaveData wave;
        bool approximate = true;
        QVERIFY(
            loadWaveView(data.filePath("peaks"), WaveQueue::key(tracks[2]), &wave, nullptr, &approximate));
        QVERIFY(!approximate);
        QVERIFY(source.open(QIODevice::ReadOnly));
        QCOMPARE(source.readAll(), original);
    }
    void referenceRetryResumesAfterRestartAndSwitchingModeResetsCursor() {
        QTemporaryDir data;
        for (const auto &name : {"A.wav", "B.wav", "C.wav"})
            QVERIFY(writeWav(data.filePath(name)));
        const auto tracks = seed(data.path(), {"A.wav", "B.wav", "C.wav"});
        QVERIFY(references(data.path(), {tracks[0], tracks[1]}));
        QVERIFY(failures(data.path(), {tracks[2]}));
        qint64 cursor = 0;
        {
            WaveQueue queue(data.filePath("peaks"));
            queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
            QTRY_VERIFY(queue.preparation()["failuresReady"].toBool());
            connect(&queue, &WaveQueue::preparationChanged, this, [&] {
                if (queue.preparation()["running"].toBool() &&
                    queue.preparation()["retryDone"].toInteger() == 1)
                    queue.pausePreparation();
            });
            queue.retryReferences(1048576);
            QTRY_COMPARE(queue.preparation()["retryDone"].toInteger(), 1);
            cursor = queue.preparation()["retryCursor"].toInteger();
            QTRY_COMPARE(checkpoint(data.path())["retryCursor"].toInteger(), cursor);
        }
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QTRY_VERIFY(queue.preparation()["failuresReady"].toBool());
        QCOMPARE(queue.preparation()["mode"].toString(), QString("reference"));
        QCOMPARE(queue.preparation()["rate"].toInteger(), 1048576);
        QVERIFY(!queue.preparation()["running"].toBool());
        QCOMPARE(queue.preparation()["retryCursor"].toInteger(), cursor);
        queue.retryReferences();
        QTRY_VERIFY(queue.preparation()["retryComplete"].toBool());
        QCOMPARE(queue.preparation()["retryDone"].toInteger(), 2);
        QCOMPARE(queue.preparation()["retryConverted"].toInteger(), 2);
        queue.retryFailed();
        QTRY_VERIFY(queue.preparation()["retryComplete"].toBool());
        QCOMPARE(queue.preparation()["mode"].toString(), QString("failed"));
        QCOMPARE(queue.preparation()["retryDone"].toInteger(), 1);
        QCOMPARE(queue.preparation()["retryConverted"].toInteger(), 0);
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 0);
    }
    void referenceRetryKeepsDisplayOnPartialFailure_data() {
        QTest::addColumn<QByteArray>("result");
        QTest::newRow("partial-failure") << QByteArray("partial-failure");
        QTest::newRow("still-reference") << QByteArray("reference");
        QTest::newRow("accurate-shared-failure") << QByteArray("shared-failure");
        QTest::newRow("manual-skip") << QByteArray("manual-skip");
    }
    void referenceRetryKeepsDisplayOnPartialFailure() {
        QFETCH(QByteArray, result);
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("reference.wav")));
        const auto tracks = seed(data.path(), {"reference.wav"});
        QVERIFY(references(data.path(), tracks));
        WaveData old;
        QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(tracks[0]), &old));
        const auto bytes = encodeWave(old);
        qputenv("AUDIO_TEST_REFERENCE_RESULT", result);
        auto restore = qScopeGuard([] { qunsetenv("AUDIO_TEST_REFERENCE_RESULT"); });
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QTRY_VERIFY(queue.preparation()["failuresReady"].toBool());
        queue.retryReferences(1048576);
        QTRY_VERIFY(queue.preparation()["decodedMs"].toInteger() > 0);
        const auto *entry = queue.find(tracks[0]);
        QVERIFY(entry && entry->complete && entry->approximate);
        QCOMPARE(encodeWave(entry->data), bytes);
        QCOMPARE(queue.activeRate_, 1048576);
        queue.setVisible({}); // Scrolling must not cancel an explicit reference job.
        QVERIFY(!queue.cancelled_);
        if (result == "manual-skip")
            queue.skipCurrent();
        QTRY_VERIFY(queue.preparation()["retryComplete"].toBool());
        const bool accurate = result == "shared-failure";
        QTRY_COMPARE(queue.preparation()["knownReferences"].toInteger(), accurate ? 0 : 1);
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), accurate ? 1 : 0);
        QCOMPARE(queue.preparation()["retryConverted"].toInteger(), accurate ? 1 : 0);
        QCOMPARE(queue.preparation()["retryRemaining"].toInteger(), accurate ? 0 : 1);
        QCOMPARE(queue.preparation()["retryFailed"].toInteger(), result == "reference" ? 0 : 1);
        entry = queue.find(tracks[0]);
        QVERIFY(entry && entry->complete && entry->stored);
        QCOMPARE(entry->approximate, !accurate);
        if (result == "partial-failure" || result == "manual-skip") {
            QCOMPARE(encodeWave(entry->data), bytes);
            WaveData retained;
            QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(tracks[0]), &retained));
            QCOMPARE(encodeWave(retained), bytes);
        }
    }
    void cachedCatalogReconciliationBackpressuresOneWriter() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 1000; ++i)
            names << QString("cached-%1.wav").arg(i, 4, 10, QChar('0'));
        QVERIFY(!seed(data.path(), names).isEmpty());
        Index index(data.filePath("library.sqlite"));
        QVERIFY(index.open());
        QList<Track> tracks;
        for (;;) {
            const auto page = index.tracksAfter(tracks.isEmpty() ? 0 : tracks.last().id);
            if (page.isEmpty())
                break;
            tracks.append(page);
        }
        QCOMPARE(tracks.size(), 1000);
        PeakBuilder builder(48000, 1);
        float samples[256] = {.2f};
        builder.append(samples, 256);
        const auto wave = builder.finish();
        for (int i = 0; i < tracks.size(); ++i)
            QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(tracks[i]), wave, i % 4 == 0));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QTRY_VERIFY(queue.preparation()["failuresReady"].toBool());
        QSemaphore entered, release;
        queue.failurePool_.start([&] {
            entered.release();
            release.acquire();
        });
        bool released = false;
        auto unblock = qScopeGuard([&] {
            if (!released)
                release.release();
            queue.failurePool_.waitForDone();
        });
        QTRY_VERIFY(entered.available() > 0);
        queue.prepareAll();
        queue.setVisible({tracks[0]});
        QTRY_VERIFY_WITH_TIMEOUT(queue.find(tracks[0]) && queue.find(tracks[0])->complete, 500);
        QTest::qWait(200);
        QVERIFY(queue.failureWriting_);
        QVERIFY(queue.pendingOutcomes_.size() <= WaveQueue::OutcomeChunkSize);
        QVERIFY(queue.batchCursor_ <= WaveQueue::OutcomeChunkSize);
        QVERIFY(queue.findChildren<QFutureWatcherBase *>().size() <= 3);
        release.release();
        released = true;
        QElapsedTimer timer;
        timer.start();
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 20000);
        QTRY_COMPARE(queue.preparation()["knownReferences"].toInteger(), 250);
        QTRY_VERIFY(!queue.failureWriting_ && !queue.checkpointDirty_);
        QCOMPARE(queue.preparation()["done"].toInteger(), 1000);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 0);
        QCOMPARE(queue.preparation()["reference"].toInteger(), 250);
        QCOMPARE(checkpoint(data.path())["cursor"].toInteger(), tracks.last().id);
        qInfo("Reconciled 1000 local views, including 250 reference views, in %lld ms after writer release",
              timer.elapsed());
    }
    void delayedFailureLedgerCannotBlockCachedWaveDisplay() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("cached.wav")));
        const auto tracks = seed(data.path(), {"cached.wav"});
        QCOMPARE(tracks.size(), 1);
        PeakBuilder builder(48000, 1);
        float samples[256] = {.2f};
        builder.append(samples, 256);
        QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(tracks[0]), builder.finish()));
        WaveQueue queue(data.filePath("peaks"));
        QSemaphore entered, release;
        queue.failurePool_.start([&] {
            entered.release();
            release.acquire();
        });
        auto unblock = qScopeGuard([&] {
            release.release();
            queue.failurePool_.waitForDone();
        });
        QTRY_VERIFY(entered.available() > 0);
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.setVisible(tracks);
        QTRY_VERIFY_WITH_TIMEOUT(queue.find(tracks[0]) && queue.find(tracks[0])->complete, 500);
        QVERIFY(!queue.preparation()["failuresReady"].toBool());
    }
    void busyContextDefersOnlyBackgroundAndShowsLocalViews() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("A.wav")));
        QVERIFY(writeWav(data.filePath("B.wav")));
        const auto tracks = seed(data.path(), {"A.wav", "B.wav"});
        QCOMPARE(tracks.size(), 2);
        PeakBuilder builder(48000, 1);
        float samples[256] = {.2f};
        builder.append(samples, 256);
        QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(tracks[0]), builder.finish()));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.setWorkContext(true, false, false);
        queue.setVisible(tracks);
        queue.prepareAll(1048576);
        QTRY_VERIFY(queue.find(tracks[0]) && queue.find(tracks[0])->complete);
        QTest::qWait(150);
        QVERIFY(!queue.active_.id);
        QVERIFY(queue.preparation()["running"].toBool());
        QVERIFY(queue.preparation()["deferred"].toBool());
        QVERIFY(queue.hasBackgroundWork());
        queue.select(tracks[1]);
        QTRY_VERIFY(queue.find(tracks[1]) && queue.find(tracks[1])->complete);
        QVERIFY(queue.preparation()["running"].toBool());
        queue.setWorkContext(false, false, false);
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 2);
        QVERIFY(!queue.workerWatchdog_.isActive());
    }
    void automaticDeferralKeepsIntentButUserPauseDoesNotResume() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("slow.wav"), 1536000));
        const auto tracks = seed(data.path(), {"slow.wav"});
        QCOMPARE(tracks.size(), 1);
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.prepareAll(1048576);
        QTRY_VERIFY(queue.active_.id);
        queue.setWorkContext(false, true, false);
        QTRY_VERIFY(!queue.active_.id);
        QVERIFY(queue.preparation()["running"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 0);
        queue.pausePreparation();
        queue.setWorkContext(false, false, false);
        QTest::qWait(100);
        QVERIFY(!queue.preparation()["running"].toBool());
        QVERIFY(!queue.active_.id);
        QVERIFY(!queue.workerWatchdog_.isActive());
    }
    void durableCatalogKeepsMoreThanRecentHundredAndReferenceSamples() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 135; ++i)
            names << QString("file-%1.wav").arg(i, 3, 10, QChar('0'));
        const auto tracks = seed(data.path(), names);
        QCOMPARE(tracks.size(), 135);
        QVERIFY(failures(data.path(), tracks));
        FailureStore store(storePath(data.path()), data.filePath("library.sqlite"));
        QString error;
        QVERIFY(store.open(&error));
        QCOMPARE(store.summary()["knownFailures"].toInteger(), 135);
        auto page = store.page(0, 100, "network", false, &error);
        QCOMPARE(page["items"].toArray().size(), 100);
        QVERIFY(page["more"].toBool());
        page = store.page(100, 100, {}, false, &error);
        QCOMPARE(page["items"].toArray().size(), 35);
        QVERIFY(!page["more"].toBool());
        QVERIFY(store.apply({{tracks[0], true, true, "reference", "含损坏片段", 0}}, &error));
        QCOMPARE(store.summary()["knownFailures"].toInteger(), 134);
        page = store.page(0, 10, {}, true, &error);
        QCOMPARE(page["total"].toInteger(), 1);
        QCOMPARE(page["items"].toArray()[0].toObject()["category"].toString(), QString("damaged"));
    }
    void legacyImportReportsUntrackedHistoryWithoutInventingRows() {
        QTemporaryDir data;
        const auto tracks = seed(data.path(), {"a.wav", "b.wav"});
        QCOMPARE(tracks.size(), 2);
        QJsonArray legacy;
        for (const auto &t : tracks)
            legacy.append(QJsonObject{{"id", t.id},
                                      {"library", t.libraryId},
                                      {"path", t.relativePath},
                                      {"name", t.name},
                                      {"kind", "timeout"},
                                      {"message", "旧失败"}});
        FailureStore store(storePath(data.path()), data.filePath("library.sqlite"));
        QString error;
        QVERIFY(store.open(&error));
        QVERIFY(store.importLegacy(legacy, 14, &error));
        QVERIFY(store.importLegacy(legacy, 14, &error));
        QCOMPARE(store.summary()["knownFailures"].toInteger(), 2);
        QCOMPARE(store.summary()["legacyUntracked"].toInteger(), 12);
    }
    void retryOnlyFailuresLeavesWholeLibraryCountsUnchanged() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("A-not-failed.wav")));
        QVERIFY(writeWav(data.filePath("B-repaired.wav")));
        QFile damaged(data.filePath("C-bad.mp3"));
        QVERIFY(damaged.open(QIODevice::WriteOnly));
        damaged.write("not audio");
        damaged.close();
        const auto tracks = seed(data.path(), {"A-not-failed.wav", "B-repaired.wav", "C-bad.mp3"});
        QCOMPARE(tracks.size(), 3);
        QVERIFY(failures(data.path(), {tracks[1], tracks[2]}));
        QVERIFY(QFile::remove(tracks[0].absolutePath()));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QTRY_VERIFY(queue.preparation()["failuresReady"].toBool());
        queue.batchDone_ = 10;
        queue.batchFailed_ = 2;
        queue.batchTotal_ = 12;
        QSet<qint64> requested;
        connect(&queue, &WaveQueue::changed, this, [&](const Track &t) { requested.insert(t.id); });
        queue.retryFailed();
        QTRY_VERIFY(queue.preparation()["retryComplete"].toBool());
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 1);
        QCOMPARE(queue.preparation()["retryDone"].toInteger(), 2);
        QCOMPARE(queue.preparation()["retryFailed"].toInteger(), 1);
        QCOMPARE(queue.preparation()["done"].toInteger(), 10);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 2);
        QVERIFY(!requested.contains(tracks[0].id));
        QCOMPARE(requested.size(), 2);
        QSignalSpy pages(&queue, &WaveQueue::failurePageReady);
        queue.requestFailurePage();
        QTRY_COMPARE(pages.size(), 1);
        const auto result = pages[0][0].toJsonObject();
        QCOMPARE(result["total"].toInteger(), 1);
        QCOMPARE(result["items"].toArray()[0].toObject()["category"].toString(), QString("damaged"));
    }
    void fullLibraryContinuesPastEightUnavailableSources_data() {
        QTest::addColumn<int>("concurrency");
        QTest::newRow("serial") << 1;
        QTest::newRow("two-workers") << 2;
        QTest::newRow("four-workers") << 4;
    }
    void fullLibraryContinuesPastEightUnavailableSources() {
        QFETCH(int, concurrency);
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 10; ++i)
            names << QString("%1-missing.wav").arg(i, 2, 10, QChar('0'));
        names << "99-readable.wav";
        QVERIFY(writeWav(data.filePath(names.last())));
        const auto tracks = seed(data.path(), names);
        QCOMPARE(tracks.size(), 11);
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.setPreparationConcurrency(concurrency);
        queue.prepareAll(0);
        QTRY_VERIFY(queue.preparation()["complete"].toBool() ||
                    (!queue.preparation()["running"].toBool() &&
                     queue.preparation()["failed"].toInteger() > 0));
        QVERIFY2(queue.preparation()["complete"].toBool(),
                 qPrintable(queue.preparation()["error"].toString()));
        QCOMPARE(queue.preparation()["done"].toInteger(), 1);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 10);
        QCOMPARE(queue.preparation()["total"].toInteger(), 11);
        QCOMPARE(queue.preparation()["failures"].toArray().size(), 10);
        QVERIFY(queue.preparation()["error"].toString().isEmpty());
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 10);
        QTRY_COMPARE(checkpoint(data.path())["cursor"].toInteger(), tracks.last().id);
        QCOMPARE(checkpoint(data.path())["failed"].toInteger(), 10);
        WaveData wave;
        QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(tracks.last()), &wave));
        FailureStore store(storePath(data.path()), data.filePath("library.sqlite"));
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        const auto records = store.page(0, 100, {}, false, &error)["items"].toArray();
        QCOMPARE(records.size(), 10);
        for (const auto &record : records)
            QCOMPARE(record.toObject()["kind"].toString(), QString("source_unavailable"));
    }
    void failedRetryContinuesPastEightUnavailableSources() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 10; ++i)
            names << QString("%1-missing.wav").arg(i, 2, 10, QChar('0'));
        names << "99-repaired.wav";
        QVERIFY(writeWav(data.filePath(names.last())));
        const auto tracks = seed(data.path(), names);
        QCOMPARE(tracks.size(), 11);
        QVERIFY(failures(data.path(), tracks));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QTRY_VERIFY(queue.preparation()["failuresReady"].toBool());
        queue.batchDone_ = 27;
        queue.batchFailed_ = 11;
        queue.batchTotal_ = 38;
        queue.retryFailed(0);
        QTRY_VERIFY(queue.preparation()["retryComplete"].toBool() ||
                    (!queue.preparation()["running"].toBool() &&
                     queue.preparation()["retryDone"].toInteger() > 0));
        QVERIFY2(queue.preparation()["retryComplete"].toBool(),
                 qPrintable(queue.preparation()["error"].toString()));
        QCOMPARE(queue.preparation()["retryDone"].toInteger(), 11);
        QCOMPARE(queue.preparation()["retryFailed"].toInteger(), 10);
        QCOMPARE(queue.preparation()["retryTotal"].toInteger(), 11);
        QCOMPARE(queue.preparation()["retryCursor"], queue.preparation()["retryUpper"]);
        QCOMPARE(queue.preparation()["done"].toInteger(), 27);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 11);
        QCOMPARE(queue.preparation()["total"].toInteger(), 38);
        QVERIFY(queue.preparation()["error"].toString().isEmpty());
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 10);
        QTRY_COMPARE(checkpoint(data.path())["retryDone"].toInteger(), 11);
        QCOMPARE(checkpoint(data.path())["retryFailed"].toInteger(), 10);
        WaveData wave;
        QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(tracks.last()), &wave));
    }
    void storageFullStillPausesPreparation_data() {
        QTest::addColumn<bool>("retry");
        QTest::newRow("full-library") << false;
        QTest::newRow("failed-retry") << true;
    }
    void storageFullStillPausesPreparation() {
        QFETCH(bool, retry);
        QTemporaryDir data;
        const auto tracks = seed(data.path(), {"full.wav", "next.wav"});
        QCOMPARE(tracks.size(), 2);
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QTRY_VERIFY(queue.preparation()["failuresReady"].toBool());
        queue.setWorkContext(true, false, false);
        queue.batchRunning_ = true;
        auto &entry = queue.entries_[WaveQueue::key(tracks[0])];
        entry.failed = true;
        entry.failureKind = "storage_full";
        entry.message = "本机缓存盘剩余空间不足 2 GiB，已停止新增缓存。";
        if (retry) {
            queue.retryMode_ = true;
            queue.retryPage_.append({1, tracks[0], "source_unavailable", "原件暂不可访问", false});
            queue.retryPage_.append({2, tracks[1], "source_unavailable", "原件暂不可访问", false});
            queue.advanceRetry(tracks[0], false, false);
            QCOMPARE(queue.preparation()["retryDone"].toInteger(), 1);
            QCOMPARE(queue.preparation()["retryFailed"].toInteger(), 1);
            QCOMPARE(queue.retryPage_.size(), 1);
        } else {
            queue.batchPage_.append({tracks[0], 0});
            queue.batchPage_.append({tracks[1], 0});
            queue.advanceBatch(tracks[0], false);
            QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
            QCOMPARE(queue.preparation()["cursor"].toInteger(), tracks[0].id);
            QCOMPARE(queue.batchPage_.size(), 1);
        }
        QVERIFY(!queue.preparation()["running"].toBool());
        QVERIFY(!queue.preparation()["complete"].toBool());
        QVERIFY(!queue.preparation()["error"].toString().isEmpty());
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 1);
        QTRY_VERIFY(!queue.failureWriting_ && !queue.checkpointDirty_);
        QVERIFY(!checkpoint(data.path())["running"].toBool());
    }
    void failedRetryCursorResumesAfterApplicationRestart() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 3; ++i) {
            auto name = QString("%1.wav").arg(i);
            names << name;
            QVERIFY(writeWav(data.filePath(name)));
        }
        const auto tracks = seed(data.path(), names);
        QCOMPARE(tracks.size(), 3);
        QVERIFY(failures(data.path(), tracks));
        qint64 firstCursor = 0;
        {
            WaveQueue queue(data.filePath("peaks"));
            queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
            QTRY_VERIFY(queue.preparation()["failuresReady"].toBool());
            connect(&queue, &WaveQueue::preparationChanged, this, [&] {
                if (queue.preparation()["retryDone"].toInteger() == 1 &&
                    queue.preparation()["running"].toBool())
                    queue.pausePreparation();
            });
            queue.retryFailed();
            QTRY_COMPARE(queue.preparation()["retryDone"].toInteger(), 1);
            firstCursor = queue.preparation()["retryCursor"].toInteger();
            QVERIFY(firstCursor > 0);
        }
        WaveQueue reopened(data.filePath("peaks"));
        reopened.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QTRY_VERIFY(reopened.preparation()["failuresReady"].toBool());
        QCOMPARE(reopened.preparation()["retryCursor"].toInteger(), firstCursor);
        QVERIFY(!reopened.preparation()["running"].toBool());
        QSet<qint64> requested;
        connect(&reopened, &WaveQueue::changed, this, [&](const Track &t) { requested.insert(t.id); });
        reopened.retryFailed();
        QTRY_VERIFY(reopened.preparation()["retryComplete"].toBool());
        QTRY_COMPARE(reopened.preparation()["knownFailures"].toInteger(), 0);
        QCOMPARE(reopened.preparation()["retryDone"].toInteger(), 3);
        QVERIFY(!requested.contains(tracks[0].id));
    }
    void failedLedgerWriteRetainsOutcomeAndDurableCursor_data() {
        QTest::addColumn<bool>("restart");
        QTest::newRow("resume") << false;
        QTest::newRow("restart") << true;
    }
    void failedLedgerWriteRetainsOutcomeAndDurableCursor() {
        QFETCH(bool, restart);
        QTemporaryDir data;
        const auto tracks = seed(data.path(), {"missing.wav"});
        QCOMPARE(tracks.size(), 1);
        auto queue = std::make_unique<WaveQueue>(data.filePath("peaks"));
        queue->configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QTRY_VERIFY(queue->preparation()["failuresReady"].toBool());
        queue->savePreparation();
        QTRY_VERIFY(!checkpoint(data.path()).isEmpty());
        QVERIFY(ledgerSql(data.path(), "CREATE TRIGGER reject_outcomes BEFORE INSERT ON failures "
                                       "BEGIN SELECT RAISE(FAIL, 'injected ledger write failure'); END"));
        queue->prepareAll();
        QTRY_VERIFY(queue->preparation()["error"].toString().contains("异常记录未能保存"));
        QVERIFY(!queue->preparation()["running"].toBool());
        queue->failurePool_.waitForDone();
        QCOMPARE(checkpoint(data.path())["cursor"].toInteger(), 0);
        QCOMPARE(checkpoint(data.path())["failed"].toInteger(), 0);
        if (restart)
            queue.reset(); // The failed write remains injected through shutdown.
        QVERIFY(ledgerSql(data.path(), "DROP TRIGGER reject_outcomes"));
        if (restart) {
            queue = std::make_unique<WaveQueue>(data.filePath("peaks"));
            queue->configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
            QTRY_VERIFY(queue->preparation()["failuresReady"].toBool());
            QCOMPARE(queue->preparation()["cursor"].toInteger(), 0);
        }
        queue->prepareAll();
        QTRY_VERIFY(queue->preparation()["complete"].toBool());
        QTRY_COMPARE(queue->preparation()["knownFailures"].toInteger(), 1);
        QTRY_COMPARE(checkpoint(data.path())["cursor"].toInteger(), tracks[0].id);
        FailureStore store(storePath(data.path()), data.filePath("library.sqlite"));
        QVERIFY(store.open(nullptr));
        QCOMPARE(store.page(0, 100, {}, false, nullptr)["items"].toArray().size(), 1);
    }
    void cachedAccurateViewsPublishWithoutOriginalAndReferencesRemainLocal() {
        QTemporaryDir data;
        const auto tracks = seed(data.path(), {"accurate.wav", "reference.wav"});
        QCOMPARE(tracks.size(), 2);
        PeakBuilder builder(48000, 1);
        float samples[256] = {.2f};
        builder.append(samples, 256);
        const auto wave = builder.finish();
        QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(tracks[0]), wave));
        QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(tracks[1]), wave, true));
        WaveQueue queue(data.filePath("peaks"));
        queue.setLibraries({{"qa", "测试", data.path(), data.filePath("shared"), "portable-test"}});
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.prepareAll();
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 2);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 0);
        QCOMPARE(queue.preparation()["reference"].toInteger(), 1);
        WaveData shared;
        QVERIFY(loadPortableWave(data.filePath("shared"), "portable-test", tracks[0].relativePath,
                                 tracks[0].size, tracks[0].modified, &shared));
        QVERIFY(!loadPortableWave(data.filePath("shared"), "portable-test", tracks[1].relativePath,
                                  tracks[1].size, tracks[1].modified, &shared));
        QCOMPARE(shared.frames, wave.frames);
    }
    void cachedViewCannotProveSharedPublicationOnCrashOrTimeout_data() {
        QTest::addColumn<bool>("timeout");
        QTest::newRow("crash") << false;
        QTest::newRow("timeout") << true;
    }
    void cachedViewCannotProveSharedPublicationOnCrashOrTimeout() {
        QFETCH(bool, timeout);
        QTemporaryDir data;
        const auto tracks = seed(data.path(), {"cached.wav"});
        QCOMPARE(tracks.size(), 1);
        PeakBuilder builder(48000, 1);
        float samples[256] = {.2f};
        builder.append(samples, 256);
        QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(tracks[0]), builder.finish()));
        const auto variable = timeout ? "AUDIO_TEST_SHARED_WAIT" : "AUDIO_TEST_WORKER_EXIT";
        qputenv(variable, "1");
        auto environment = qScopeGuard([variable] { qunsetenv(variable); });
        WaveQueue queue(data.filePath("peaks"));
        queue.setLibraries({{"qa", "测试", data.path(), data.filePath("shared"), "portable-test"}});
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.setWorkContext(true, false, false);
        queue.setVisible(tracks);
        QTRY_VERIFY(queue.find(tracks[0]) && queue.find(tracks[0])->stored);
        queue.prepareAll();
        queue.setWorkContext(false, false, false);
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 5000);
        QCOMPARE(queue.preparation()["done"].toInteger(), 0);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 1);
        QVERIFY(queue.find(tracks[0])->stored);
        QCOMPARE(queue.find(tracks[0])->failureKind,
                 QString(timeout ? "shared_publish_timeout" : "shared_cache_write"));
        QVERIFY(!queue.workerWatchdog_.isActive());
        qunsetenv(variable);
        queue.retryFailed();
        QTRY_VERIFY(queue.preparation()["retryComplete"].toBool());
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 0);
        QCOMPARE(queue.preparation()["retryDone"].toInteger(), 1);
        QCOMPARE(queue.preparation()["done"].toInteger(), 0);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
        WaveData shared;
        QVERIFY(loadPortableWave(data.filePath("shared"), "portable-test", tracks[0].relativePath,
                                 tracks[0].size, tracks[0].modified, &shared));
    }
    void repeatedWaitingEventsCannotExtendSharedWaitForever() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("waiting.wav")));
        QCOMPARE(seed(data.path(), {"waiting.wav"}).size(), 1);
        qputenv("AUDIO_TEST_SHARED_WAIT", "1");
        auto environment = qScopeGuard([] { qunsetenv("AUDIO_TEST_SHARED_WAIT"); });
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.prepareAll();
        QTRY_VERIFY(queue.preparation()["waiting"].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 5000);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 1);
        QVERIFY(!queue.workerWatchdog_.isActive());
    }
};
int main(int argc, char **argv) {
    if (argc > 2 && QByteArray(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        if (QByteArray(argv[2]) == "waves" && qEnvironmentVariableIsSet("AUDIO_TEST_PARALLEL_GATE")) {
            const QDir gate(qEnvironmentVariable("AUDIO_TEST_PARALLEL_GATE"));
            QFile input, output;
            input.open(stdin, QIODevice::ReadOnly);
            output.open(stdout, QIODevice::WriteOnly);
            auto send = [&](const QJsonObject &message) {
                output.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n");
                output.flush();
            };
            while (true) {
                const auto line = input.readLine();
                if (line.isEmpty())
                    return 0;
                auto config = QJsonDocument::fromJson(line).object();
                const auto name = QFileInfo(config["path"].toString()).fileName();
                config["pid"] = QCoreApplication::applicationPid();
                QSaveFile record(gate.filePath(name + ".json"));
                if (!record.open(QIODevice::WriteOnly))
                    return 1;
                record.write(QJsonDocument(config).toJson());
                if (!record.commit())
                    return 1;
                while (!QFileInfo::exists(gate.filePath(name + ".release")) &&
                       !QFileInfo::exists(gate.filePath("release-all")))
                    QThread::msleep(10);
                const bool success = !name.contains("fail");
                if (success) {
                    PeakBuilder builder(48000, 1);
                    const float samples[256] = {.2f};
                    builder.append(samples, 256);
                    const auto wave = builder.finish();
                    const auto key = waveKey(config["library"].toString(), config["relative"].toString(),
                                             config["indexedSize"].toInteger(), config["indexedModified"].toInteger());
                    if (!saveWaveView(config["local"].toString(), key, wave))
                        return 1;
                    send({{"event", "wave"}, {"complete", true}, {"stored", true},
                          {"data", QString::fromLatin1(encodeWave(wave).toBase64())}});
                } else {
                    send({{"event", "error"}, {"kind", "invalid_audio"}, {"message", "injected file failure"}});
                }
                send({{"event", "job-finished"}, {"success", success}});
            }
        }
        if (QByteArray(argv[2]) == "waves" && qEnvironmentVariableIsSet("AUDIO_TEST_REFERENCE_RESULT")) {
            QFile input;
            input.open(stdin, QIODevice::ReadOnly);
            const auto config = QJsonDocument::fromJson(input.readLine()).object();
            QFile output;
            output.open(stdout, QIODevice::WriteOnly);
            auto send = [&](const QJsonObject &message) {
                output.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n");
                output.flush();
            };
            PeakBuilder builder(48000, 1);
            QVector<float> samples(4800, .8f);
            builder.append(samples.data(), samples.size());
            const auto wave = builder.finish();
            const auto encoded = QString::fromLatin1(encodeWave(wave).toBase64());
            send({{"event", "wave"}, {"complete", false}, {"stored", false}, {"data", encoded}});
            QThread::msleep(600);
            const auto result = qgetenv("AUDIO_TEST_REFERENCE_RESULT");
            if (result != "partial-failure") {
                const auto version =
                    waveKey(config["library"].toString(), config["relative"].toString(),
                            config["indexedSize"].toInteger(), config["indexedModified"].toInteger());
                if (!saveWaveView(config["local"].toString(), version, wave, result == "reference"))
                    return 1;
                send({{"event", "wave"},
                      {"complete", true},
                      {"stored", true},
                      {"approximate", result == "reference"},
                      {"data", encoded}});
            }
            if (result != "reference")
                send({{"event", "error"},
                      {"kind", result == "shared-failure" ? "shared_cache_write" : "source_unavailable"},
                      {"message", "injected retry failure"}});
            send({{"event", "job-finished"}, {"success", result == "reference"}});
            return 0;
        }
        if (qEnvironmentVariableIsSet("AUDIO_TEST_WORKER_EXIT")) {
            QFile input;
            input.open(stdin, QIODevice::ReadOnly);
            input.readLine();
            return 1;
        }
        if (QByteArray(argv[2]) == "waves" && qEnvironmentVariableIsSet("AUDIO_TEST_SHARED_WAIT")) {
            QFile input;
            input.open(stdin, QIODevice::ReadOnly);
            input.readLine();
            QFile output;
            output.open(stdout, QIODevice::WriteOnly);
            for (int i = 0; i < 100; ++i) {
                output.write("{\"event\":\"waiting\",\"kind\":\"shared_wave\",\"limitMs\":0,\"message\":"
                             "\"test wait\"}\n");
                output.flush();
                QThread::msleep(100);
            }
            return 1;
        }
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    QueuePolicyTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "queue_policy_tests.moc"
