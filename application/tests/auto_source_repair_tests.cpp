#include "wave_queue.h"
#include "workers.h"
#if __has_include("source_repair.h")
#include "source_repair.h"
#else
struct SourceRepairReply { Track mapped; QString error; bool paused = false, needsConnection = false; };
using SourceRepairCompletion = std::function<void(SourceRepairReply)>;
using SourceRepairHandler = std::function<void(const Track &, SourceRepairCompletion)>;
#endif
#include <QApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QtTest>

// The fallback makes the pre-feature executable run and fail on missing behavior,
// instead of treating a missing header or symbol as the RED result.
template <typename Queue> static void installRepair(Queue &queue, SourceRepairHandler handler) {
    if constexpr (requires { queue.setSourceRepairHandler(handler); })
        queue.setSourceRepairHandler(std::move(handler));
}
template <typename Queue> static void resumeRepair(Queue &queue) {
    if constexpr (requires { queue.resumeAfterSourceRepair(); })
        queue.resumeAfterSourceRepair();
}
class AutoSourceRepairTests : public QObject {
    Q_OBJECT
    static bool wav(const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) return false;
        QDataStream out(&f); out.setByteOrder(QDataStream::LittleEndian);
        out.writeRawData("RIFF", 4); out << quint32(36 + 9600);
        out.writeRawData("WAVEfmt ", 8);
        out << quint32(16) << quint16(1) << quint16(1) << quint32(48000) << quint32(96000)
            << quint16(2) << quint16(16);
        out.writeRawData("data", 4); out << quint32(9600);
        for (int i = 0; i < 4800; ++i) out << qint16(i % 100 < 50 ? 4000 : -4000);
        return out.status() == QDataStream::Ok;
    }
    static QList<Track> seed(const QString &root, int count = 1) {
        Index index(QDir(root).filePath("library.sqlite"));
        if (!index.open() || !index.saveLibrary({"auto", "auto", root, {}})) return {};
        QList<Track> tracks;
        for (int i = 0; i < count; ++i) {
            Track track;
            track.libraryId = "auto";
            track.name = track.relativePath = QString("missing-%1.wav").arg(i, 2, 10, QChar('0'));
            track.folder = "."; track.size = 9644; track.modified = 1000;
            tracks << track;
        }
        if (!index.upsert(tracks)) return {};
        return index.tracksAfter(0);
    }
    static Track mapped(const QString &root, const Track &track) {
        auto result = track;
        result.compatiblePath = QDir(root).filePath("compatible.wav");
        if (!QFileInfo::exists(result.compatiblePath) && !wav(result.compatiblePath)) return {};
        QFile file(result.compatiblePath);
        if (!file.open(QIODevice::ReadOnly)) return {};
        result.compatibleSha256 = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
        result.compatibleSize = QFileInfo(file).size();
        result.compatibleModified = track.modified;
        Index index(QDir(root).filePath("library.sqlite"));
        if (!index.open() || !index.setCompatibleSource(track, result.compatiblePath, result.compatibleSha256))
            return {};
        return index.trackByIdentity(track.libraryId, track.relativePath).value_or(Track{});
    }
    static void configure(WaveQueue &queue, const QTemporaryDir &data) {
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
    }
  private slots:
    void terminalRetryFetchCannotResumePausedAllPreparation_data() {
        QTest::addColumn<bool>("reopen");
        QTest::newRow("explicit-resume") << false;
        QTest::newRow("reopen-then-resume") << true;
    }
    void terminalRetryFetchCannotResumePausedAllPreparation() {
        QFETCH(bool, reopen);
        QTemporaryDir data; const auto tracks = seed(data.path());
        auto queue = std::make_unique<WaveQueue>(data.filePath("peaks")); configure(*queue, data);
        QTRY_VERIFY(queue->failuresReady_ && !queue->failureWriting_);
        // This is the terminal ledger page of an automatic historical sweep.
        // Hold the real serial pool so Pause necessarily occurs before its callback.
        queue->batchRunning_ = queue->retryMode_ = queue->resumeAllAfterRetry_ = queue->allRetryChecked_ = true;
        queue->retryCursor_ = queue->retryUpper_ = queue->retryDone_ = queue->retryTotal_ = 1;
        queue->batchTotal_ = 1;
        QSemaphore entered, release;
        queue->failurePool_.start([&] { entered.release(); release.acquire(); });
        auto unblock = qScopeGuard([&] { release.release(); queue->failurePool_.waitForDone(); });
        QVERIFY(entered.tryAcquire(1, 3000));
        queue->fetchRetries(false);
        QVERIFY(queue->retryLoading_);
        queue->pausePreparation();
        QVERIFY(!queue->preparation()["running"].toBool());
        release.release(); unblock.dismiss();
        QTRY_VERIFY(!queue->retryLoading_);
        QVERIFY2(!queue->preparation()["running"].toBool(), "Terminal retry callback must retain the user's paused state");
        QCOMPARE(queue->preparation()["cursor"].toInteger(), 0);
        QCOMPARE(queue->preparation()["retryDone"].toInteger(), 1);
        QVERIFY(queue->preparation()["resumeAllAfterRetry"].toBool());
        if (reopen) {
            queue.reset();
            queue = std::make_unique<WaveQueue>(data.filePath("peaks")); configure(*queue, data);
            QVERIFY(!queue->preparation()["running"].toBool());
            QVERIFY(queue->preparation()["resumeAllAfterRetry"].toBool());
        }
        installRepair(*queue, [&](const Track &track, SourceRepairCompletion done) { done({mapped(data.path(), track)}); });
        queue->prepareAll(0);
        QTRY_VERIFY_WITH_TIMEOUT(queue->preparation()["complete"].toBool(), 5000);
        QCOMPARE(queue->preparation()["mode"].toString(), QString("all"));
        QCOMPARE(queue->preparation()["done"].toInteger(), 1);
        QCOMPARE(queue->preparation()["failed"].toInteger(), 0);
        QCOMPARE(queue->preparation()["retryDone"].toInteger(), 1);
        QCOMPARE(queue->preparation()["cursor"].toInteger(), tracks.first().id);
    }
    void prefixCompletionCannotResumePausedAllPreparation_data() {
        QTest::addColumn<bool>("reopen");
        QTest::newRow("explicit-resume") << false;
        QTest::newRow("reopen-then-resume") << true;
    }
    void prefixCompletionCannotResumePausedAllPreparation() {
        QFETCH(bool, reopen);
        QTemporaryDir data; const auto tracks = seed(data.path(), 2);
        PeakBuilder builder(48000, 1); const float samples[256] = {.2f}; builder.append(samples, 256);
        QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(tracks.first()), builder.finish()));
        auto queue = std::make_unique<WaveQueue>(data.filePath("peaks")); configure(*queue, data);
        QTRY_VERIFY(queue->failuresReady_);
        queue->batchCursor_ = tracks.first().id;
        queue->batchTotal_ = 2;
        queue->batchRunning_ = queue->allRetryChecked_ = true;
        QSemaphore entered, release;
        queue->localPool_.start([&] { entered.release(); release.acquire(); });
        auto unblock = qScopeGuard([&] { release.release(); queue->localPool_.waitForDone(); });
        QVERIFY(entered.tryAcquire(1, 3000));
        queue->continueAllAfterRetry();
        QVERIFY(queue->allPrefixReconciling_);
        queue->pausePreparation();
        release.release(); unblock.dismiss();
        QTRY_VERIFY(!queue->allPrefixReconciling_);
        QVERIFY(!queue->preparation()["running"].toBool());
        QCOMPARE(queue->preparation()["cursor"].toInteger(), tracks.first().id);
        QCOMPARE(queue->preparation()["done"].toInteger(), 1); // Local reconciliation remains valid while paused.
        if (reopen) {
            queue.reset();
            queue = std::make_unique<WaveQueue>(data.filePath("peaks")); configure(*queue, data);
            QVERIFY(!queue->preparation()["running"].toBool());
        }
        installRepair(*queue, [&](const Track &track, SourceRepairCompletion done) { done({mapped(data.path(), track)}); });
        queue->prepareAll(0);
        QTRY_VERIFY_WITH_TIMEOUT(queue->preparation()["complete"].toBool(), 5000);
        QCOMPARE(queue->preparation()["done"].toInteger(), 2);
        QCOMPARE(queue->preparation()["failed"].toInteger(), 0);
        QCOMPARE(queue->preparation()["cursor"].toInteger(), tracks.last().id);
    }
    void repairFailureIsCountedOnceAndContinuesPastEightFiles() {
        QTemporaryDir data;
        const auto tracks = seed(data.path(), 11);
        WaveQueue queue(data.filePath("peaks")); configure(queue, data);
        int repairs = 0;
        installRepair(queue, [&](const Track &, SourceRepairCompletion done) {
            ++repairs; done({{}, "synthetic unavailable"});
        });
        queue.prepareAll(0);
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 10000);
        QCOMPARE(repairs, 11);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 11);
        QCOMPARE(queue.preparation()["cursor"].toInteger(), tracks.last().id);
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 11);
    }
    void mappedSourceStillUnavailableGetsOnlyOneRepairAttempt() {
        QTemporaryDir data;
        const auto tracks = seed(data.path());
        WaveQueue queue(data.filePath("peaks")); configure(queue, data);
        int repairs = 0;
        installRepair(queue, [&](const Track &track, SourceRepairCompletion done) {
            ++repairs;
            auto replacement = track;
            replacement.compatiblePath = data.filePath("also-missing.wav");
            replacement.compatibleSha256 = QByteArray(64, '0');
            done({replacement});
        });
        queue.prepareAll(0);
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 5000);
        QCOMPARE(repairs, 1);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 1);
    }
    void authenticationPausesWholePoolAndResumesTheSameMode_data() {
        QTest::addColumn<int>("concurrency");
        QTest::addColumn<bool>("retry");
        QTest::newRow("serial-all") << 1 << false;
        QTest::newRow("parallel-all") << 4 << false;
        QTest::newRow("retry-failures") << 1 << true;
    }
    void authenticationPausesWholePoolAndResumesTheSameMode() {
        QFETCH(int, concurrency); QFETCH(bool, retry);
        QTemporaryDir data;
        const auto tracks = seed(data.path(), 4);
        if (retry) {
            FailureStore store(data.filePath("waveform-failures.sqlite"), data.filePath("library.sqlite"));
            QVERIFY(store.open(nullptr));
            QList<FailureOutcome> outcomes;
            for (const auto &track : tracks)
                outcomes.append({track, false, false, "source_unavailable", "offline", 0});
            QVERIFY(store.apply(outcomes, nullptr));
        }
        WaveQueue queue(data.filePath("peaks")); configure(queue, data);
        bool authenticated = false;
        QList<SourceRepairCompletion> pending;
        installRepair(queue, [&](const Track &track, SourceRepairCompletion done) {
            if (!authenticated) pending << std::move(done);
            else done({mapped(data.path(), track)});
        });
        QSignalSpy connection(&queue, SIGNAL(sourceRepairNeedsConnection(Track,QString)));
        queue.setPreparationConcurrency(concurrency);
        if (retry) queue.retryFailed(0); else queue.prepareAll(0);
        QTRY_COMPARE_WITH_TIMEOUT(pending.size(), concurrency, 5000);
        const auto before = queue.preparation();
        auto first = pending.takeFirst();
        first({{}, "connect once", true, true});
        QTRY_VERIFY(!queue.preparation()["running"].toBool());
        QCOMPARE(connection.count(), 1);
        QCOMPARE(queue.preparation()["cursor"], before["cursor"]);
        QCOMPARE(queue.preparation()["retryCursor"], before["retryCursor"]);
        QCOMPARE(queue.preparation()["mode"].toString(), retry ? QString("failed") : QString("all"));
        for (auto &done : pending) done({{}, "late connect", true, true});
        QCOMPARE(connection.count(), 1);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 0);
        QCOMPARE(queue.preparation()["retryFailed"].toInteger(), 0);
        authenticated = true; resumeRepair(queue);
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()[retry ? "retryComplete" : "complete"].toBool(), 10000);
        QCOMPARE(queue.preparation()[retry ? "retryDone" : "done"].toInteger(), 4);
        QCOMPARE(queue.preparation()["mode"].toString(), retry ? QString("failed") : QString("all"));
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 0);
    }
    void userPauseRejectsLateRepairAndKeepsOriginalCursor_data() {
        QTest::addColumn<int>("concurrency");
        QTest::addColumn<bool>("foreground");
        QTest::newRow("serial") << 1 << false;
        QTest::newRow("parallel") << 4 << false;
        QTest::newRow("foreground-selected") << 1 << true;
    }
    void userPauseRejectsLateRepairAndKeepsOriginalCursor() {
        QFETCH(int, concurrency); QFETCH(bool, foreground);
        QTemporaryDir data;
        const auto tracks = seed(data.path(), 4);
        WaveQueue queue(data.filePath("peaks")); configure(queue, data);
        QList<QPair<Track, SourceRepairCompletion>> pending;
        installRepair(queue, [&](const Track &track, SourceRepairCompletion done) { pending << qMakePair(track, done); });
        QSignalSpy jobs(&queue, &WaveQueue::jobFinished);
        queue.setPreparationConcurrency(concurrency);
        if (foreground) queue.select(tracks.first());
        queue.prepareAll(0);
        QTRY_COMPARE_WITH_TIMEOUT(pending.size(), concurrency, 5000);
        queue.pausePreparation();
        for (auto &request : pending) request.second({mapped(data.path(), request.first)});
        QTest::qWait(200);
        QCOMPARE(jobs.count(), 0);
        QCOMPARE(queue.preparation()["knownFailures"].toInteger(), 0);
        QCOMPARE(queue.preparation()["cursor"].toInteger(), 0);
        QCOMPARE(queue.preparation()["done"].toInteger(), 0);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 0);
        QVERIFY(!queue.preparation()["running"].toBool());
        installRepair(queue, [&](const Track &track, SourceRepairCompletion done) { done({mapped(data.path(), track)}); });
        queue.prepareAll(0);
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 10000);
        QCOMPARE(queue.preparation()["done"].toInteger(), 4);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 0);
    }
    void destroyedQueueRejectsLateRepair() {
        QTemporaryDir data; const auto tracks = seed(data.path());
        SourceRepairCompletion completion;
        auto *queue = new WaveQueue(data.filePath("peaks")); configure(*queue, data);
        installRepair(*queue, [&](const Track &, SourceRepairCompletion done) { completion = done; });
        queue->prepareAll(0);
        QTRY_VERIFY(bool(completion));
        delete queue;
        completion({mapped(data.path(), tracks.first())});
        WaveData wave;
        QVERIFY(!loadWaveView(data.filePath("peaks"), WaveQueue::key(tracks.first()), &wave));
    }
    void automaticFullPreparationSweepsOldFailuresThenResumesSavedCursor() {
        QTemporaryDir data; const auto tracks = seed(data.path(), 4);
        FailureStore store(data.filePath("waveform-failures.sqlite"), data.filePath("library.sqlite"));
        QVERIFY(store.open(nullptr));
        QList<FailureOutcome> outcomes;
        for (int i = 0; i < 2; ++i)
            outcomes.append({tracks[i], false, false, "source_unavailable", "previous run", 0});
        QVERIFY(store.apply(outcomes, nullptr));
        QFile state(data.filePath("progress.json"));
        QVERIFY(state.open(QIODevice::WriteOnly));
        state.write(QJsonDocument(QJsonObject{{"schema", 2}, {"cursor", tracks[1].id}, {"done", 0},
                                              {"failed", 2}, {"total", 4}}).toJson());
        state.close();
        WaveQueue queue(data.filePath("peaks")); configure(queue, data);
        QList<qint64> repaired;
        installRepair(queue, [&](const Track &track, SourceRepairCompletion done) {
            repaired << track.id; done({mapped(data.path(), track)});
        });
        queue.prepareAll(0);
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 10000);
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 0);
        QCOMPARE(repaired, QList<qint64>({tracks[0].id, tracks[1].id, tracks[2].id, tracks[3].id}));
        QCOMPARE(queue.preparation()["cursor"].toInteger(), tracks.last().id);
        QCOMPARE(queue.preparation()["done"].toInteger(), 4);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 2); // Historical all sweep counters.
        QCOMPARE(queue.preparation()["mode"].toString(), QString("all"));
    }
    void interruptedPrefixReconciliationCountsReferenceViewsAfterRestart() {
        QTemporaryDir data; const auto tracks = seed(data.path(), 3);
        PeakBuilder builder(48000, 1); const float samples[256] = {.2f}; builder.append(samples, 256);
        QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(tracks[0]), builder.finish(), true));
        QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(tracks[1]), builder.finish(), false));
        QFile state(data.filePath("progress.json")); QVERIFY(state.open(QIODevice::WriteOnly));
        state.write(QJsonDocument(QJsonObject{{"schema", 2}, {"cursor", tracks[1].id}, {"done", 0},
            {"failed", 2}, {"total", 3}, {"allRetryChecked", true}, {"reconcilingCachedPrefix", true}}).toJson());
        state.close();
        WaveQueue queue(data.filePath("peaks")); configure(queue, data);
        installRepair(queue, [&](const Track &track, SourceRepairCompletion done) { done({mapped(data.path(), track)}); });
        queue.prepareAll(0);
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 5000);
        QCOMPARE(queue.preparation()["done"].toInteger(), 3);
        QCOMPARE(queue.preparation()["reference"].toInteger(), 1);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 2);
    }
    void boundedHistoricalRetryContinuesAllWithoutLoopingUnrepairableFiles() {
        QTemporaryDir data; const auto tracks = seed(data.path(), 3);
        FailureStore store(data.filePath("waveform-failures.sqlite"), data.filePath("library.sqlite"));
        QVERIFY(store.open(nullptr));
        QVERIFY(store.apply({{tracks[0], false, false, "source_unavailable", "old", 0},
                             {tracks[1], false, false, "source_unavailable", "old", 0}}, nullptr));
        QFile state(data.filePath("progress.json")); QVERIFY(state.open(QIODevice::WriteOnly));
        state.write(QJsonDocument(QJsonObject{{"schema", 2}, {"cursor", tracks[1].id}, {"failed", 2}, {"total", 3}}).toJson());
        state.close();
        WaveQueue queue(data.filePath("peaks")); configure(queue, data);
        QList<qint64> attempts;
        installRepair(queue, [&](const Track &track, SourceRepairCompletion done) {
            attempts << track.id;
            if (track.id <= tracks[1].id) done({{}, "ordinary unrecoverable file"});
            else done({mapped(data.path(), track)});
        });
        queue.prepareAll(0);
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 5000);
        QCOMPARE(attempts, QList<qint64>({tracks[0].id, tracks[1].id, tracks[2].id}));
        QCOMPARE(queue.preparation()["done"].toInteger(), 1);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 2);
        QCOMPARE(queue.preparation()["retryDone"].toInteger(), 2);
        QCOMPARE(queue.preparation()["retryFailed"].toInteger(), 2);
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 2);
    }
    void restartedAllRunRejectsForegroundRepairCompletionFromPreviousRun() {
        QTemporaryDir data; const auto tracks = seed(data.path());
        WaveQueue queue(data.filePath("peaks")); configure(queue, data);
        QList<SourceRepairCompletion> pending;
        installRepair(queue, [&](const Track &, SourceRepairCompletion done) { pending << done; });
        QSignalSpy repaired(&queue, SIGNAL(sourceRepaired(Track)));
        queue.select(tracks.first()); queue.prepareAll(0);
        QTRY_COMPARE(pending.size(), 1);
        queue.prepareAll(0, true);
        QTRY_COMPARE_WITH_TIMEOUT(pending.size(), 2, 2000);
        pending[0]({mapped(data.path(), tracks.first())});
        QTest::qWait(60);
        QCOMPARE(repaired.count(), 0);
        QCOMPARE(queue.preparation()["cursor"].toInteger(), 0);
        pending[1]({mapped(data.path(), tracks.first())});
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 5000);
        QCOMPARE(repaired.count(), 1);
        QCOMPARE(queue.preparation()["done"].toInteger(), 1);
    }
    void capturedWorkerConfigurationPreservesPersistentRetryAndContainsNoCredentials() {
        QTemporaryDir data; const auto tracks = seed(data.path(), 4);
        qputenv("AUTO_SOURCE_REPAIR_CAPTURE", data.path().toUtf8());
        auto cleanup = qScopeGuard([] { qunsetenv("AUTO_SOURCE_REPAIR_CAPTURE"); });
        WaveQueue queue(data.filePath("peaks")); configure(queue, data);
        installRepair(queue, [&](const Track &track, SourceRepairCompletion done) { done({mapped(data.path(), track)}); });
        QSignalSpy repaired(&queue, SIGNAL(sourceRepaired(Track)));
        queue.setPreparationConcurrency(4); queue.prepareAll(0);
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 10000);
        QCOMPARE(repaired.count(), 4);
        for (const auto &track : tracks) {
            QFile record(data.filePath(track.relativePath + ".jsonl"));
            QVERIFY(record.open(QIODevice::ReadOnly));
            const auto first = QJsonDocument::fromJson(record.readLine()).object();
            const auto second = QJsonDocument::fromJson(record.readLine()).object();
            QVERIFY(record.readLine().isEmpty());
            QVERIFY(first["compatiblePath"].toString().isEmpty());
            QVERIFY(!second["compatiblePath"].toString().isEmpty());
            QCOMPARE(first["pid"], second["pid"]);
            QCOMPARE(first["verificationCache"].toString(), data.filePath("media-verification.sqlite"));
            QCOMPARE(second["verificationCache"], first["verificationCache"]);
            QCOMPARE(second["relative"].toString(), track.relativePath);
            QCOMPARE(second["background"].toBool(), true);
            QCOMPARE(second["readBytesPerSecond"].toInteger(), 0);
            for (const auto &field : first.keys())
                QVERIFY2(!QStringList({"username", "password", "credentials", "session", "token"}).contains(field.toLower()), qPrintable(field));
        }
    }
    void sourceFailureRepairsSameJobBeforeCounting_data() {
        QTest::addColumn<int>("concurrency");
        QTest::newRow("serial") << 1;
        QTest::newRow("four-workers") << 4;
    }
    void sourceFailureRepairsSameJobBeforeCounting() {
        QFETCH(int, concurrency);
        QTemporaryDir data;
        const auto tracks = seed(data.path(), concurrency == 4 ? 6 : 1);
        QCOMPARE(tracks.size(), concurrency == 4 ? 6 : 1);
        WaveQueue queue(data.filePath("peaks")); configure(queue, data);
        int repairs = 0;
        installRepair(queue, [&](const Track &track, SourceRepairCompletion done) {
            ++repairs;
            QCOMPARE(queue.preparation()["failed"].toInteger(), 0);
            const auto replacement = mapped(data.path(), track);
            QVERIFY(replacement.id > 0);
            QTimer::singleShot(5, &queue, [done, replacement] { done({replacement}); });
        });
        QSignalSpy jobs(&queue, &WaveQueue::jobFinished);
        queue.setPreparationConcurrency(concurrency); queue.prepareAll(0);
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 10000);
        QCOMPARE(queue.preparation()["done"].toInteger(), tracks.size());
        QCOMPARE(queue.preparation()["failed"].toInteger(), 0);
        QCOMPARE(repairs, tracks.size());
        for (const auto &track : tracks) {
            const auto *entry = queue.find(track);
            QVERIFY(entry && entry->complete && entry->stored && !entry->failed);
            WaveData wave;
            QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(track), &wave));
            QCOMPARE(wave.frames, 4800);
        }
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 0);
        for (const auto &job : jobs) QVERIFY(job[1].toBool());
    }
};
int main(int argc, char **argv) {
    if (argc > 2 && QByteArray(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        if (qEnvironmentVariableIsSet("AUTO_SOURCE_REPAIR_CAPTURE") && QByteArray(argv[2]) == "waves") {
            const QDir directory(qEnvironmentVariable("AUTO_SOURCE_REPAIR_CAPTURE"));
            QFile input, output;
            if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly)) return 2;
            auto send = [&](const QJsonObject &message) {
                output.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n"); output.flush();
            };
            while (true) {
                const auto line = input.readLine();
                if (line.isEmpty()) return 0;
                auto config = QJsonDocument::fromJson(line).object();
                config["pid"] = QCoreApplication::applicationPid();
                QFile record(directory.filePath(config["relative"].toString() + ".jsonl"));
                if (!record.open(QIODevice::Append)) return 2;
                record.write(QJsonDocument(config).toJson(QJsonDocument::Compact) + "\n"); record.close();
                const bool success = !config["compatiblePath"].toString().isEmpty();
                if (success) {
                    PeakBuilder builder(48000, 1); QVector<float> samples(4800, .2f);
                    builder.append(samples.data(), samples.size()); const auto wave = builder.finish();
                    if (!saveWaveView(config["local"].toString(), waveKey(config["library"].toString(),
                        config["relative"].toString(), config["indexedSize"].toInteger(), config["indexedModified"].toInteger()), wave)) return 2;
                    send({{"event", "wave"}, {"complete", true}, {"stored", true},
                        {"data", QString::fromLatin1(encodeWave(wave).toBase64())}});
                } else send({{"event", "error"}, {"kind", "source_unavailable"}, {"message", "synthetic source unavailable"}});
                send({{"event", "job-finished"}, {"success", success}});
            }
        }
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    AutoSourceRepairTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "auto_source_repair_tests.moc"
