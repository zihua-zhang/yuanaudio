#include "window.h"
#include "workers.h"
#include <QApplication>
#include <QDataStream>
#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

class QueueRefreshTests : public QObject {
    Q_OBJECT

    static bool writeWav(const QString &path, int frames = 4800) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        QDataStream out(&file);
        out.setByteOrder(QDataStream::LittleEndian);
        out.writeRawData("RIFF", 4);
        out << quint32(36 + frames * 2);
        out.writeRawData("WAVEfmt ", 8);
        out << quint32(16) << quint16(1) << quint16(1);
        out << quint32(48000) << quint32(96000) << quint16(2) << quint16(16);
        out.writeRawData("data", 4);
        out << quint32(frames * 2);
        for (int i = 0; i < frames; ++i)
            out << qint16(i % 200 < 100 ? 4000 : -4000);
        return out.status() == QDataStream::Ok;
    }
    static Track seed(const QString &data, const QString &name = "cached.wav") {
        const auto music = QDir(data).filePath("music");
        QDir().mkpath(music);
        const auto source = QDir(music).filePath(name);
        if (!QFile::exists(source) && !writeWav(source))
            return {};
        Index db(QDir(data).filePath("library.sqlite"));
        if (!db.open() || !db.saveLibrary({"qa", "测试", music, ""}))
            return {};
        const QFileInfo info(source);
        Track track;
        track.libraryId = "qa";
        track.relativePath = track.name = name;
        track.folder = ".";
        track.size = info.size();
        track.modified = info.lastModified().toMSecsSinceEpoch();
        if (!db.upsert({track}))
            return {};
        return db.search(name).tracks.value(0);
    }
    static bool cacheTrack(const QString &data, const Track &track) {
        PeakBuilder builder(48000, 1);
        const float samples[256] = {0.5f};
        builder.append(samples, 256);
        return saveWaveView(QDir(data).filePath("peaks"), WaveQueue::key(track), builder.finish());
    }
    static void addPausedHistory(WaveQueue *queue) {
        queue->batchCursor_ = 17;
        queue->batchDone_ = 12;
        queue->batchFailed_ = 2;
        queue->batchTotal_ = 40;
        queue->preparationError_ = "请检查网络后继续";
        queue->recentFailures_ = QJsonArray{QJsonObject{{"name", "missing.wav"}, {"kind", "timeout"}}};
        queue->savePreparation();
    }

  private slots:
    void failedScanPreservesViewsAndPreparationHistory() {
        QTemporaryDir data;
        const auto track = seed(data.path());
        QVERIFY(track.id);
        QVERIFY(cacheTrack(data.path(), track));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && window.waveQueue_->find(track));
        addPausedHistory(window.waveQueue_);
        const auto before = window.waveQueue_->preparation();
        auto lib = window.libraries_.first();
        lib.root = data.filePath("disconnected");
        window.beginScan(lib);
        QTRY_VERIFY(!window.scan_);
        QTest::qWait(100);
        WaveData wave;
        QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(track), &wave));
        const auto after = window.waveQueue_->preparation();
        for (const auto &field : {"cursor", "done", "failed", "total", "failures", "error"})
            QCOMPARE(after[field], before[field]);
        QVERIFY(!after["catalogNeedsReview"].toBool());
    }
    void cancelledScanPreservesViewsAndPreparationHistory() {
        QTemporaryDir data;
        const auto track = seed(data.path());
        QVERIFY(track.id);
        QVERIFY(cacheTrack(data.path(), track));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && window.waveQueue_->find(track));
        addPausedHistory(window.waveQueue_);
        const auto before = window.waveQueue_->preparation();
        qputenv("AUDIO_TEST_STALL_SCAN", "1");
        auto environment = qScopeGuard([] { qunsetenv("AUDIO_TEST_STALL_SCAN"); });
        window.beginScan(window.libraries_.first());
        QTRY_VERIFY(window.scan_ && window.scan_->state() == QProcess::Running);
        window.scan_->kill();
        QTRY_VERIFY(!window.scan_);
        QTest::qWait(100);
        WaveData wave;
        QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(track), &wave));
        const auto after = window.waveQueue_->preparation();
        for (const auto &field : {"cursor", "done", "failed", "total", "failures", "error"})
            QCOMPARE(after[field], before[field]);
    }
    void successfulRefreshPreservesCacheAndRequiresExplicitCatalogReview() {
        QTemporaryDir data;
        const auto track = seed(data.path());
        QVERIFY(track.id);
        QVERIFY(cacheTrack(data.path(), track));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && window.waveQueue_->find(track));
        addPausedHistory(window.waveQueue_);
        window.beginScan(window.libraries_.first());
        QTRY_VERIFY(!window.scan_);
        QTRY_VERIFY(window.waveQueue_->preparation()["catalogNeedsReview"].toBool());
        const auto state = window.waveQueue_->preparation();
        QVERIFY(!state["running"].toBool());
        QVERIFY(!state["complete"].toBool());
        QCOMPARE(state["done"].toInteger(), 12);
        QCOMPARE(state["failed"].toInteger(), 2);
        WaveData wave;
        QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(track), &wave));
        QVERIFY(!QFile::exists(data.filePath("peaks/views-v1/epoch")));
    }
    void reviewedCatalogSurvivesRestartAndSkipsUnchangedOfflineViews() {
        QTemporaryDir data;
        const auto track = seed(data.path());
        QVERIFY(track.id);
        QVERIFY(cacheTrack(data.path(), track));
        const auto database = data.filePath("library.sqlite");
        const auto stateFile = data.filePath("preparation.json");
        // Full-library reconciliation must not reopen old-but-valid cached originals.
        const auto key = WaveQueue::key(track);
        QFile view(data.filePath("peaks/views-v1/" + key.left(2) + "/" + key + ".view"));
        QVERIFY(view.open(QIODevice::ReadWrite));
        QVERIFY(view.seek(4));
        QDataStream stamp(&view);
        stamp << qint64(QDateTime::currentMSecsSinceEpoch() - 25LL * 3600000);
        view.close();
        {
            WaveQueue queue(data.filePath("peaks"));
            queue.configurePreparation(database, stateFile);
            addPausedHistory(&queue);
            queue.refreshIndex();
        }
        QVERIFY(QFile::remove(track.absolutePath()));
        WaveQueue reopened(data.filePath("peaks"));
        reopened.configurePreparation(database, stateFile);
        QVERIFY(reopened.preparation()["catalogNeedsReview"].toBool());
        QCOMPARE(reopened.preparation()["done"].toInteger(), 12);
        reopened.prepareAll();
        QTRY_VERIFY(reopened.preparation()["complete"].toBool());
        QVERIFY(!reopened.preparation()["catalogNeedsReview"].toBool());
        QCOMPARE(reopened.preparation()["done"].toInteger(), 1);
        QCOMPARE(reopened.preparation()["failed"].toInteger(), 0);
        QVERIFY(reopened.findChildren<QProcess *>().isEmpty());
    }
    void refreshDiscardsQueuedOldVersionsAndPreparesOnlyChangedTrack() {
        QTemporaryDir data;
        const auto unchanged = seed(data.path(), "unchanged.wav");
        const auto original = seed(data.path(), "changed.wav");
        QVERIFY(unchanged.id && original.id);
        QVERIFY(cacheTrack(data.path(), unchanged));
        QVERIFY(cacheTrack(data.path(), original));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("preparation.json"));
        queue.batchPage_ = {{unchanged, 1}, {original, 1}};
        queue.batchRunning_ = true;
        QVERIFY(writeWav(original.absolutePath(), 9600));
        const auto changed = seed(data.path(), "changed.wav");
        QVERIFY(WaveQueue::key(original) != WaveQueue::key(changed));
        queue.refreshIndex();
        QVERIFY(queue.batchPage_.isEmpty());
        QVERIFY(!queue.preparation()["running"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 0);
        QVERIFY(QFile::remove(unchanged.absolutePath()));
        queue.prepareAll();
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 2);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 0);
        WaveData wave;
        QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(changed), &wave));
        QCOMPARE(wave.frames, 9600);
    }
    void partialScanChangesRetainHistoryButRequireReview_data() {
        QTest::addColumn<QByteArray>("mode");
        QTest::newRow("cancel-after-commit") << QByteArray("cancel");
        QTest::newRow("failure-after-commit") << QByteArray("failure");
    }
    void partialScanChangesRetainHistoryButRequireReview() {
        QFETCH(QByteArray, mode);
        QTemporaryDir data;
        const auto original = seed(data.path());
        QVERIFY(original.id);
        QVERIFY(cacheTrack(data.path(), original));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && window.waveQueue_->find(original));
        addPausedHistory(window.waveQueue_);
        window.waveQueue_->batchComplete_ = true;
        window.waveQueue_->batchPage_ = {{original, 1}};
        const auto before = window.waveQueue_->preparation();
        QVERIFY(writeWav(original.absolutePath(), 9600));
        qputenv("AUDIO_TEST_PARTIAL_SCAN", mode);
        auto environment = qScopeGuard([] { qunsetenv("AUDIO_TEST_PARTIAL_SCAN"); });
        window.beginScan(window.libraries_.first());
        QTRY_VERIFY(window.status_->text().contains("已检查 1 首") || !window.scan_);
        if (mode == "cancel") {
            QVERIFY(window.scan_);
            window.scan_->kill();
        }
        QTRY_VERIFY(!window.scan_);
        const auto after = window.waveQueue_->preparation();
        QVERIFY(after["catalogNeedsReview"].toBool());
        QVERIFY(!after["running"].toBool());
        QVERIFY(!after["complete"].toBool());
        QVERIFY(window.waveQueue_->batchPage_.isEmpty());
        for (const auto &field : {"cursor", "done", "failed", "total", "failures", "error"})
            QCOMPARE(after[field], before[field]);
        WaveData wave;
        QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(original), &wave));
        auto *toggle = window.findChild<QPushButton *>("waveCacheToggle");
        QVERIFY(toggle);
        // The footer prioritizes the retained error; the review action now lives in settings.
        QCOMPARE(toggle->text(), QString("缓存需处理"));
        QVERIFY(toggle->toolTip().contains(after["error"].toString()));
        QVERIFY(!window.settingsDialog_->isVisible());
        toggle->click();
        QVERIFY(window.settingsDialog_->isVisible());
        auto *review = window.findChild<QPushButton *>("prepareAllWaves");
        QVERIFY(review && review->isVisible());
        QVERIFY(review->text().contains("检查"));
    }
    void scannerReportsOnlyActualCommittedFileChanges() {
        QTemporaryDir data;
        const auto track = seed(data.path());
        QVERIFY(track.id);
        auto scan = [&]() {
            QProcess process;
            process.start(QCoreApplication::applicationFilePath(), {"--worker", "scan"});
            if (!process.waitForStarted(3000))
                return QByteArray();
            process.write(QJsonDocument(QJsonObject{{"root", track.root},
                                                    {"library", track.libraryId},
                                                    {"database", data.filePath("library.sqlite")}})
                              .toJson(QJsonDocument::Compact) +
                          "\n");
            process.closeWriteChannel();
            if (!process.waitForFinished(5000)) {
                process.kill();
                process.waitForFinished(1000);
                return QByteArray();
            }
            return process.readAllStandardOutput();
        };
        auto committedChange = [](const QByteArray &messages) {
            bool found = false;
            for (const auto &line : messages.split('\n')) {
                const auto message = QJsonDocument::fromJson(line).object();
                if (message["event"] == "progress") {
                    found = true;
                    if (!message.contains("filesChanged"))
                        return -1;
                    if (message["filesChanged"].toBool())
                        return 1;
                }
            }
            return found ? 0 : -1;
        };
        QCOMPARE(committedChange(scan()), 0);
        {
            Index db(data.filePath("library.sqlite"));
            QVERIFY(db.open());
            QVERIFY(db.setPersonal(track.id, true, "个人标记"));
            db.setDuration(track, 100);
        }
        QCOMPARE(committedChange(scan()), 0);
        QVERIFY(writeWav(track.absolutePath(), 9600));
        QCOMPARE(committedChange(scan()), 1);
        QCOMPARE(committedChange(scan()), 0);
        QVERIFY(writeWav(QDir(track.root).filePath("new.wav")));
        QCOMPARE(committedChange(scan()), 1);
    }
    void preparationResumedDuringPartialScanRequiresReviewAfterCancellation() {
        QTemporaryDir data;
        const auto original = seed(data.path());
        QVERIFY(original.id);
        QVERIFY(cacheTrack(data.path(), original));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && window.waveQueue_->find(original));
        QVERIFY(writeWav(original.absolutePath(), 9600));
        qputenv("AUDIO_TEST_PARTIAL_SCAN", "cancel");
        auto environment = qScopeGuard([] { qunsetenv("AUDIO_TEST_PARTIAL_SCAN"); });
        window.beginScan(window.libraries_.first());
        QTRY_VERIFY(window.waveQueue_->preparation()["catalogNeedsReview"].toBool());
        QVERIFY(window.scan_);
        window.waveQueue_->prepareAll();
        QVERIFY(!window.waveQueue_->preparation()["catalogNeedsReview"].toBool());
        window.scan_->kill();
        QTRY_VERIFY(!window.scan_);
        QVERIFY(window.waveQueue_->preparation()["catalogNeedsReview"].toBool());
        QVERIFY(!window.waveQueue_->preparation()["running"].toBool());
        QVERIFY(window.waveQueue_->batchPage_.isEmpty());
        WaveData wave;
        QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(original), &wave));
    }
    void upsertChangeSignalDoesNotReportRolledBackOrUnchangedRows() {
        QTemporaryDir data;
        const auto original = seed(data.path());
        QVERIFY(original.id);
        Index db(data.filePath("library.sqlite"));
        QVERIFY(db.open());
        auto changed = original;
        ++changed.size;
        auto invalid = original;
        invalid.relativePath = "../invalid.wav";
        QString error;
        bool filesChanged = true;
        QVERIFY(!db.upsert({changed, invalid}, &error, &filesChanged));
        QVERIFY(!filesChanged);
        QCOMPARE(db.search(original.name).tracks[0].size, original.size);
        QVERIFY(db.upsert({changed}, &error, &filesChanged));
        QVERIFY(filesChanged);
        QVERIFY(db.upsert({changed}, &error, &filesChanged));
        QVERIFY(!filesChanged);
    }
    void refreshVerificationSurvivesRestartWithoutDiscardingDisplayedCache() {
        QTemporaryDir data;
        const auto track = seed(data.path());
        QVERIFY(track.id);
        QVERIFY(cacheTrack(data.path(), track));
        const auto database = data.filePath("library.sqlite");
        const auto stateFile = data.filePath("preparation.json");
        {
            WaveQueue queue(data.filePath("peaks"));
            queue.configurePreparation(database, stateFile);
            queue.refreshIndex();
        }
        WaveQueue reopened(data.filePath("peaks"));
        reopened.configurePreparation(database, stateFile);
        bool previousCacheDisplayed = false;
        connect(&reopened, &WaveQueue::changed, this, [&](const Track &) {
            const auto *entry = reopened.find(track);
            if (entry && entry->complete && entry->data.frames == 256)
                previousCacheDisplayed = true;
        });
        reopened.setVisible({track});
        QTRY_VERIFY(reopened.find(track) && reopened.find(track)->complete &&
                    reopened.find(track)->data.frames == 4800);
        QVERIFY(previousCacheDisplayed);
    }
};

int main(int argc, char **argv) {
    if (argc > 2 && QByteArray(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        if (QByteArray(argv[2]) == "scan" && qEnvironmentVariableIsSet("AUDIO_TEST_STALL_SCAN")) {
            QThread::sleep(30);
            return 1;
        }
        if (QByteArray(argv[2]) == "scan" && qEnvironmentVariableIsSet("AUDIO_TEST_PARTIAL_SCAN")) {
            // A real committed update followed by deterministic interruption. The scanner's
            // actual progress payload is covered separately above, without timing a kill race.
            QFile input;
            input.open(stdin, QIODevice::ReadOnly);
            const auto config = QJsonDocument::fromJson(input.readLine()).object();
            Index db(config["database"].toString());
            if (!db.open())
                return 2;
            Track track;
            track.libraryId = config["library"].toString();
            track.relativePath = track.name = "cached.wav";
            track.folder = ".";
            const QFileInfo source(QDir(config["root"].toString()).filePath(track.relativePath));
            track.size = source.size();
            track.modified = source.lastModified().toMSecsSinceEpoch();
            if (!db.upsert({track}))
                return 2;
            QFile output;
            output.open(stdout, QIODevice::WriteOnly);
            output.write("{\"event\":\"progress\",\"count\":1,\"folder\":\".\",\"filesChanged\":true}\n");
            output.flush();
            if (qgetenv("AUDIO_TEST_PARTIAL_SCAN") == "cancel")
                QThread::sleep(30);
            else {
                output.write("{\"event\":\"error\",\"message\":\"simulated interrupted scan\"}\n");
                output.flush();
            }
            return 1;
        }
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    QueueRefreshTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "queue_refresh_tests.moc"
