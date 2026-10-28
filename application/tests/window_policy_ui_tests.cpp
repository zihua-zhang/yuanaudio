#include "window.h"
#include "workers.h"
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QHelpEvent>
#include <QJsonDocument>
#include <QLineEdit>
#include <QPainter>
#include <QTemporaryDir>
#include <QToolTip>
#include <QtTest>

class WindowPolicyUiTests : public QObject {
    Q_OBJECT
  private slots:
    void reloadingLibrariesRestoresVisibleWaveformsAfterCacheAttachment() {
        QTemporaryDir data;
        Track track;
        {
            Index db(QDir(data.path()).filePath("library.sqlite"));
            QVERIFY(db.open());
            QVERIFY(db.saveLibrary({"qa", "Music", data.path(), ""}));
            track.libraryId = "qa";
            track.relativePath = track.name = "saved.wav";
            track.folder = ".";
            track.size = 100;
            track.modified = 100;
            QVERIFY(db.upsert({track}));
            track = db.search("").tracks.first();
        }
        PeakBuilder builder(8000, 1);
        const float sample[] = {-.5f, .5f, 0};
        builder.append(sample, 3);
        QVERIFY(saveWaveView(QDir(data.path()).filePath("peaks"), WaveQueue::key(track), builder.finish()));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.waveQueue_->find(track) && window.waveQueue_->find(track)->complete);
        window.waveQueue_->refreshIndex();
        QVERIFY(!window.waveQueue_->find(track));
        window.reloadLibraries();
        QTRY_VERIFY(window.waveQueue_->find(track) && window.waveQueue_->find(track)->complete);
    }
    void noWatchdogWhileIdleAndLoadedPauseDoesNotKillPlayer() {
        QTemporaryDir data;
        Window window(data.path());
        QTRY_VERIFY(window.indexReady_);
        QVERIFY(!window.playerWatchdog_.isActive());
        window.playerLoading_ = true;
        window.playerWatchdog_.start();
        window.handlePlayer({{"event", "ready"}, {"generation", 0}});
        QVERIFY(!window.playerWatchdog_.isActive());
        QVERIFY(!window.playerLoading_);
        window.handlePlayer({{"event", "state"}, {"generation", 0}, {"playing", true}});
        QVERIFY(window.playerWatchdog_.isActive());
        window.handlePlayer({{"event", "state"}, {"generation", 0}, {"playing", false}});
        QVERIFY(!window.playerWatchdog_.isActive());
        QVERIFY(!window.loadTimer_.isActive());
    }
    void scanPolicyPausesForPreviewAndResumesWithExplicitOverride() {
        QTemporaryDir data;
        Window window(data.path());
        QTRY_VERIFY(window.indexReady_);
        window.scanControlPath_ = QDir(data.path()).filePath("control.json");
        window.isPlaying_ = true;
        QVERIFY(window.writeScanControl());
        auto read = [&] {
            QFile file(window.scanControlPath_);
            file.open(QIODevice::ReadOnly);
            return QJsonDocument::fromJson(file.readAll()).object();
        };
        QVERIFY(read()["paused"].toBool());
        QCOMPARE(read()["entriesPerSecond"].toInt(), 400);
        window.editingPriority_->setChecked(false);
        QVERIFY(!read()["paused"].toBool());
        QCOMPARE(read()["entriesPerSecond"].toInt(), 0);
        window.isPlaying_ = false;
        window.editingPriority_->setChecked(true);
        QVERIFY(!read()["paused"].toBool());
    }
    void waveformHoverUsesSameInsetAndBoundsAsClick() {
        WaveQueue queue("unused-test-cache");
        TrackModel model;
        Track track;
        track.id = 42;
        track.duration = 120000;
        track.name = "音乐.wav";
        model.replace({track});
        TrackTable table;
        table.setModel(&model);
        RowWaveDelegate delegate(&queue, &table);
        QStyleOptionViewItem option;
        option.rect = QRect(0, 0, 216, 52);
        QHelpEvent hover(QEvent::ToolTip, QPoint(108, 26), QPoint(108, 26));
        QVERIFY(delegate.helpEvent(&hover, &table, option, model.index(0, TrackModel::Wave)));
        QVERIFY(QToolTip::text().startsWith("1:00 / 2:00"));
        QHelpEvent end(QEvent::ToolTip, QPoint(230, 26), QPoint(230, 26));
        QVERIFY(delegate.helpEvent(&end, &table, option, model.index(0, TrackModel::Wave)));
        QVERIFY(QToolTip::text().startsWith("2:00 / 2:00"));
        QToolTip::hideText();
    }
    void musicNameHighlightPaintsOnlyLiteralMatchingText() {
        QTemporaryDir data;
        Window window(data.path());
        QTRY_VERIFY(window.indexReady_);
        window.search_->setText("回忆");
        window.debounce_.stop();
        Track track;
        track.name = "<b>回忆</b>.wav";
        track.folder = "剧情";
        window.model_->replace({track});
        QImage image(340, 52, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        QStyleOptionViewItem option;
        option.rect = image.rect();
        option.font = window.font();
        window.table_->itemDelegateForColumn(TrackModel::Name)
            ->paint(&painter, option, window.model_->index(0, TrackModel::Name));
        painter.end();
        int yellow = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                yellow += image.pixelColor(x, y) == QColor("#fff0bb");
        QVERIFY(yellow > 10);
        QCOMPARE(window.model_->tracks[0].name, QString("<b>回忆</b>.wav"));
    }
};
int main(int argc, char **argv) {
    if (argc > 2 && QByteArray(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    WindowPolicyUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "window_policy_ui_tests.moc"
