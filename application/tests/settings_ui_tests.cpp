#include "window.h"
#include "theme.h"
#include "ui_controls.h"
#include "workers.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSignalSpy>
#include <QSlider>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>
#include <algorithm>
#include <cmath>

class SettingsUiTests : public QObject {
    Q_OBJECT

    static bool seedCachedCatalog(const QString &root, int count = 32) {
        Index db(QDir(root).filePath("library.sqlite"));
        if (!db.open() || !db.saveLibrary({"settings-qa", "设置测试", root, ""}))
            return false;
        QList<Track> tracks;
        for (int i = 0; i < count; ++i) {
            Track track;
            track.libraryId = "settings-qa";
            track.relativePath = track.name = QString("music-%1.wav").arg(i, 3, 10, QChar('0'));
            track.folder = ".";
            track.duration = 1000;
            track.size = 2044;
            track.modified = 1000;
            tracks.append(track);
        }
        if (!db.upsert(tracks))
            return false;
        PeakBuilder builder(1000, 1);
        const float samples[1000] = {.25f, -.25f};
        builder.append(samples, 1000);
        const auto wave = builder.finish();
        for (const auto &track : db.tracksAfter(0, count))
            if (!saveWaveView(QDir(root).filePath("peaks"), WaveQueue::key(track), wave))
                return false;
        return true;
    }

    static bool fullyInsideWindow(QWidget *widget, QWidget *window) {
        if (!widget || !widget->isVisible() || widget->width() <= 0 || widget->height() <= 0)
            return false;
        for (auto *ancestor = widget->parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
            const QRect rect(widget->mapTo(ancestor, QPoint()), widget->size());
            if (!ancestor->rect().contains(rect))
                return false;
            if (ancestor == window)
                return true;
        }
        return false;
    }

    static bool seedAppearanceCatalog(const QString &root) {
        Index db(QDir(root).filePath("library.sqlite"));
        if (!db.open() || !db.saveLibrary({"settings-qa", "短剧素材库", root, ""}))
            return false;
        const QStringList names{
            "01 微光入场 · 温暖钢琴.wav", "02 未说完的话 · 细腻回忆.flac",
            "03 长街夜雨 · 悬疑铺垫.wav", "04 心跳之后 · 情绪递进.mp3",
            "05 初见时分 · 轻盈弦乐.wav", "06 暗流涌动 · 紧张氛围.wav",
            "07 春山来信 · 古风叙事.flac", "08 转身以后 · 钢琴独奏.wav",
            "09 命运交错 · 管弦推进.wav", "10 屋檐下的晴天 · 日常轻喜.mp3",
            "11 时间的褶皱 · 记忆闪回.wav", "12 安静的告别 · 弦乐尾声.flac",
            "13 新的起点 · 片尾配乐.wav", "14 暗夜追逐 · 动作节奏.wav",
            "15 小小的期待 · 木吉他.mp3", "16 山海之间 · 宽阔氛围.wav"};
        const QStringList folders{"短剧配乐/情绪与回忆", "短剧配乐/悬疑与张力",
                                  "短剧配乐/古风与叙事", "短剧配乐/日常与转场"};
        QList<Track> tracks;
        for (int i = 0; i < names.size(); ++i) {
            Track track;
            track.libraryId = "settings-qa";
            track.folder = folders[(i / 4) % folders.size()];
            track.name = names[i];
            track.relativePath = track.folder + '/' + track.name;
            track.size = 8 * 1024 * 1024 + i * 4096;
            track.modified = 1700000000000LL + i * 1000;
            tracks.append(track);
        }
        if (!db.upsert(tracks))
            return false;
        const auto indexed = db.searchAll("").tracks;
        for (int i = 0; i < indexed.size(); ++i) {
            const auto &track = indexed[i];
            const qint64 duration = (82 + (i * 23) % 159) * 1000;
            WaveData wave;
            wave.sampleRate = 48000;
            wave.channels = 2;
            wave.frames = duration * 48;
            wave.stride = 8192;
            const int buckets = int((wave.frames + wave.stride - 1) / wave.stride);
            wave.buckets.reserve(buckets);
            // Synthetic envelopes only: no audio files, decoder or server access.
            for (int sample = 0; sample < buckets; ++sample) {
                const double progress = double(sample) / qMax(1, buckets - 1);
                const double fade = qMin(1.0, progress * 18.0) * qMin(1.0, (1.0 - progress) * 13.0);
                const double phrase = .32 + .2 * std::sin(progress * (12 + i % 5) + i);
                const double pulse = .09 + .11 * std::abs(std::sin(sample * (.39 + i * .021)));
                const float amplitude = float(qBound(.015, (phrase + pulse) * fade, .88));
                wave.buckets.append(QVector<Peak>{Peak{-amplitude * .92f, amplitude},
                                                  Peak{-amplitude * .78f, amplitude * .86f}});
            }
            if (!saveWaveView(QDir(root).filePath("peaks"), WaveQueue::key(track), wave, i == 5) ||
                !db.setDuration(track, duration) ||
                !db.setPersonal(track, i == 1 || i == 4, i == 1 ? "回忆 女主 温柔" : ""))
                return false;
        }
        return true;
    }

  private slots:
    void seekBarCommitsPointerPositionOnlyOnRelease() {
        SeekSlider slider(Qt::Horizontal);
        slider.setRange(0, 10000);
        slider.setSingleStep(100);
        slider.setTracking(false);
        slider.setValue(1000);
        slider.resize(440, 32);
        slider.show();
        QSignalSpy commits(&slider, &QSlider::valueChanged);
        QSignalSpy releases(&slider, &QSlider::sliderReleased);
        const auto point = [&slider](double fraction) {
            return QPoint(int(slider.width() * fraction), slider.height() / 2);
        };
        QTest::mousePress(&slider, Qt::LeftButton, Qt::NoModifier, point(.75));
        QVERIFY(slider.isSliderDown());
        QCOMPARE(slider.value(), 1000);
        QVERIFY(slider.sliderPosition() > 6500);
        QCOMPARE(commits.size(), 0);
        QTest::mouseMove(&slider, point(.5));
        QVERIFY(slider.sliderPosition() > 4000 && slider.sliderPosition() < 6000);
        QCOMPARE(slider.value(), 1000);
        QCOMPARE(commits.size(), 0);
        QTest::mouseRelease(&slider, Qt::LeftButton, Qt::NoModifier, point(.5));
        QVERIFY(!slider.isSliderDown());
        QCOMPARE(commits.size(), 1);
        QCOMPARE(releases.size(), 1);
        QCOMPARE(slider.value(), slider.sliderPosition());
        QVERIFY(slider.value() > 4000 && slider.value() < 6000);
        const int committed = slider.value();
        QTest::mousePress(&slider, Qt::LeftButton, Qt::NoModifier, point(.2));
        QCOMPARE(slider.value(), committed);
        QCOMPARE(commits.size(), 1);
        QTest::mouseRelease(&slider, Qt::LeftButton, Qt::NoModifier, point(.2));
        QCOMPARE(commits.size(), 2);
        QVERIFY(slider.value() > 1000 && slider.value() < 3000);
        const int beforeKey = slider.value();
        QTest::keyClick(&slider, Qt::Key_Right);
        QCOMPARE(slider.value(), beforeKey + 100);
        QCOMPARE(commits.size(), 3);
    }

    void playbackReportsDoNotIssueSeekOrOverrideActiveDrag() {
        QTemporaryDir data;
        QVERIFY(seedCachedCatalog(data.path()));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_ && window.model_->rowCount() == 32);
        QVERIFY(window.seek_);
        QVERIFY(!window.seek_->isEnabled());
        window.selected_ = window.model_->tracks[0];
        window.selectionGeneration_ = 19;
        QSignalSpy commits(window.seek_, &QSlider::valueChanged);
        window.handlePlayer({{"event", "position"}, {"generation", 19},
                             {"position", 25000}, {"duration", 100000}});
        QVERIFY(window.seek_->isEnabled());
        QCOMPARE(window.seek_->value(), 2500);
        QCOMPARE(commits.size(), 0);
        QVERIFY(!window.player_);
        window.seek_->setSliderDown(true);
        window.seek_->setSliderPosition(7500);
        window.handlePlayer({{"event", "position"}, {"generation", 19},
                             {"position", 30000}, {"duration", 100000}});
        QCOMPARE(window.seek_->sliderPosition(), 7500);
        QCOMPARE(commits.size(), 0);
        {
            QSignalBlocker programmatic(window.seek_);
            window.seek_->setSliderDown(false);
        }
        window.handlePlayer({{"event", "position"}, {"generation", 19},
                             {"position", 32000}, {"duration", 100000}});
        QCOMPARE(window.seek_->value(), 3200);
        QCOMPARE(commits.size(), 0);
        QVERIFY(!window.player_);
        window.clearSelectedTrack();
        QCOMPARE(window.seek_->value(), 0);
        QVERIFY(!window.seek_->isEnabled());
        QCOMPARE(commits.size(), 0);
    }

    void openingAndNavigatingSettingsPreservesListAndPreviewState() {
        QTemporaryDir data;
        QVERIFY(seedCachedCatalog(data.path()));
        Window window(data.path());
        window.resize(980, 640);
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_ && window.model_->rowCount() == 32);
        {
            QSignalBlocker blocker(window.table_->selectionModel());
            window.table_->setCurrentIndex(window.model_->index(20, TrackModel::Name));
        }
        window.table_->verticalScrollBar()->setValue(12);
        window.selected_ = window.model_->tracks[20];
        window.selectionGeneration_ = 37;
        window.position_ = 321;
        window.duration_ = 1000;
        window.isPlaying_ = true;
        window.updateWorkPolicy();
        const auto row = QPersistentModelIndex(window.table_->currentIndex());
        const auto trackId = window.selected_.id;
        const int scroll = window.table_->verticalScrollBar()->value();
        const QRect tableGeometry = window.table_->geometry();
        const auto queryGeneration = window.queryGeneration_;
        QSignalSpy resets(window.model_, &QAbstractItemModel::modelReset);

        auto *open = window.findChild<QPushButton *>("openSettings");
        QVERIFY(open);
        QTest::mouseClick(open, Qt::LeftButton);
        QVERIFY(window.settingsDialog_);
        QPointer<QDialog> dialog(window.settingsDialog_);
        QTRY_VERIFY(dialog->isVisible());
        QVERIFY(!dialog->isModal());
        QCOMPARE(dialog->windowModality(), Qt::NonModal);
        QVERIFY(!dialog->testAttribute(Qt::WA_DeleteOnClose));
        QCOMPARE(window.settingsPages_->count(), 6);
        QCOMPARE(window.settingsNavigation_->count(), 6);
        const QStringList pageNames{"播放与浏览", "波形缓存", "性能", "显示", "素材库", "快捷键"};
        for (int page = 0; page < pageNames.size(); ++page) {
            QCOMPARE(window.settingsNavigation_->item(page)->text(), pageNames[page]);
            window.settingsNavigation_->setCurrentRow(page);
            QCOMPARE(window.settingsPages_->currentIndex(), page);
        }
        QVERIFY(window.settingsPages_->widget(0)->isAncestorOf(window.autoPlay_));
        QVERIFY(window.settingsPages_->widget(2)->isAncestorOf(window.editingPriority_));
        QTest::keyClick(dialog, Qt::Key_Escape);
        QTRY_VERIFY(!dialog->isVisible());
        QVERIFY(dialog);
        QVERIFY(window.isVisible());
        QCOMPARE(window.table_->geometry(), tableGeometry);
        QCOMPARE(window.table_->verticalScrollBar()->value(), scroll);
        QCOMPARE(window.table_->currentIndex(), QModelIndex(row));
        QVERIFY(row.isValid());
        QCOMPARE(window.selected_.id, trackId);
        QCOMPARE(window.selectionGeneration_, quint64(37));
        QCOMPARE(window.position_, qint64(321));
        QCOMPARE(window.duration_, qint64(1000));
        QVERIFY(window.isPlaying_);
        QCOMPARE(window.queryGeneration_, queryGeneration);
        QCOMPARE(resets.size(), 0);
        window.showSettings(3);
        QCOMPARE(window.settingsDialog_, dialog.data());
        QCOMPARE(window.settingsPages_->currentIndex(), 3);
        dialog->close();
    }

    void cacheStatusOpensDedicatedPageWithoutResizingMainList() {
        QTemporaryDir data;
        Window window(data.path());
        window.resize(980, 640);
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        auto *status = window.findChild<QPushButton *>("waveCacheToggle");
        auto *panel = window.findChild<QWidget *>("cachePanel");
        QVERIFY(status && panel);
        QVERIFY(!status->isCheckable());
        QCOMPARE(status, window.cacheStatusButton_);
        QCOMPARE(panel->window(), window.settingsDialog_);
        QVERIFY(!panel->isVisible());
        const QRect listGeometry = window.table_->geometry();
        QTest::mouseClick(status, Qt::LeftButton);
        QTRY_VERIFY(window.settingsDialog_->isVisible());
        QCOMPARE(window.settingsPages_->currentIndex(), 1);
        QVERIFY(panel->isVisible());
        QVERIFY(window.settingsPages_->widget(1)->isAncestorOf(panel) ||
                window.settingsPages_->widget(1) == panel);
        QCOMPARE(window.table_->geometry(), listGeometry);
        for (const auto *name :
             {"prepareAllWaves", "pauseWavePreparation", "skipWavePreparation", "wavePreparationFailures",
              "reanalyzeReferenceWaves", "sharedCacheSettings"}) {
            auto *button = panel->findChild<QPushButton *>(name);
            QVERIFY2(button, name);
            QVERIFY2(button->isVisible(), name);
        }
        window.settingsDialog_->close();
        QTRY_VERIFY(!window.settingsDialog_->isVisible());
        QVERIFY(!panel->isVisible());
        QCOMPARE(window.table_->geometry(), listGeometry);
    }

    void displayAndPlaybackPreferencesApplyImmediatelyAndSurviveRestart() {
        QTemporaryDir data;
        {
            Window window(data.path());
            window.show();
            QTRY_VERIFY(window.indexReady_ && !window.loading_);
            window.showSettings(3);
            QVERIFY(window.folderIndentation_ && window.listDensity_ && window.preparationSpeed_);
            QCOMPARE(window.folderIndentation_->objectName(), QString("folderIndentation"));
            QCOMPARE(window.listDensity_->objectName(), QString("listDensity"));
            QCOMPARE(window.preparationSpeed_->objectName(), QString("backgroundReadRate"));
            QCOMPARE(window.folderIndentation_->currentData().toInt(), 12);
            QCOMPARE(window.tree_->indentation(), 12);
            QCOMPARE(window.listDensity_->currentData().toInt(), 52);
            QCOMPARE(window.table_->verticalHeader()->defaultSectionSize(), 52);
            QCOMPARE(window.preparationSpeed_->currentData().toLongLong(), qint64(1048576));
            for (const auto value : {8, 12, 16})
                QVERIFY(window.folderIndentation_->findData(value) >= 0);
            for (const auto value : {44, 52, 60})
                QVERIFY(window.listDensity_->findData(value) >= 0);
            const int tight = window.folderIndentation_->findData(8);
            const int compact = window.listDensity_->findData(44);
            const int faster = window.preparationSpeed_->findData(10 * 1048576);
            QVERIFY(faster >= 0);
            window.folderIndentation_->setCurrentIndex(tight);
            window.listDensity_->setCurrentIndex(compact);
            window.preparationSpeed_->setCurrentIndex(faster);
            window.autoPlay_->setChecked(true);
            window.editingPriority_->setChecked(false);
            window.volume_->setValue(27);
            QCOMPARE(window.tree_->indentation(), 8);
            QCOMPARE(window.table_->verticalHeader()->defaultSectionSize(), 44);
            window.settingsDialog_->close();
            QSettings saved(data.filePath("browser.ini"), QSettings::IniFormat);
            saved.sync();
            QCOMPARE(saved.value("folderIndentation").toInt(), 8);
            QCOMPARE(saved.value("rowHeight").toInt(), 44);
            QCOMPARE(saved.value("backgroundReadRate").toLongLong(), qint64(10 * 1048576));
            QCOMPARE(saved.value("autoPlay").toBool(), true);
            QCOMPARE(saved.value("editingPriority").toBool(), false);
            QCOMPARE(saved.value("volume").toInt(), 27);
        }
        Window reopened(data.path());
        QTRY_VERIFY(reopened.indexReady_ && !reopened.loading_);
        QCOMPARE(reopened.folderIndentation_->currentData().toInt(), 8);
        QCOMPARE(reopened.tree_->indentation(), 8);
        QCOMPARE(reopened.listDensity_->currentData().toInt(), 44);
        QCOMPARE(reopened.table_->verticalHeader()->defaultSectionSize(), 44);
        QCOMPARE(reopened.preparationSpeed_->currentData().toLongLong(), qint64(10 * 1048576));
        QVERIFY(reopened.autoPlay_->isChecked());
        QVERIFY(!reopened.editingPriority_->isChecked());
        QCOMPARE(reopened.volume_->value(), 27);
        QVERIFY(!reopened.settingsDialog_->isVisible());
    }

    void unlimitedAndParallelPreferencesSurviveRestart() {
        QTemporaryDir data;
        {
            Window window(data.path());
            QTRY_VERIFY(window.indexReady_ && !window.loading_);
            auto *speed = window.preparationSpeed_;
            const int unlimited = speed->findData(0);
            QVERIFY(unlimited >= 0);
            auto *parallel = window.settingsDialog_->findChild<QComboBox *>("backgroundConcurrency");
            QVERIFY(parallel);
            QCOMPARE(parallel->currentData().toInt(), 1);
            QVERIFY(parallel->findData(2) >= 0);
            QVERIFY(parallel->findData(4) >= 0);
            speed->setCurrentIndex(unlimited);
            parallel->setCurrentIndex(parallel->findData(4));
            QCOMPARE(window.waveQueue_->preparation()["concurrency"].toInt(), 4);
            QVERIFY(window.editingPriority_->isChecked());
            window.show();
            window.showSettings(2);
            window.settingsDialog_->resize(880, 640);
            auto *page = qobject_cast<QScrollArea *>(window.settingsPages_->currentWidget());
            QVERIFY(page);
            page->ensureWidgetVisible(parallel);
            QCoreApplication::processEvents();
            QVERIFY(fullyInsideWindow(parallel, window.settingsDialog_));
            QVERIFY(fullyInsideWindow(speed, window.settingsDialog_));
            const auto captures = qEnvironmentVariable("YUAUDIO_SCREENSHOT_DIR");
            if (!captures.isEmpty()) {
                QDir().mkpath(captures);
                for (const auto &mode : {QString("light"), QString("dark")}) {
                    window.appearanceMode_->setCurrentIndex(window.appearanceMode_->findData(mode));
                    QCoreApplication::processEvents();
                    QVERIFY(window.settingsDialog_->grab().save(
                        QDir(captures).filePath("performance-" + mode + ".png")));
                }
            }
        }
        Window reopened(data.path());
        QTRY_VERIFY(reopened.indexReady_ && !reopened.loading_);
        QCOMPARE(reopened.preparationSpeed_->currentData().toLongLong(), qint64(0));
        auto *parallel = reopened.settingsDialog_->findChild<QComboBox *>("backgroundConcurrency");
        QVERIFY(parallel);
        QCOMPARE(parallel->currentData().toInt(), 4);
        QCOMPARE(reopened.waveQueue_->preparation()["concurrency"].toInt(), 4);
    }

    void appearanceChangesImmediatelyAndPersistsWithoutResettingMusic() {
        QTemporaryDir data;
        QVERIFY(seedAppearanceCatalog(data.path()));
        {
            Window window(data.path());
            window.resize(1280, 800);
            window.show();
            QTRY_VERIFY(window.indexReady_ && !window.loading_ && window.model_->rowCount() == 16);
            window.locateFolder("settings-qa", "短剧配乐");
            QTRY_COMPARE(window.scopeFolder_, QString("短剧配乐"));
            window.search_->setText("配乐");
            QTRY_VERIFY(!window.loading_ && !window.debounce_.isActive() && window.model_->rowCount() == 16);
            window.waveQueue_->setVisible(window.model_->tracks);
            QTRY_VERIFY(std::all_of(window.model_->tracks.cbegin(), window.model_->tracks.cend(),
                                   [&window](const Track &track) {
                                       const auto *wave = window.waveQueue_->find(track);
                                       return wave && wave->complete && wave->data.buckets.size() > 64;
                                   }));
            {
                // Give the real controls a stable playing state without starting a decoder.
                QSignalBlocker selection(window.table_->selectionModel());
                window.table_->selectRow(1);
                window.table_->setCurrentIndex(window.model_->index(1, TrackModel::Name));
            }
            window.selected_ = window.model_->tracks[1];
            window.selectionGeneration_ = 73;
            window.position_ = 37000;
            window.duration_ = window.selected_.duration;
            window.isPlaying_ = true;
            window.title_->setText(window.selected_.name);
            window.detail_->setText(window.selected_.absolutePath());
            window.tags_->setText(window.selected_.tags);
            window.favorite_->setText("★ 已收藏");
            window.favorite_->setEnabled(true);
            window.tags_->setEnabled(true);
            window.play_->setText("Ⅱ 暂停");
            window.play_->setEnabled(true);
            window.time_->setText(QString("0:37 / %1:%2")
                                      .arg(window.duration_ / 60000)
                                      .arg((window.duration_ / 1000) % 60, 2, 10, QChar('0')));
            window.rowWave_->playingKey = WaveQueue::key(window.selected_);
            window.rowWave_->position = window.position_;
            window.rowWave_->duration = window.duration_;
            {
                QSignalBlocker programmatic(window.seek_);
                window.seek_->setValue(int(window.position_ * 10000 / window.duration_));
                window.seek_->setEnabled(true);
            }
            window.waveStatus_->setText("本机波形已就绪");
            window.updateWorkPolicy();
            QVERIFY(window.appearanceMode_);
            QCOMPARE(window.appearanceMode_->currentData().toString(), QString("system"));
            const Track selected = window.selected_;
            const auto selectedIndex = QPersistentModelIndex(window.table_->currentIndex());
            const auto scope = window.scopeLibrary_;
            const auto folder = window.scopeFolder_;
            const auto query = window.search_->text();
            const auto queryGeneration = window.queryGeneration_;
            const auto playingKey = window.rowWave_->playingKey;
            const auto position = window.position_, duration = window.duration_;
            const int seekPosition = window.seek_->value();
            QSignalSpy resets(window.model_, &QAbstractItemModel::modelReset);
            QSignalSpy seekCommands(window.seek_, &QSlider::valueChanged);
            const auto captures = qEnvironmentVariable("YUAUDIO_SCREENSHOT_DIR");
            if (!captures.isEmpty())
                QVERIFY(QDir().mkpath(captures));
            for (const auto &mode : {QString("light"), QString("dark")}) {
                const int at = window.appearanceMode_->findData(mode);
                QVERIFY(at >= 0);
                window.appearanceMode_->setCurrentIndex(at);
                const bool dark = mode == "dark";
                QCOMPARE(qApp->property("sourceAudioDarkMode").toBool(), dark);
                QCOMPARE(qApp->palette().color(QPalette::Window),
                         dark ? theme::dark().window : theme::light().window);
                QVERIFY(!qApp->styleSheet().contains("@{"));
                for (const auto size : {QSize(1280, 800), QSize(1000, 700)}) {
                    window.settingsDialog_->hide();
                    window.resize(size);
                    QTRY_COMPARE(window.size(), size);
                    QCoreApplication::processEvents();
                    QVERIFY(fullyInsideWindow(window.search_, &window));
                    QVERIFY(fullyInsideWindow(window.tree_, &window));
                    QVERIFY(fullyInsideWindow(window.table_, &window));
                    QVERIFY(fullyInsideWindow(window.settingsButton_, &window));
                    QVERIFY(fullyInsideWindow(window.play_, &window));
                    QVERIFY(fullyInsideWindow(window.seek_, &window));
                    QVERIFY(fullyInsideWindow(window.volume_, &window));
                    QVERIFY(window.table_->columnWidth(TrackModel::Wave) >= 140);
                    QVERIFY(window.table_->viewport()->height() >= 200);
                    const auto *selectedWave = window.waveQueue_->find(selected);
                    QVERIFY(selectedWave && selectedWave->complete && !selectedWave->failed);
                    QVERIFY(!window.player_);
                    QVERIFY(window.waveQueue_->findChildren<QProcess *>().isEmpty());
                    const auto suffix = QString("-%1x%2.png").arg(size.width()).arg(size.height());
                    if (!captures.isEmpty())
                        QVERIFY(window.grab().save(QDir(captures).filePath("main-" + mode + suffix)));

                    window.showSettings(3);
                    window.settingsDialog_->resize(size);
                    QTRY_COMPARE(window.settingsDialog_->size(), size);
                    auto *page = qobject_cast<QScrollArea *>(window.settingsPages_->currentWidget());
                    QVERIFY(page);
                    page->ensureWidgetVisible(window.appearanceMode_);
                    QCoreApplication::processEvents();
                    QVERIFY(fullyInsideWindow(window.settingsNavigation_, window.settingsDialog_));
                    QVERIFY(fullyInsideWindow(window.appearanceMode_, window.settingsDialog_));
                    QVERIFY(fullyInsideWindow(window.settingsDialog_->findChild<QPushButton *>("closeSettings"),
                                             window.settingsDialog_));
                    if (!captures.isEmpty())
                        QVERIFY(window.settingsDialog_->grab().save(
                            QDir(captures).filePath("settings-" + mode + suffix)));
                    window.settingsDialog_->close();

                    QCOMPARE(window.scopeLibrary_, scope);
                    QCOMPARE(window.scopeFolder_, folder);
                    QCOMPARE(window.search_->text(), query);
                    QCOMPARE(window.selected_.id, selected.id);
                    QCOMPARE(window.selected_.tags, selected.tags);
                    QCOMPARE(window.table_->currentIndex(), QModelIndex(selectedIndex));
                    QCOMPARE(window.selectionGeneration_, quint64(73));
                    QCOMPARE(window.position_, position);
                    QCOMPARE(window.duration_, duration);
                    QCOMPARE(window.rowWave_->playingKey, playingKey);
                    QCOMPARE(window.seek_->value(), seekPosition);
                    QCOMPARE(seekCommands.size(), 0);
                    QVERIFY(window.isPlaying_);
                    QCOMPARE(window.queryGeneration_, queryGeneration);
                    QCOMPARE(resets.size(), 0);
                }
            }
            QSettings saved(data.filePath("browser.ini"), QSettings::IniFormat);
            saved.sync();
            QCOMPARE(saved.value("appearanceMode").toString(), QString("dark"));
        }
        Window reopened(data.path());
        QCOMPARE(reopened.appearanceMode_->currentData().toString(), QString("dark"));
        QVERIFY(qApp->property("sourceAudioDarkMode").toBool());
        QCOMPARE(qApp->palette().color(QPalette::Window), theme::dark().window);
    }

    void multipleLibrariesCanBeManagedWithoutDeletingOriginals() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        const auto firstRoot = data.filePath("one");
        const auto secondRoot = data.filePath("two");
        QVERIFY(QDir().mkpath(firstRoot + "/剧情"));
        QVERIFY(QDir().mkpath(secondRoot));
        QFile original(firstRoot + "/剧情/开场.wav");
        QVERIFY(original.open(QIODevice::WriteOnly));
        original.write("unchanged original");
        original.close();
        Index db(data.filePath("library.sqlite"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"one", "剧集配乐", firstRoot, ""}));
        QVERIFY(db.saveLibrary({"two", "通用音效", secondRoot, ""}));
        Track first;
        first.libraryId = "one";
        first.relativePath = "剧情/开场.wav";
        first.name = "开场.wav";
        first.folder = "剧情";
        first.root = firstRoot;
        first.size = original.size();
        first.modified = 1000;
        Track second = first;
        second.libraryId = "two";
        second.relativePath = second.name = "结尾.wav";
        second.folder = ".";
        second.root = secondRoot;
        QVERIFY(db.upsert({first, second}));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_ && window.model_->rowCount() == 2);
        window.showSettings(4);
        QVERIFY(window.libraryList_ && window.removeLibraryButton_ && window.excludedList_);
        QCOMPARE(window.libraryList_->count(), 2);
        QTRY_VERIFY(window.libraryList_->item(0)->text().contains("首"));
        window.requestCatalogRemoval(Window::RemovalKind::Track, "one", first.relativePath);
        QTRY_VERIFY(!window.catalogBusy_);
        QTRY_COMPARE(db.searchAll("").tracks.size(), 1);
        QVERIFY(QFileInfo::exists(original.fileName()));
        QCOMPARE(db.excludedItems("one").size(), 1);
        window.libraryList_->setCurrentRow(0);
        QTRY_COMPARE(window.excludedList_->count(), 1);
        QCOMPARE(window.excludedList_->item(0)->data(Qt::UserRole).toString(), first.relativePath);
        window.locateFolder("one", "剧情");
        QTRY_COMPARE(window.scopeFolder_, QString("剧情"));
        window.requestCatalogRemoval(Window::RemovalKind::Folder, "one", "剧情");
        QTRY_VERIFY(!window.catalogBusy_);
        QTRY_VERIFY(window.scopeLibrary_.isEmpty() && window.scopeFolder_.isEmpty());
        QCOMPARE(db.excludedItems("one").size(), 2);
        QVERIFY(QFileInfo::exists(original.fileName()));
        window.requestCatalogRemoval(Window::RemovalKind::Library, "one");
        QTRY_VERIFY(!window.catalogBusy_);
        QTRY_COMPARE(window.libraryList_->count(), 1);
        QCOMPARE(window.libraryList_->item(0)->data(Qt::UserRole).toString(), QString("two"));
        QVERIFY(QFileInfo::exists(original.fileName()));
        QCOMPARE(db.libraries().size(), 1);
        QTRY_COMPARE(window.model_->rowCount(), 1);
    }

    void preferenceSwitchesAcceptPointerAndKeyboardWithoutStartingPreview() {
        QTemporaryDir data;
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        QSignalSpy playClicks(window.play_, &QPushButton::clicked);
        for (const auto page : {0, 2}) {
            window.showSettings(page);
            auto *control = page == 0 ? window.autoPlay_ : window.editingPriority_;
            auto *scroll = qobject_cast<QScrollArea *>(window.settingsPages_->currentWidget());
            QVERIFY(scroll);
            scroll->ensureWidgetVisible(control);
            QVERIFY(fullyInsideWindow(control, window.settingsDialog_));
            QVERIFY(control->focusPolicy() & Qt::TabFocus);
            QVERIFY(!control->accessibleName().isEmpty());
            const bool original = control->isChecked();
            QSignalSpy toggles(control, &QCheckBox::toggled);
            // Both ends of the painted switch belong to the interactive hit area.
            QTest::mouseClick(control, Qt::LeftButton, Qt::NoModifier, QPoint(2, control->height() / 2));
            QCOMPARE(control->isChecked(), !original);
            QCOMPARE(toggles.size(), 1);
            QTest::mouseClick(control, Qt::LeftButton, Qt::NoModifier,
                              QPoint(control->width() - 3, control->height() / 2));
            QCOMPARE(control->isChecked(), original);
            QCOMPARE(toggles.size(), 2);
            control->setFocus();
            QTest::keyClick(control, Qt::Key_Space);
            QCOMPARE(control->isChecked(), !original);
            QCOMPARE(toggles.size(), 3);
            control->setEnabled(false);
            QTest::mouseClick(control, Qt::LeftButton);
            QTest::keyClick(control, Qt::Key_Space);
            QCOMPARE(control->isChecked(), !original);
            QCOMPARE(toggles.size(), 3);
            control->setEnabled(true);
            control->setChecked(original);
        }
        QCOMPARE(playClicks.size(), 0);
        QVERIFY(!window.player_);
        window.settingsDialog_->close();
    }

    void closingSettingsKeepsStartedPreparationIntentAndOnlyExplicitPauseStopsIt() {
        QTemporaryDir data;
        QVERIFY(seedCachedCatalog(data.path()));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_ && window.model_->rowCount() == 32);
        QTRY_VERIFY(window.waveQueue_->preparation()["failuresReady"].toBool());
        // A playing preview holds the synthetic preparation at a deterministic point.
        // No decoder, source music, network share, or timing-dependent large file is needed.
        window.isPlaying_ = true;
        window.updateWorkPolicy();
        window.showSettings(1);
        auto *prepare = window.settingsDialog_->findChild<QPushButton *>("prepareAllWaves");
        auto *pause = window.settingsDialog_->findChild<QPushButton *>("pauseWavePreparation");
        QVERIFY(prepare && pause);
        QTest::mouseClick(prepare, Qt::LeftButton);
        QTRY_VERIFY(window.waveQueue_->preparation()["running"].toBool());
        QTRY_VERIFY(window.waveQueue_->preparation()["deferred"].toBool());
        QTRY_COMPARE(window.waveQueue_->preparation()["total"].toInteger(), qint64(32));
        const auto before = window.waveQueue_->preparation();
        window.settingsDialog_->close();
        QTRY_VERIFY(!window.settingsDialog_->isVisible());
        QTest::qWait(40);
        const auto after = window.waveQueue_->preparation();
        for (const auto *name : {"running", "deferred", "cursor", "done", "failed", "total", "rate"})
            QCOMPARE(after[name], before[name]);
        window.showSettings(1);
        QVERIFY(pause->isEnabled());
        QTest::mouseClick(pause, Qt::LeftButton);
        QTRY_VERIFY(!window.waveQueue_->preparation()["running"].toBool());
        window.settingsDialog_->close();
        QVERIFY(!window.waveQueue_->preparation()["running"].toBool());
    }

    void minimumWindowsKeepSettingsAndMainControlsReachable() {
        QTemporaryDir data;
        Window window(data.path());
        window.resize(980, 640);
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        QCOMPARE(window.size(), QSize(980, 640));
        QVERIFY(fullyInsideWindow(window.settingsButton_, &window));
        QVERIFY(fullyInsideWindow(window.cacheStatusButton_, &window));
        QVERIFY(fullyInsideWindow(window.play_, &window));
        window.showSettings(1);
        auto *dialog = window.settingsDialog_;
        dialog->resize(760, 520);
        QTest::qWait(20);
        QCOMPARE(dialog->size(), QSize(760, 520));
        QVERIFY(fullyInsideWindow(window.settingsNavigation_, dialog));
        for (int page = 0; page < 6; ++page) {
            auto *item = window.settingsNavigation_->item(page);
            const auto rect = window.settingsNavigation_->visualItemRect(item);
            QVERIFY(window.settingsNavigation_->viewport()->rect().contains(rect));
            QTest::mouseClick(window.settingsNavigation_->viewport(), Qt::LeftButton, Qt::NoModifier,
                              rect.center());
            QCOMPARE(window.settingsPages_->currentIndex(), page);
            auto *scroll = qobject_cast<QScrollArea *>(window.settingsPages_->currentWidget());
            QVERIFY(scroll);
            if (page == 0) {
                scroll->ensureWidgetVisible(window.autoPlay_);
                QVERIFY(fullyInsideWindow(window.autoPlay_, dialog));
            } else if (page == 1) {
                for (const auto *name :
                     {"prepareAllWaves", "pauseWavePreparation", "skipWavePreparation",
                      "wavePreparationFailures", "reanalyzeReferenceWaves", "sharedCacheSettings"}) {
                    auto *button = dialog->findChild<QPushButton *>(name);
                    QVERIFY2(button, name);
                    scroll->ensureWidgetVisible(button);
                    QVERIFY2(fullyInsideWindow(button, dialog), name);
                }
            } else if (page == 2) {
                scroll->ensureWidgetVisible(window.editingPriority_);
                QVERIFY(fullyInsideWindow(window.editingPriority_, dialog));
            } else if (page == 3) {
                scroll->ensureWidgetVisible(window.folderIndentation_);
                QVERIFY(fullyInsideWindow(window.folderIndentation_, dialog));
                scroll->ensureWidgetVisible(window.listDensity_);
                QVERIFY(fullyInsideWindow(window.listDensity_, dialog));
            }
        }
        dialog->close();
        QVERIFY(window.isVisible());
        QVERIFY(fullyInsideWindow(window.play_, &window));
    }
};

int main(int argc, char **argv) {
    if (argc > 2 && QByteArray(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    SettingsUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "settings_ui_tests.moc"
