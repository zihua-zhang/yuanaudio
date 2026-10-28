#include "window.h"
#include "workers.h"
#include <QApplication>
#include <QCheckBox>
#include <QDataStream>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHelpEvent>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSemaphore>
#include <QSlider>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QThread>
#include <QToolTip>
#include <QTreeView>
#include <QtTest>
#include <cmath>

class DragCaptureTable : public TrackTable {
  public:
    QList<QUrl> draggedUrls;
    int drags = 0;

  protected:
    void executeFileDrag(QMimeData *mime, const QString &) override {
        draggedUrls = mime->urls();
        ++drags;
        delete mime;
    }
};

class WindowTests : public QObject {
    Q_OBJECT
    static bool writeWav(const QString &path, int frames = 96000) {
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
            out << qint16(4000 * std::sin(i * 2 * 3.14159265358979 * 440 / 48000));
        return out.status() == QDataStream::Ok;
    }
    static bool seed(const QString &root, const QStringList &names, const QString &sourceRoot = {}) {
        Index db(QDir(root).filePath("library.sqlite"));
        const auto music = sourceRoot.isEmpty() ? root : sourceRoot;
        if (!db.open() || !db.saveLibrary({"qa", "测试", music, ""}))
            return false;
        QList<Track> tracks;
        for (const auto &name : names) {
            Track t;
            t.libraryId = "qa";
            t.name = QFileInfo(name).fileName();
            t.relativePath = name;
            t.folder = QFileInfo(name).path();
            const QFileInfo info(QDir(music).filePath(name));
            t.size = info.exists() ? info.size() : 192044;
            t.modified = info.exists() ? info.lastModified().toMSecsSinceEpoch() : 1000;
            tracks << t;
        }
        return db.upsert(tracks);
    }
  private slots:
    void waveDragExportsPressedRowWithoutCueingOrChangingSelection() {
        TrackModel model;
        Track a, b;
        a.id = 1;
        a.name = "first.wav";
        a.relativePath = a.name;
        a.root = "//nas/music";
        b.id = 2;
        b.name = "中文 配乐.wav";
        b.relativePath = "剧情/" + b.name;
        b.root = a.root;
        model.replace({a, b});
        DragCaptureTable table;
        table.setModel(&model);
        table.resize(950, 260);
        table.setColumnWidth(TrackModel::Wave, 400);
        table.setCurrentIndex(model.index(0, TrackModel::Name));
        table.show();
        QTest::qWait(30);
        QSignalSpy cues(&table, &TrackTable::waveClicked);
        QSignalSpy changes(table.selectionModel(), &QItemSelectionModel::currentRowChanged);
        const auto point = table.visualRect(model.index(1, TrackModel::Wave)).center();
        QTest::mousePress(table.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        const auto moved = point + QPoint(QApplication::startDragDistance() + 15, 0);
        QMouseEvent move(QEvent::MouseMove, moved, table.viewport()->mapToGlobal(moved), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(table.viewport(), &move);
        QApplication::sendEvent(table.viewport(),
                                &move); // Cancelled/finished drag cannot restart the gesture.
        QTest::mouseRelease(table.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(table.drags, 1);
        QCOMPARE(table.draggedUrls, QList<QUrl>{QUrl::fromLocalFile(b.absolutePath())});
        QCOMPARE(cues.size(), 0);
        QCOMPARE(changes.size(), 0);
        QTest::mouseClick(table.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(cues.size(), 1);
        QCOMPARE(cues[0][0].toInt(), 1);
    }
    void waveSmallMotionClicksAndResetCancelsPendingGesture() {
        TrackModel model;
        Track a;
        a.id = 1;
        a.name = "music.wav";
        a.relativePath = a.name;
        a.root = "C:/music";
        model.replace({a});
        DragCaptureTable table;
        table.setModel(&model);
        table.resize(900, 240);
        table.show();
        QTest::qWait(30);
        QSignalSpy cues(&table, &TrackTable::waveClicked);
        const auto point = table.visualRect(model.index(0, TrackModel::Wave)).center();
        QTest::mousePress(table.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QMouseEvent small(QEvent::MouseMove, point + QPoint(1, 0),
                          table.viewport()->mapToGlobal(point + QPoint(1, 0)), Qt::NoButton, Qt::LeftButton,
                          Qt::NoModifier);
        QApplication::sendEvent(table.viewport(), &small);
        QTest::mouseRelease(table.viewport(), Qt::LeftButton, Qt::NoModifier, point + QPoint(1, 0));
        QCOMPARE(cues.size(), 1);
        QCOMPARE(table.drags, 0);
        QTest::mousePress(table.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        model.replace({a});
        const auto moved = point + QPoint(40, 0);
        QMouseEvent move(QEvent::MouseMove, moved, table.viewport()->mapToGlobal(moved), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(table.viewport(), &move);
        QTest::mouseRelease(table.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QCOMPARE(cues.size(), 1);
        QCOMPARE(table.drags, 0);
    }
    void unavailableSourcesStillPauseInsteadOfFailingTheWholeLibrary() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 10; ++i)
            names << QString("missing-%1.wav").arg(i);
        QVERIFY(seed(data.path(), names));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.prepareAll();
        QTRY_VERIFY(!queue.preparation()["running"].toBool());
        QCOMPARE(queue.preparation()["failed"].toInteger(), 8);
        QVERIFY(!queue.preparation()["complete"].toBool());
        QVERIFY(!queue.preparation()["error"].toString().isEmpty());
    }
    void silentWorkerTimesOutAndNextFileContinues() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("stalled.wav")));
        QVERIFY(writeWav(data.filePath("next.wav")));
        QVERIFY(seed(data.path(), {"stalled.wav", "next.wav"}));
        qputenv("AUDIO_TEST_STALL_WAVE", "1");
        const auto cleanup = qScopeGuard([] { qunsetenv("AUDIO_TEST_STALL_WAVE"); });
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.prepareAll();
        QTRY_VERIFY(queue.process_ && queue.process_->state() == QProcess::Running);
        qunsetenv("AUDIO_TEST_STALL_WAVE"); // Only the first worker inherited the deliberate stall.
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 18000);
        QCOMPARE(queue.preparation()["done"].toInteger(), 1);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
        const auto failures = queue.preparation()["failures"].toArray();
        QCOMPARE(failures.size(), 1);
        QCOMPARE(failures[0].toObject()["kind"].toString(), QString("timeout"));
    }
    void corruptFilesDoNotStopTheRestOfTheLibrary() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 10; ++i) {
            const auto name = QString("bad-%1.mp3").arg(i);
            QFile file(data.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("not an audio file");
            names << name;
        }
        names << "valid.wav";
        QVERIFY(writeWav(data.filePath(names.last())));
        QVERIFY(seed(data.path(), names));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.prepareAll();
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 4000);
        QCOMPARE(queue.preparation()["done"].toInteger(), 1);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 10);
    }
    void livePreparationShowsCurrentFileAndReadProgress() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("large.wav"), 3072000));
        QVERIFY(seed(data.path(), {"large.wav"}));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        window.waveQueue_->prepareAll(1048576);
        QTRY_VERIFY_WITH_TIMEOUT(window.waveQueue_->preparation()["readBytes"].toInteger() > 0, 1800);
        auto *current = window.findChild<QLabel *>("wavePreparationCurrent");
        QVERIFY(current);
        QVERIFY(current->text().contains("large.wav"));
        QVERIFY(current->text().contains("MiB"));
        QVERIFY(window.waveQueue_->preparation()["running"].toBool());
        auto *toggle = window.findChild<QPushButton *>("waveCacheToggle");
        auto *panel = window.findChild<QWidget *>("cachePanel");
        QVERIFY(toggle && panel);
        QVERIFY(!panel->isVisible());
        QVERIFY(toggle->text().contains("缓存中"));
        toggle->click();
        QVERIFY(panel->isVisible());
        QCOMPARE(panel->window(), window.settingsDialog_);
        window.settingsDialog_->close();
        QVERIFY(!panel->isVisible());
        QVERIFY(window.waveQueue_->preparation()["running"].toBool());
    }
    void cacheFailuresRemainAccessibleWithoutOpeningSettingsAutomatically() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 10; ++i)
            names << QString("missing-%1.wav").arg(i);
        QVERIFY(seed(data.path(), names));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 10);
        auto *toggle = window.findChild<QPushButton *>("waveCacheToggle");
        auto *panel = window.findChild<QWidget *>("cachePanel");
        QVERIFY(toggle && panel);
        QVERIFY(!panel->isVisible());
        window.waveQueue_->prepareAll();
        QTRY_VERIFY(!window.waveQueue_->preparation()["running"].toBool());
        QVERIFY(!window.waveQueue_->preparation()["error"].toString().isEmpty());
        QVERIFY(toggle->text().contains("处理"));
        QVERIFY(!panel->isVisible());
        toggle->click();
        QVERIFY(panel->isVisible());
    }
    void userCanSkipOneSlowFileWithoutStoppingPreparation() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("large.wav"), 3072000));
        QVERIFY(writeWav(data.filePath("next.wav")));
        QVERIFY(seed(data.path(), {"large.wav", "next.wav"}));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        queue.prepareAll(1048576);
        QTRY_VERIFY(queue.active_.id && queue.batchActive_);
        QVERIFY(QMetaObject::invokeMethod(&queue, "skipCurrent"));
        QTRY_VERIFY_WITH_TIMEOUT(queue.preparation()["complete"].toBool(), 2000);
        QCOMPARE(queue.preparation()["done"].toInteger(), 1);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
        queue.prepareAll();
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 2);
        Index index(data.filePath("library.sqlite"));
        QVERIFY(index.open());
        const auto track = index.search("large").tracks[0];
        QVERIFY(queue.find(track) && !queue.find(track)->failed);
    }
    void failureDetailsSurviveRestart() {
        QTemporaryDir data;
        QFile bad(data.filePath("bad.mp3"));
        QVERIFY(bad.open(QIODevice::WriteOnly));
        bad.write("not audio");
        bad.close();
        QVERIFY(seed(data.path(), {"bad.mp3"}));
        {
            WaveQueue queue(data.filePath("peaks"));
            queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
            queue.prepareAll();
            QTRY_VERIFY(queue.preparation()["complete"].toBool());
            QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
        }
        WaveQueue reopened(data.filePath("peaks"));
        reopened.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        const auto failures = reopened.preparation()["failures"].toArray();
        QCOMPARE(failures.size(), 1);
        QCOMPARE(failures[0].toObject()["name"].toString(), QString("bad.mp3"));
        QCOMPARE(failures[0].toObject()["kind"].toString(), QString("invalid_audio"));
        QVERIFY(!failures[0].toObject()["message"].toString().isEmpty());
    }
    void damagedAudioKeepsClearlyMarkedReferenceCacheOffline() {
        QTemporaryDir data;
        QFile fixture(QFINDTESTDATA("fixtures/sine-with-cover.mp3"));
        QVERIFY(fixture.open(QIODevice::ReadOnly));
        QFile source(data.filePath("reference.mp3"));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write(fixture.readAll() + QByteArray("junk").repeated(1024));
        source.close();
        QVERIFY(seed(data.path(), {"reference.mp3"}));
        Index index(data.filePath("library.sqlite"));
        QVERIFY(index.open());
        const auto track = index.search("").tracks[0];
        {
            WaveQueue queue(data.filePath("peaks"));
            queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
            queue.prepareAll();
            QTRY_VERIFY(queue.preparation()["complete"].toBool());
            QCOMPARE(queue.preparation()["done"].toInteger(), 1);
            QCOMPARE(queue.preparation()["reference"].toInteger(), 1);
            QCOMPARE(queue.preparation()["failed"].toInteger(), 0);
        }
        QVERIFY(QFile::rename(source.fileName(), data.filePath("offline.mp3")));
        WaveQueue reopened(data.filePath("peaks"));
        reopened.setVisible({track});
        QTRY_VERIFY(reopened.find(track) && reopened.find(track)->complete);
        QVERIFY(reopened.find(track)->approximate);
        QVERIFY(!reopened.process_);
        QCOMPARE(index.search("").tracks[0].duration, 0);
    }
    void referenceRowsShowEstimatesUntilOriginalPlayerReportsDuration() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("A.wav")));
        QVERIFY(writeWav(data.filePath("B.wav")));
        QVERIFY(seed(data.path(), {"A.wav", "B.wav"}));
        Index index(data.filePath("library.sqlite"));
        QVERIFY(index.open());
        const auto tracks = index.search("").tracks;
        QCOMPARE(tracks.size(), 2);
        WaveData reference;
        reference.sampleRate = 48000;
        reference.channels = 1;
        reference.frames = 96000;
        reference.stride = 65536;
        reference.buckets = {{Peak{-0.5f, 0.5f}}, {Peak{-0.5f, 0.5f}}};
        for (const auto &track : tracks)
            QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(track), reference, true));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(window.waveQueue_->find(window.model_->tracks[0]) &&
                                     window.waveQueue_->find(window.model_->tracks[1]),
                                 3000);
        for (int row = 0; row < 2; ++row) {
            const Track track = window.model_->tracks[row];
            const auto *cached = window.waveQueue_->find(track);
            QVERIFY(cached && cached->complete && cached->approximate && cached->stored);
            QCOMPARE(window.model_->index(row, TrackModel::Duration).data().toString(), QString("约 0:02"));
        }
        const Track second = window.model_->tracks[1];
        RowWave retained = *window.waveQueue_->find(second);
        retained.failed = true; // A failed recheck must not hide a usable old reference view.
        window.waveQueue_->entries_[WaveQueue::key(second)] = retained;
        window.updateWave(second);
        QCOMPARE(window.model_->index(1, TrackModel::Duration).data().toString(), QString("约 0:02"));
        const auto waveIndex = window.model_->index(1, TrackModel::Wave);
        QStyleOptionViewItem waveOption;
        waveOption.rect = window.table_->visualRect(waveIndex);
        const auto point = waveOption.rect.center();
        QHelpEvent help(QEvent::ToolTip, point, window.table_->viewport()->mapToGlobal(point));
        QVERIFY(window.rowWave_->helpEvent(&help, window.table_, waveOption, waveIndex));
        QVERIFY(QToolTip::text().contains("约 0:02"));
        QToolTip::hideText();
        const auto stored = index.search("").tracks;
        QCOMPARE(stored[0].duration, qint64(0));
        QCOMPARE(stored[1].duration, qint64(0));

        window.selected_ = window.model_->tracks[0];
        window.selectionGeneration_ = 7;
        window.handlePlayer({{"event", "position"}, {"generation", 6}, {"duration", 3000}});
        QCOMPARE(window.model_->index(0, TrackModel::Duration).data().toString(), QString("约 0:02"));
        window.handlePlayer(
            {{"event", "position"}, {"generation", 7}, {"position", 1000}, {"duration", 3000}});
        QCOMPARE(window.model_->index(0, TrackModel::Duration).data().toString(), QString("0:03"));
        QCOMPARE(window.model_->index(1, TrackModel::Duration).data().toString(), QString("约 0:02"));
        QCOMPARE(window.time_->text(), QString("0:01 / 0:03"));
        QTRY_COMPARE(index.search("A").tracks[0].duration, qint64(3000));
        // A repeated position tick must not queue another metadata write.
        index.setDuration(window.selected_, 9000);
        window.handlePlayer(
            {{"event", "position"}, {"generation", 7}, {"position", 1500}, {"duration", 3000}});
        window.metadataPool_.waitForDone();
        QCOMPARE(index.search("A").tracks[0].duration, qint64(9000));
        QCOMPARE(window.model_->index(0, TrackModel::Duration).data().toString(), QString("0:03"));
        QCOMPARE(window.selected_.duration, qint64(3000));

        const auto databasePath = window.databasePath_;
        QFile blocker(data.filePath("blocked-directory"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("not a directory");
        blocker.close();
        window.databasePath_ = data.filePath("blocked-directory/library.sqlite");
        window.handlePlayer(
            {{"event", "position"}, {"generation", 7}, {"position", 1500}, {"duration", 4000}});
        QCOMPARE(window.model_->index(0, TrackModel::Duration).data().toString(), QString("0:04"));
        QTRY_VERIFY_WITH_TIMEOUT(
            window.durationWriteRetry_.interval() == 2000 && !window.pendingDurationWrites_.isEmpty(), 1000);
        window.databasePath_ = databasePath;
        QTRY_COMPARE_WITH_TIMEOUT(index.search("A").tracks[0].duration, qint64(4000), 4000);

        const Track first = window.model_->tracks[0];
        window.handlePlayer(
            {{"event", "position"}, {"generation", 7}, {"position", 1500}, {"duration", 5000}});
        QVERIFY(!window.pendingDurationWrites_.isEmpty());
        RowWave verified = *window.waveQueue_->find(first);
        verified.approximate = false;
        window.waveQueue_->entries_[WaveQueue::key(first)] = verified;
        window.updateWave(first);
        QVERIFY(window.pendingDurationWrites_.isEmpty());
        QCOMPARE(window.model_->index(0, TrackModel::Duration).data().toString(), QString("0:02"));
        QTRY_COMPARE_WITH_TIMEOUT(index.search("A").tracks[0].duration, qint64(2000), 2000);

        RowWave unavailable;
        unavailable.failed = true;
        unavailable.estimatedDuration = 2000;
        window.waveQueue_->entries_[WaveQueue::key(second)] = unavailable;
        window.updateWave(second);
        QCOMPARE(window.model_->index(1, TrackModel::Duration).data().toString(), QString("—"));
    }
    void verifiedWaveReplacesPlayerDurationWhenIndexedValueOriginallyMatched() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("music.wav")));
        QVERIFY(seed(data.path(), {"music.wav"}));
        Index index(data.filePath("library.sqlite"));
        QVERIFY(index.open());
        Track track = index.search("").tracks[0];
        QVERIFY(index.setDuration(track, 2000));
        track = index.search("").tracks[0];
        QCOMPARE(track.duration, qint64(2000));
        WaveData reference;
        reference.sampleRate = 48000;
        reference.channels = 1;
        reference.frames = 96000;
        reference.stride = 65536;
        reference.buckets = {{Peak{-0.5f, 0.5f}}, {Peak{-0.5f, 0.5f}}};
        QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(track), reference, true));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QTRY_VERIFY(window.waveQueue_->find(track) && window.waveQueue_->find(track)->approximate);
        window.selected_ = window.model_->tracks[0];
        window.selectionGeneration_ = 9;
        window.handlePlayer(
            {{"event", "position"}, {"generation", 9}, {"position", 1000}, {"duration", 3000}});
        QTRY_COMPARE(index.search("").tracks[0].duration, qint64(3000));
        QTRY_VERIFY(window.pendingDurationWrites_.isEmpty());
        QCOMPARE(window.model_->index(0, TrackModel::Duration).data().toString(), QString("0:03"));
        RowWave verified = *window.waveQueue_->find(track);
        verified.approximate = false;
        window.waveQueue_->entries_[WaveQueue::key(track)] = verified;
        window.updateWave(track); // The queued Track still says 2 seconds.
        QCOMPARE(window.model_->index(0, TrackModel::Duration).data().toString(), QString("0:02"));
        QTRY_COMPARE(index.search("").tracks[0].duration, qint64(2000));
        QCOMPARE(window.selected_.duration, qint64(2000));
    }
    void staleCachedViewIsRevalidatedWithoutLosingImmediateDisplay() {
        QTemporaryDir data;
        const auto source = data.filePath("stale.wav");
        QVERIFY(writeWav(source));
        QVERIFY(seed(data.path(), {"stale.wav"}));
        Index index(data.filePath("library.sqlite"));
        QVERIFY(index.open());
        const auto tracks = index.search("").tracks;
        PeakBuilder builder(48000, 1);
        const float samples[256] = {0.9f};
        builder.append(samples, 256);
        const auto previous = encodeWave(builder.finish());
        const auto cache = data.filePath("peaks");
        const auto key = WaveQueue::key(tracks[0]);
        QVERIFY(saveWaveView(cache, key, builder.finish()));
        QFile view(QDir(cache).filePath("views-v1/" + key.left(2) + "/" + key + ".view"));
        QVERIFY(view.open(QIODevice::ReadWrite));
        QVERIFY(view.seek(4));
        QDataStream stamp(&view);
        stamp << qint64(QDateTime::currentMSecsSinceEpoch() - 25LL * 3600000);
        view.close();
        WaveQueue queue(cache);
        bool displayedCached = false;
        connect(&queue, &WaveQueue::changed, this, [&](const Track &) {
            const auto *entry = queue.find(tracks[0]);
            if (entry && entry->complete && encodeWave(entry->data) == previous)
                displayedCached = true;
        });
        queue.setVisible(tracks);
        QTRY_VERIFY(queue.find(tracks[0]) && queue.find(tracks[0])->stored &&
                    encodeWave(queue.find(tracks[0])->data) != previous);
        QVERIFY(displayedCached);
    }
    void wholeLibraryUsesOneReusableWorker() {
        QTemporaryDir data;
        for (int i = 0; i < 3; ++i)
            QVERIFY(writeWav(data.filePath(QString::number(i) + ".wav")));
        QVERIFY(seed(data.path(), {"0.wav", "1.wav", "2.wav"}));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("preparation.json"));
        QSet<qint64> workers;
        connect(&queue, &WaveQueue::changed, this, [&](const Track &) {
            if (queue.process_ && queue.process_->processId())
                workers.insert(queue.process_->processId());
        });
        queue.prepareAll();
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 3);
        QCOMPARE(workers.size(), 1);
    }
    void startingBatchAppliesNewBudgetToCurrentBackgroundJob() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("large.wav"), 1536000));
        QVERIFY(seed(data.path(), {"large.wav"}));
        Index index(data.filePath("library.sqlite"));
        QVERIFY(index.open());
        const auto tracks = index.search("").tracks;
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("preparation.json"));
        queue.setVisible(tracks);
        QTRY_VERIFY(queue.active_.id && queue.process_->state() == QProcess::Running);
        queue.prepareAll(10 * 1048576);
        QTRY_VERIFY_WITH_TIMEOUT(queue.find(tracks[0]) && queue.find(tracks[0])->complete, 1500);
    }
    void preparationReportsTotalWhileVisibleAudioIsStillDecoding() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("large.wav"), 1536000));
        QVERIFY(seed(data.path(), {"large.wav"}));
        Index index(data.filePath("library.sqlite"));
        QVERIFY(index.open());
        const auto tracks = index.search("").tracks;
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("preparation.json"));
        queue.setVisible(tracks);
        QTRY_VERIFY(queue.active_.id && queue.process_->state() == QProcess::Running);
        queue.prepareAll(1048576);
        QTRY_COMPARE_WITH_TIMEOUT(queue.preparation()["total"].toInteger(), 1, 500);
        QVERIFY(!queue.find(tracks[0])->complete);
    }
    void failedWorkerLaunchIsReportedWithoutCrashing() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("launch.wav")));
        QVERIFY(seed(data.path(), {"launch.wav"}));
        WaveQueue queue(data.filePath("peaks"));
        queue.workerProgram_ = data.filePath("unavailable-worker.exe");
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("preparation.json"));
        queue.prepareAll();
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
    }
    void scrollingCachedRowsDoesNotRestartWholeLibraryDecoding() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("large.wav"), 1536000));
        QVERIFY(seed(data.path(), {"large.wav"}));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("preparation.json"));
        queue.prepareAll(1048576);
        QTRY_VERIFY(queue.batchActive_ && queue.active_.id);
        const auto *process = queue.process_;
        for (int i = 0; i < 5; ++i) {
            queue.setVisible({});
            QTest::qWait(20);
            QCOMPARE(queue.process_, process);
            QVERIFY(!queue.cancelled_);
        }
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 1);
    }
    void refreshedViewsResetPreparationAndChangedSourcesAreNotCredited() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("change.wav")));
        QVERIFY(seed(data.path(), {"change.wav"}));
        WaveQueue queue(data.filePath("peaks"));
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("preparation.json"));
        queue.prepareAll();
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 1);
        queue.invalidate();
        QCOMPARE(queue.preparation()["done"].toInteger(), 0);
        QVERIFY(!queue.preparation()["complete"].toBool());
        QVERIFY(writeWav(data.filePath("change.wav"), 192000));
        queue.prepareAll();
        QTRY_VERIFY(queue.preparation()["complete"].toBool());
        QCOMPARE(queue.preparation()["done"].toInteger(), 0);
        QCOMPARE(queue.preparation()["failed"].toInteger(), 1);
    }
    void fullPreparationCanPauseResumeAndSurvivesRestart() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 30; ++i) {
            const auto name = QString("batch-%1.wav").arg(i, 2, 10, QChar('0'));
            names << name;
            QVERIFY(writeWav(data.filePath(name), 240000));
        }
        QVERIFY(seed(data.path(), names));
        {
            Window first(data.path());
            first.show();
            QTRY_COMPARE(first.model_->rowCount(), 30);
            auto *start = first.findChild<QPushButton *>("prepareAllWaves");
            QVERIFY2(start, "Provide a full-library preparation control");
            start->click();
            QTRY_VERIFY(first.waveQueue_->preparation()["done"].toInteger() > 0);
            auto *pause = first.findChild<QPushButton *>("pauseWavePreparation");
            QTRY_VERIFY(pause && pause->isEnabled());
            pause->click();
            QVERIFY(first.findChild<QLabel *>("wavePreparationStatus")->text().contains("暂停"));
        }
        Window second(data.path());
        second.show();
        QTRY_COMPARE(second.model_->rowCount(), 30);
        QVERIFY(second.waveQueue_->preparation()["cursor"].toInteger() > 0);
        auto *resume = second.findChild<QPushButton *>("prepareAllWaves");
        QVERIFY(resume);
        resume->click();
        QTRY_VERIFY_WITH_TIMEOUT(second.waveQueue_->preparation()["complete"].toBool(), 10000);
        QVERIFY(second.findChild<QLabel *>("wavePreparationStatus")->text().contains("本轮结束"));
        // Entire indexed library was prepared, independently of the viewport.
        for (const auto &t : second.model_->tracks) {
            WaveData wave;
            QVERIFY(loadWaveView(data.filePath("peaks"), WaveQueue::key(t), &wave));
        }
    }
    void cachedRowsBypassColdDecodeAndDatabaseWriteLocks() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("A-cold.wav"), 1536000));
        QVERIFY(writeWav(data.filePath("B-cached.wav")));
        QVERIFY(seed(data.path(), {"A-cold.wav", "B-cached.wav"}));
        Index index(data.filePath("library.sqlite"));
        QVERIFY(index.open());
        const auto tracks = index.search("").tracks;
        PeakBuilder builder(48000, 1);
        const float samples[256] = {0.5f};
        builder.append(samples, 256);
        QVERIFY(saveWaveView(data.filePath("peaks"), WaveQueue::key(tracks[1]), builder.finish()));
        auto lockDb = QSqlDatabase::addDatabase("QSQLITE", "wave-locked-index");
        lockDb.setDatabaseName(data.filePath("library.sqlite"));
        QVERIFY(lockDb.open());
        QSqlQuery lock(lockDb);
        QVERIFY(lock.exec("BEGIN IMMEDIATE"));
        auto unlock = qScopeGuard([&] { lock.exec("ROLLBACK"); });
        Window window(data.path());
        window.show();
        QElapsedTimer elapsed;
        elapsed.start();
        QTRY_VERIFY_WITH_TIMEOUT(
            window.waveQueue_->find(tracks[1]) && window.waveQueue_->find(tracks[1])->complete, 900);
        QVERIFY2(elapsed.elapsed() < 900,
                 "A locked metadata write or cold row must not block cached display");
        QVERIFY(lock.exec("ROLLBACK"));
        unlock.dismiss();
        lock = QSqlQuery();
        lockDb.close();
        lockDb = QSqlDatabase();
        QSqlDatabase::removeDatabase("wave-locked-index");
    }
    void preparationReportsFailuresAndRetriesAfterSourceReturns() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("valid.wav")));
        QVERIFY(seed(data.path(), {"valid.wav", "missing.wav"}));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 2);
        window.waveQueue_->prepareAll();
        QTRY_VERIFY(window.waveQueue_->preparation()["complete"].toBool());
        QCOMPARE(window.waveQueue_->preparation()["done"].toInteger(), 1);
        QCOMPARE(window.waveQueue_->preparation()["failed"].toInteger(), 1);
        QVERIFY(writeWav(data.filePath("missing.wav")));
        QVERIFY(seed(data.path(), {"valid.wav", "missing.wav"}));
        window.waveQueue_->prepareAll();
        QTRY_VERIFY(window.waveQueue_->preparation()["complete"].toBool());
        QCOMPARE(window.waveQueue_->preparation()["done"].toInteger(), 2);
        QCOMPARE(window.waveQueue_->preparation()["failed"].toInteger(), 0);
    }
    void cachedWavesReopenWithoutOriginalAudioOrWorkers() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("cached.wav")));
        QVERIFY(seed(data.path(), {"cached.wav"}));
        Track track;
        {
            Window first(data.path());
            first.show();
            QTRY_COMPARE(first.model_->rowCount(), 1);
            track = first.model_->tracks[0];
            QTRY_VERIFY(first.waveQueue_->find(track) && first.waveQueue_->find(track)->complete);
            QTest::qWait(150); // Allow atomic cache publication to finish.
        }
        QVERIFY(QFile::rename(data.filePath("cached.wav"), data.filePath("offline.wav")));
        Window reopened(data.path());
        reopened.show();
        QTRY_COMPARE(reopened.model_->rowCount(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(
            reopened.waveQueue_->find(track) && reopened.waveQueue_->find(track)->complete, 1000);
        QVERIFY(reopened.waveQueue_->findChildren<QProcess *>().isEmpty());
    }
    void composingChineseDoesNotActivatePreviousFolderMatch() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"旧目录/song.wav"}));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_);
        window.folderSearch_->setText("旧目录");
        QTRY_COMPARE(window.folderMatches_.size(), 1);
        window.folderSearch_->setFocus();
        QInputMethodEvent preedit("xin", {});
        QApplication::sendEvent(window.folderSearch_, &preedit);
        QTest::keyClick(window.folderSearch_, Qt::Key_Return);
        QTest::qWait(150);
        QVERIFY2(window.scopeLibrary_.isEmpty(),
                 "IME confirmation must not activate a stale folder candidate");
    }
    void addingLibrarySelectsItsTreeNodeAndAllScopeRemainsUsable() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"old.wav"}));
        const auto newRoot = data.filePath("新增库");
        QVERIFY(QDir().mkpath(newRoot));
        QVERIFY(writeWav(QDir(newRoot).filePath("new.wav"), 4800));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto edits = dialog->findChildren<QLineEdit *>();
            QVERIFY(!edits.isEmpty());
            edits.first()->setText(newRoot);
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        });
        window.addLibrary();
        QTRY_VERIFY(window.libraries_.size() == 2 && !window.scan_ && !window.loading_);
        QVERIFY(!window.scopeLibrary_.isEmpty());
        QCOMPARE(window.tree_->currentIndex().data(FolderModel::LibraryRole).toString(),
                 window.scopeLibrary_);
        QVERIFY(window.scopeLabel_->text().contains("新增库"));
        QPushButton *all = nullptr;
        for (auto *button : window.findChildren<QPushButton *>())
            if (button->text() == "全部素材")
                all = button;
        QVERIFY(all);
        all->click();
        QTRY_VERIFY(window.scopeLibrary_.isEmpty() && window.model_->rowCount() == 2);
    }
    void failedProfileImportCannotBeBypassedByAddingLibrary() {
        QTemporaryDir data;
        QFile corrupt(data.filePath("corrupt.db"));
        QVERIFY(corrupt.open(QIODevice::WriteOnly));
        corrupt.write("not a sqlite database");
        corrupt.close();
        const auto profile = data.filePath("profile");
        Window window(profile, corrupt.fileName());
        window.show();
        QTRY_VERIFY(window.status_->text().contains("未就绪"));
        QVERIFY(!window.indexReady_);
        auto *add = window.findChild<QPushButton *>("libraryAction");
        QVERIFY(add);
        QVERIFY2(!add->isEnabled(), "A failed import must not permit creating an empty replacement index");
        QVERIFY(!QFileInfo::exists(QDir(profile).filePath("library.sqlite")));
    }
    void obsoleteFolderRequestsSkipDatabaseWork() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"folder/song.wav"}));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        auto lock = QSqlDatabase::addDatabase("QSQLITE", "folder-queue-lock");
        lock.setDatabaseName(data.filePath("library.sqlite"));
        QVERIFY(lock.open());
        QSqlQuery query(lock);
        QVERIFY(query.exec("PRAGMA journal_mode=DELETE"));
        QVERIFY(query.exec("BEGIN EXCLUSIVE"));
        const auto cleanup = qScopeGuard([&] {
            query.exec("ROLLBACK");
            query = QSqlQuery();
            lock.close();
            lock = QSqlDatabase();
            QSqlDatabase::removeDatabase("folder-queue-lock");
        });
        QSemaphore gate;
        window.folderPool_.start([&] { gate.acquire(); });
        for (int i = 0; i < 8; ++i) {
            window.folderSearch_->setText(QString("query%1").arg(i));
            window.searchFolders();
        }
        window.folderDebounce_.stop();
        gate.release();
        QTRY_VERIFY_WITH_TIMEOUT(window.folderHint_->text().contains("未完成"), 4500);
    }
    void folderSearchLocatesDeepNodeKeepsPlaybackAndSupportsHistory() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("playing.wav"), 480000));
        QVERIFY(seed(data.path(),
                     {"playing.wav", "古风/伤感/雨.wav", "现代/伤感/雪.wav", "古风/伤感/回忆/叶.wav"}));
        Window window(data.path());
        window.show();
        window.volume_->setValue(0);
        QTRY_COMPARE(window.model_->rowCount(), 4);
        int playing = -1;
        for (int i = 0; i < window.model_->rowCount(); ++i)
            if (window.model_->tracks[i].name == "playing.wav")
                playing = i;
        QVERIFY(playing >= 0);
        window.selectTrack(playing, .1);
        QTRY_VERIFY(window.isPlaying_);
        const auto selected = window.selected_.id;
        const auto generation = window.selectionGeneration_;
        window.search_->setText("unmatched");
        QTRY_COMPARE(window.model_->rowCount(), 0);
        window.folderSearch_->setText("伤感");
        QTRY_COMPARE(window.folderMatches_.size(), 3);
        const int target = [&] {
            for (int i = 0; i < window.folderMatches_.size(); ++i)
                if (window.folderMatches_[i].path == "古风/伤感")
                    return i;
            return -1;
        }();
        QVERIFY(target >= 0);
        window.folderResults_->setCurrentRow(target);
        window.folderSearch_->setFocus();
        QTest::keyClick(window.folderSearch_, Qt::Key_Return);
        QTRY_COMPARE(window.scopeFolder_, QString("古风/伤感"));
        QTRY_COMPARE(window.model_->rowCount(), 2);
        QCOMPARE(window.tree_->currentIndex().data(FolderModel::PathRole).toString(), QString("古风/伤感"));
        QVERIFY(window.tree_->isExpanded(window.tree_->currentIndex().parent()));
        QVERIFY(window.tree_->visualRect(window.tree_->currentIndex())
                    .intersects(window.tree_->viewport()->rect()));
        QCOMPARE(window.search_->text(), QString());
        QCOMPARE(window.selected_.id, selected);
        QCOMPARE(window.selectionGeneration_, generation);
        QVERIFY(window.isPlaying_);
        window.subfolders_->setChecked(false);
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QCOMPARE(window.model_->tracks[0].name, QString("雨.wav"));
        window.folderSearch_->clear();
        QTest::qWait(120);
        QCOMPARE(window.scopeFolder_, QString("古风/伤感"));
        window.navigateHistory(-1);
        QTRY_COMPARE(window.scopeFolder_, QString());
        QTRY_COMPARE(window.search_->text(), QString("unmatched"));
        window.navigateHistory(1);
        QTRY_COMPARE(window.scopeFolder_, QString("古风/伤感"));
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QVERIFY(!window.subfolders_->isChecked());
    }
    void directoryTreeLoadsOnDemandAndRefreshPreservesSelection() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"root.wav"}));
        {
            Index db(data.filePath("library.sqlite"));
            QVERIFY(db.open());
            QStringList folders;
            for (int i = 0; i < 1000; ++i)
                folders << QString("Folder%1/child").arg(i, 4, 10, QChar('0'));
            QVERIFY(db.upsertFolders("qa", folders));
        }
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_);
        const auto library = window.folderModel_->index(2, 0);
        QCOMPARE(window.folderModel_->rowCount(library), 0);
        window.locateFolder("qa", "Folder0999/child");
        QTRY_COMPARE(window.scopeFolder_, QString("Folder0999/child"));
        QCOMPARE(window.tree_->currentIndex().data(FolderModel::PathRole).toString(),
                 QString("Folder0999/child"));
        const auto current = QPersistentModelIndex(window.tree_->currentIndex());
        window.reloadLibraries();
        QTest::qWait(300);
        QVERIFY(current.isValid());
        QCOMPARE(window.tree_->currentIndex(), QModelIndex(current));
        QVERIFY(window.tree_->isExpanded(current.parent()));
        QVERIFY2(window.tree_->visualRect(current).intersects(window.tree_->viewport()->rect()),
                 "Later sibling pages must not push the located node out of view");
        window.folderSearch_->setText("Folder0001");
        QTest::qWait(95);
        window.folderSearch_->setText("Folder0999 child");
        QTRY_VERIFY(window.folderMatches_.size() == 1 && window.folderMatches_[0].path == "Folder0999/child");
    }
    void historyCancelsPendingSearchAndRestoresFullListScroll() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 150; ++i)
            names << QString("A/track%1.wav").arg(i, 3, 10, QChar('0'));
        names << "B/other.wav";
        QVERIFY(seed(data.path(), names));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_);
        window.locateFolder("qa", "A");
        QTRY_VERIFY(window.scopeFolder_ == "A" && !window.loading_ && window.model_->rowCount() == 150);
        window.table_->verticalScrollBar()->setValue(60);
        QCOMPARE(window.table_->verticalScrollBar()->value(), 60);
        window.locateFolder("qa", "B");
        QTRY_COMPARE(window.model_->rowCount(), 1);
        window.search_->setText("not-present");
        window.navigateHistory(-1);
        QTRY_VERIFY(window.scopeFolder_ == "A" && !window.loading_ && window.model_->rowCount() == 150);
        QTest::qWait(350);
        QCOMPARE(window.table_->verticalScrollBar()->value(), 60);
        QCOMPARE(window.search_->text(), QString());
        window.navigateHistory(-1);
        window.navigateHistory(1);
        QTRY_VERIFY(window.scopeFolder_ == "A" && !window.loading_ && window.model_->rowCount() == 150);
        QCOMPARE(window.table_->verticalScrollBar()->value(), 60);
    }
    void refreshingFullResultsPreservesScrollAndSelection() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 125; ++i)
            names << QString("track%1.wav").arg(i, 3, 10, QChar('0'));
        QVERIFY(seed(data.path(), names));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(!window.loading_ && window.model_->rowCount() == 125);
        {
            QSignalBlocker blocker(window.table_->selectionModel());
            window.table_->setCurrentIndex(window.model_->index(90, TrackModel::Name));
        }
        window.table_->verticalScrollBar()->setValue(85);
        const int scroll = window.table_->verticalScrollBar()->value();
        QSignalSpy reset(window.model_, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(window.model_, &QAbstractItemModel::rowsInserted);
        window.refreshSearch();
        QTRY_VERIFY(!window.loading_ && window.model_->rowCount() == 125);
        QCOMPARE(reset.size(), 1);
        QCOMPARE(inserted.size(), 0);
        QCOMPARE(window.table_->currentIndex().row(), 90);
        QCOMPARE(window.table_->verticalScrollBar()->value(), scroll);
        QCOMPARE(window.model_->tracks.last().name, QString("track124.wav"));
        QVERIFY(!window.more_->isVisible());
    }
    void failedMusicSearchKeepsResultsAndCanRetry_data() {
        QTest::addColumn<bool>("failOpen");
        QTest::newRow("open-failure") << true;
        QTest::newRow("query-failure") << false;
    }
    void failedMusicSearchKeepsResultsAndCanRetry() {
        QFETCH(bool, failOpen);
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"rain.wav", "wind.wav"}));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(!window.loading_ && window.model_->rowCount() == 2);
        const auto database = window.databasePath_;
        const QPersistentModelIndex retained(window.model_->index(0, TrackModel::Name));
        if (failOpen) {
            window.databasePath_ = data.path(); // A directory cannot be opened as SQLite.
        } else {
            const auto connection = QString("failed-music-query");
            {
                auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
                db.setDatabaseName(database);
                QVERIFY(db.open());
                QSqlQuery q(db);
                QVERIFY(q.exec("DROP TABLE search_fts"));
            }
            QSqlDatabase::removeDatabase(connection);
        }
        {
            QSignalBlocker blocker(window.search_);
            window.search_->setText("rain");
        }
        window.refreshSearch();
        QTRY_VERIFY(!window.loading_);
        QCOMPARE(window.model_->rowCount(), 2);
        QVERIFY(retained.isValid());
        QVERIFY(window.results_->text().contains("未完成"));
        window.databasePath_ = database;
        {
            QSignalBlocker blocker(window.search_);
            window.search_->clear();
        }
        window.refreshSearch(); // A clean query must distinguish success from the previous failure.
        QTRY_VERIFY(!window.loading_);
        QCOMPARE(window.model_->rowCount(), 2);
        QVERIFY(!window.results_->text().contains("未完成"));
    }
    void completeListBeyondOldLimitDoesNotOfferAnotherPage() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 2101; ++i)
            names << QString("track%1.wav").arg(i, 4, 10, QChar('0'));
        QVERIFY(seed(data.path(), names));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(!window.loading_ && window.model_->rowCount() == 2101);
        QVERIFY(!window.more_->isVisible());
        QVERIFY(!window.hasMore_);
        const auto track = window.model_->tracks[0];
        window.waveQueue_->setVisible({track});
        window.viewportDebounce_.start();
        const auto generation = window.queryGeneration_;
        window.refreshSearch(true); // A legacy request for another page is now a no-op.
        QCOMPARE(window.waveQueue_->visible_.size(), 1);
        QCOMPARE(window.queryGeneration_, generation);
        QVERIFY(window.viewportDebounce_.isActive());
        QVERIFY(!window.loading_);
        QCOMPARE(window.model_->tracks.last().name, QString("track2100.wav"));
    }
    void folderLocatorIsSeparateFromMusicSearch() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"rain.wav"}));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        auto *locator = window.findChild<QLineEdit *>("folderSearch");
        QVERIFY2(locator, "Folder locator must be independently accessible in the left sidebar");
        locator->setText("missing-folder");
        QTest::qWait(250);
        QCOMPARE(window.search_->text(), QString());
        QCOMPARE(window.model_->rowCount(), 1);
    }
    void switchingToCachedTrackCancelsHiddenFormerForegroundJob() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 20; ++i) {
            const auto name = QString("music-%1.wav").arg(i, 2, 10, QChar('0'));
            names << name;
            QVERIFY(writeWav(data.filePath(name), i == 19 ? 1536000 : 4800));
        }
        QVERIFY(seed(data.path(), names));
        Window window(data.path());
        window.show();
        window.volume_->setValue(0);
        QTRY_COMPARE(window.model_->rowCount(), 20);
        window.selectTrack(0);
        QTRY_VERIFY(window.waveQueue_->find(window.model_->tracks[0]) &&
                    window.waveQueue_->find(window.model_->tracks[0])->complete);
        const auto oldForeground = window.model_->tracks[19];
        // Same event-loop turn: the cold job is starting when the cached row is selected.
        window.selectTrack(19);
        window.selectTrack(0);
        QTest::qWait(700);
        QVERIFY2(!window.waveQueue_->find(oldForeground), "Hidden former foreground job must be cancelled");
        QCOMPARE(window.selected_.id, window.model_->tracks[0].id);
    }
    void refreshRevalidatesWaveEvenWhenSizeAndTimestampArePreserved() {
        QTemporaryDir data;
        const auto music = data.filePath("music");
        QVERIFY(QDir().mkpath(music));
        const auto path = QDir(music).filePath("replace.wav");
        QVERIFY(writeWav(path));
        QVERIFY(seed(data.path(), {"replace.wav"}, music));
        Window window(data.path());
        window.show();
        window.volume_->setValue(0);
        QTRY_COMPARE(window.model_->rowCount(), 1);
        window.table_->setCurrentIndex(window.model_->index(0, 1));
        auto complete = [&] {
            const auto *entry = window.waveQueue_->find(window.model_->tracks[0]);
            return entry && entry->complete;
        };
        QTRY_VERIFY(complete());
        const auto before = encodeWave(window.waveQueue_->find(window.model_->tracks[0])->data);
        const auto stamp = QFileInfo(path).lastModified();
        QFile changed(path);
        QVERIFY(changed.open(QIODevice::ReadWrite));
        QVERIFY(changed.seek(44));
        QCOMPARE(changed.write(QByteArray(192000, char(0))), 192000);
        QVERIFY(changed.flush());
        QVERIFY(changed.setFileTime(stamp, QFileDevice::FileModificationTime));
        changed.close();
        QCOMPARE(QFileInfo(path).size(), qint64(192044));
        QCOMPARE(QFileInfo(path).lastModified(), stamp);
        window.beginScan(window.libraries_[0]);
        QTRY_VERIFY(!window.scan_ && !window.loading_);
        QCOMPARE(window.model_->tracks[0].name, QString("replace.wav"));
        QTRY_VERIFY(complete() &&
                    encodeWave(window.waveQueue_->find(window.model_->tracks[0])->data) != before);
        const auto *entry = window.waveQueue_->find(window.model_->tracks[0]);
        for (const auto &bucket : entry->data.buckets)
            for (const auto &peak : bucket)
                QCOMPARE(peak.maximum, 0.f);
    }
    void clickingBackgroundWavePromotesItToForegroundSpeed() {
        QTemporaryDir data;
        QVERIFY(writeWav(data.filePath("long.wav"), 1536000));
        QVERIFY(seed(data.path(), {"long.wav"}));
        Window window(data.path());
        window.show();
        window.volume_->setValue(0);
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QTRY_VERIFY(window.waveQueue_->find(window.model_->tracks[0]));
        QVERIFY(!window.waveQueue_->find(window.model_->tracks[0])->complete);
        auto rect = window.table_->visualRect(window.model_->index(0, TrackModel::Wave));
        QTest::mouseClick(window.table_->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        QTRY_VERIFY_WITH_TIMEOUT(window.waveQueue_->find(window.model_->tracks[0]) &&
                                     window.waveQueue_->find(window.model_->tracks[0])->complete,
                                 1500);
        QTRY_VERIFY(window.isPlaying_ && window.position_ >= 15000);
    }
    void visibleRowsPrepareWavesWithoutReadingHiddenRowsOrPlaying() {
        QTemporaryDir data;
        QStringList names;
        for (int i = 0; i < 40; ++i) {
            auto name = QString("music-%1.wav").arg(i, 2, 10, QChar('0'));
            names << name;
            QVERIFY(writeWav(data.filePath(name), 4800));
        }
        QVERIFY(seed(data.path(), names));
        Window window(data.path());
        window.show();
        window.volume_->setValue(0);
        QTRY_COMPARE(window.model_->rowCount(), 40);
        QTRY_VERIFY_WITH_TIMEOUT(window.model_->tracks[0].duration > 0, 3000);
        QTest::qWait(500);
        QCOMPARE(window.model_->tracks[39].duration, 0);
        QVERIFY(!window.isPlaying_ && window.selected_.id == 0);
        window.table_->scrollTo(window.model_->index(39, 1), QAbstractItemView::PositionAtBottom);
        QTRY_VERIFY_WITH_TIMEOUT(window.model_->tracks[39].duration > 0, 3000);
        QCOMPARE(window.model_->tracks[20].duration, 0);
        QVERIFY(!window.isPlaying_ && window.selected_.id == 0);
    }
    void waveformClickStartsAtClickedLocationWithoutAutoplay() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"A.wav", "B.wav"}));
        QVERIFY(writeWav(data.filePath("A.wav"), 384000));
        QVERIFY(writeWav(data.filePath("B.wav"), 384000));
        Window window(data.path());
        window.resize(980,
                      640); // Cue hit-testing must also work with the stretched waveform at minimum size.
        window.show();
        window.volume_->setValue(0);
        window.autoPlay_->setChecked(false);
        QTRY_COMPARE(window.model_->rowCount(), 2);
        int column = -1;
        for (int c = 0; c < window.model_->columnCount(); ++c)
            if (window.model_->headerData(c, Qt::Horizontal, Qt::DisplayRole).toString().contains("波形"))
                column = c;
        QVERIFY2(column >= 0, "Each audio result must offer a clickable waveform");
        auto rect = window.table_->visualRect(window.model_->index(1, column)).adjusted(8, 0, -8, 0);
        QTest::mouseClick(window.table_->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(rect.left() + int(rect.width() * .6), rect.center().y()));
        QTRY_COMPARE(window.selected_.name, QString("B.wav"));
        QTRY_VERIFY(window.isPlaying_ && window.position_ >= 4700);
        QVERIFY(window.position_ < 6000);
        QTest::mouseClick(window.play_, Qt::LeftButton);
        QTRY_VERIFY(!window.isPlaying_);
        QTest::mouseClick(window.table_->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(rect.left() + int(rect.width() * .25), rect.center().y()));
        QTRY_VERIFY(window.isPlaying_ && window.position_ >= 1950 && window.position_ < 3000);
    }
    void offlinePlayRetriesSameSelectionAndPreservesFailureReason() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"reconnect.wav"}));
        Window window(data.path());
        window.show();
        window.volume_->setValue(0);
        QTRY_COMPARE(window.model_->rowCount(), 1);
        window.table_->setCurrentIndex(window.model_->index(0, 1));
        QTRY_VERIFY(window.waveNeedsRetry_);
        QTRY_VERIFY(window.status_->text().contains("试听失败"));
        QVERIFY(window.waveStatus_->text().contains("路径访问失败（尚未解码）"));
        QCOMPARE(window.waveQueue_->find(window.selected_)->failureKind, QString("source_unavailable"));
        const auto id = window.selected_.id;
        QVERIFY(writeWav(data.filePath("reconnect.wav")));
        QTest::mouseClick(window.play_, Qt::LeftButton);
        QTRY_VERIFY(!window.waveNeedsRetry_ && window.position_ > 100);
        QCOMPARE(window.selected_.id, id);
        QTRY_VERIFY(window.waveQueue_->find(window.selected_) &&
                    window.waveQueue_->find(window.selected_)->complete);
        // The restored file has a new mtime; playback works but the old index needs refreshing.
        QVERIFY(window.waveStatus_->text().contains("版本已变化"));
    }
    void clickingCurrentRowRetriesFailedWave() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"reconnect.wav"}));
        Window window(data.path());
        window.show();
        window.volume_->setValue(0);
        QTRY_COMPARE(window.model_->rowCount(), 1);
        window.table_->setCurrentIndex(window.model_->index(0, 1));
        QTRY_VERIFY(window.waveNeedsRetry_);
        QVERIFY(writeWav(data.filePath("reconnect.wav")));
        const auto rect = window.table_->visualRect(window.model_->index(0, 1));
        QTest::mouseClick(window.table_->viewport(), Qt::LeftButton, Qt::NoModifier, rect.center());
        QTRY_VERIFY(!window.waveNeedsRetry_);
        QTRY_VERIFY(window.waveQueue_->find(window.selected_) &&
                    window.waveQueue_->find(window.selected_)->complete);
        QVERIFY(window.waveStatus_->text().contains("版本已变化"));
    }
    void rapidSelectionAndSearchKeepNewestResult() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"A.wav", "B.wav", "Final.wav"}));
        QVERIFY(writeWav(data.filePath("A.wav"), 48000));
        QVERIFY(writeWav(data.filePath("B.wav"), 96000));
        QVERIFY(writeWav(data.filePath("Final.wav"), 144000));
        Window window(data.path());
        window.show();
        window.volume_->setValue(0);
        QTRY_COMPARE(window.model_->rowCount(), 3);
        for (int i = 0; i < 30; ++i) {
            window.table_->setCurrentIndex(window.model_->index(i % 3, 1));
            QTest::qWait(15);
        }
        QTRY_COMPARE(window.duration_, 3000);
        QCOMPARE(window.selected_.name, QString("Final.wav"));
        QCOMPARE(window.duration_, 3000);
        QVERIFY(!window.waveNeedsRetry_);
        for (const auto &query : {"A", "missing", "B", "Final"})
            window.search_->setText(query);
        QTRY_VERIFY(window.model_->rowCount() == 1 && window.model_->tracks[0].name == "Final.wav");
    }
};
int main(int argc, char **argv) {
    if (argc > 2 && QByteArray(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        if (QByteArray(argv[2]) == "waves" && qEnvironmentVariableIsSet("AUDIO_TEST_STALL_WAVE")) {
            QThread::sleep(30);
            return 1;
        }
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    WindowTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "window_tests.moc"
