#include "window.h"
#include "workers.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QInputMethodEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QSemaphore>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

class DeferredSearchDelivery : public QObject {
  protected:
    bool eventFilter(QObject *, QEvent *event) override {
        // Let the real query finish, but hold its result delivery until the save
        // callback has run. The test invokes the watcher's finished signal once.
        return event->type() == QEvent::FutureCallOut;
    }
};

class WindowUxTests : public QObject {
    Q_OBJECT
    static bool seed(const QString &root, const QStringList &names = {"旧曲.wav", "雨夜.wav"}) {
        Index db(QDir(root).filePath("library.sqlite"));
        if (!db.open() || !db.saveLibrary({"qa", "测试素材库", root, ""}))
            return false;
        QList<Track> tracks;
        for (const auto &name : names) {
            Track track;
            track.libraryId = "qa";
            track.relativePath = name;
            track.name = QFileInfo(name).fileName();
            track.folder = QFileInfo(name).path();
            track.size = 1000;
            track.modified = 1000;
            tracks << track;
        }
        return db.upsert(tracks);
    }

  private slots:
    void chinesePreeditDefersPendingQueryUntilCommit() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 2);
        window.search_->setFocus();
        window.search_->setText("旧");
        QInputMethodEvent preedit("yuye", {});
        QApplication::sendEvent(window.search_, &preedit);
        QVERIFY(window.musicComposing_);
        QVERIFY(!window.debounce_.isActive());
        const auto generation = window.queryGeneration_;
        window.refreshSearch();
        QTest::qWait(250);
        QCOMPARE(window.queryGeneration_, generation);
        QCOMPARE(window.model_->rowCount(), 2);
        QInputMethodEvent commit;
        commit.setCommitString("雨夜", -1, 1);
        QApplication::sendEvent(window.search_, &commit);
        QVERIFY(!window.musicComposing_);
        QCOMPARE(window.search_->text(), QString("雨夜"));
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QCOMPARE(window.model_->tracks[0].name, QString("雨夜.wav"));
        QVERIFY(!window.loading_);
    }
    void cancellingPreeditRunsCommittedQueryWithoutAnotherTextChange() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 2);
        window.search_->setFocus();
        window.search_->setText("旧");
        QInputMethodEvent preedit("yu", {});
        QApplication::sendEvent(window.search_, &preedit);
        QTest::qWait(200);
        QCOMPARE(window.model_->rowCount(), 2);
        QSignalSpy textChanges(window.search_, &QLineEdit::textChanged);
        QInputMethodEvent cancel;
        QApplication::sendEvent(window.search_, &cancel);
        QCOMPARE(textChanges.count(), 0);
        QCOMPARE(window.search_->text(), QString("旧"));
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QCOMPARE(window.model_->tracks[0].name, QString("旧曲.wav"));
        QVERIFY(!window.musicComposing_ && !window.loading_);
    }
    void cancellingFolderPreeditRunsCommittedQueryWithoutAnotherTextChange() {
        QTemporaryDir data;
        QVERIFY(seed(data.path(), {"旧目录/音乐.wav", "雨夜/音乐.wav"}));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        window.folderSearch_->setFocus();
        window.folderSearch_->setText("旧目录");
        QInputMethodEvent preedit("yu", {});
        QApplication::sendEvent(window.folderSearch_, &preedit);
        QTest::qWait(150);
        QCOMPARE(window.folderMatches_.size(), 0);
        QSignalSpy textChanges(window.folderSearch_, &QLineEdit::textChanged);
        QInputMethodEvent cancel;
        QApplication::sendEvent(window.folderSearch_, &cancel);
        QCOMPARE(textChanges.count(), 0);
        QTRY_COMPARE(window.folderMatches_.size(), 1);
        QCOMPARE(window.folderMatches_.first().path, QString("旧目录"));
    }
    void personalSaveWaitsOffUiThreadAndDoesNotChangeAnotherSelection() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 2);
        window.table_->setCurrentIndex(window.model_->index(0, TrackModel::Name));
        const Track first = window.selected_;
        QSemaphore blocked, release;
        window.metadataPool_.start([&] {
            blocked.release();
            release.acquire();
        });
        auto unblock = qScopeGuard([&] {
            release.release();
            window.metadataPool_.waitForDone();
        });
        QTRY_VERIFY(blocked.available() > 0);
        window.tags_->setText("稍后保存的草稿");
        QElapsedTimer elapsed;
        elapsed.start();
        window.favorite_->click();
        QVERIFY(elapsed.elapsed() < 100);
        QVERIFY(!window.selected_.favorite);
        QVERIFY(!window.favorite_->isEnabled());
        window.table_->setCurrentIndex(window.model_->index(1, TrackModel::Name));
        const Track second = window.selected_;
        QVERIFY(window.favorite_->isEnabled());
        window.table_->setCurrentIndex(window.model_->index(0, TrackModel::Name));
        QCOMPARE(window.tags_->text(), QString("稍后保存的草稿"));
        QVERIFY(!window.tags_->isEnabled());
        window.table_->setCurrentIndex(window.model_->index(1, TrackModel::Name));
        bool heartbeat = false;
        QTimer::singleShot(0, &window, [&] { heartbeat = true; });
        QTRY_VERIFY(heartbeat);
        release.release();
        unblock.dismiss();
        QTRY_VERIFY(window.model_->tracks[window.model_->rowForTrack(first)].favorite);
        QCOMPARE(window.selected_.id, second.id);
        QVERIFY(!window.selected_.favorite);
        QVERIFY(window.favorite_->text().startsWith("☆"));
        Index db(window.databasePath_);
        QVERIFY(db.open());
        const auto saved = db.searchAll("", "", "", true, true);
        QCOMPARE(saved.tracks.size(), 1);
        QCOMPARE(saved.tracks.first().id, first.id);
        QCOMPARE(saved.tracks.first().tags, QString("稍后保存的草稿"));
    }
    void failedFavoriteSaveKeepsTruthfulStateAndNextClickRetries() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 2);
        window.table_->setCurrentIndex(window.model_->index(0, TrackModel::Name));
        const auto database = window.databasePath_;
        window.databasePath_ = data.path();
        window.tags_->setText("待保存的标签");
        window.favorite_->click();
        QTRY_VERIFY(window.favorite_->isEnabled());
        QVERIFY(!window.selected_.favorite);
        QVERIFY(window.selected_.tags.isEmpty());
        QCOMPARE(window.tags_->text(), QString("待保存的标签"));
        QVERIFY(!window.model_->tracks[0].favorite);
        QVERIFY(window.favorite_->text().startsWith("☆"));
        window.databasePath_ = database;
        window.favorite_->click();
        QTRY_VERIFY(window.selected_.favorite && window.favorite_->isEnabled());
        QVERIFY(window.model_->tracks[0].favorite);
        QCOMPARE(window.selected_.tags, QString("待保存的标签"));
    }
    void personalSaveRefreshesFavoriteAndTagSearchMembership() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        {
            Index db(data.filePath("library.sqlite"));
            QVERIFY(db.open());
            for (const auto &track : db.searchAll("", "", "", false, true).tracks)
                QVERIFY(db.setPersonal(track.id, true, "旧标签"));
        }
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 2);
        window.tree_->setCurrentIndex(window.folderModel_->special(true));
        QTRY_VERIFY(window.favoriteScope_ && !window.loading_);
        window.table_->setCurrentIndex(window.model_->index(0, TrackModel::Name));
        const auto generation = window.selectionGeneration_;
        window.favorite_->click();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QCOMPARE(window.selectionGeneration_, generation);
        window.tree_->setCurrentIndex(window.folderModel_->special(false));
        window.search_->setText("旧标签");
        QTRY_VERIFY(!window.loading_ && window.model_->rowCount() == 2);
        window.table_->setCurrentIndex(window.model_->index(1, TrackModel::Name));
        const auto editedGeneration = window.selectionGeneration_;
        window.tags_->setText("新标签");
        window.persistPersonal();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QCOMPARE(window.selectionGeneration_, editedGeneration);
        QCOMPARE(window.selected_.tags, QString("新标签"));
        window.search_->setText("新标签");
        QTRY_VERIFY(!window.loading_ && window.model_->rowCount() == 1 &&
                    window.model_->tracks.first().tags == "新标签");
    }
    void olderUnfilteredQueryCannotOverwriteConfirmedPersonalSave() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(!window.loading_ && window.model_->rowCount() == 2);
        window.table_->setCurrentIndex(window.model_->index(0, TrackModel::Name));
        const Track savedTrack = window.selected_;
        window.refreshSearch();
        QFutureWatcher<SearchPage> *oldQuery = nullptr;
        for (auto *candidate : window.findChildren<QFutureWatcherBase *>())
            if (auto *search = dynamic_cast<QFutureWatcher<SearchPage> *>(candidate))
                oldQuery = search;
        QVERIFY(oldQuery);
        DeferredSearchDelivery delayed;
        oldQuery->installEventFilter(&delayed);
        QTRY_VERIFY(oldQuery->future().isFinished());
        const auto oldPage = oldQuery->future().result();
        QCOMPARE(oldPage.tracks.size(), 2);
        QVERIFY(!oldPage.tracks.first().favorite);
        QVERIFY(window.loading_);

        window.tags_->setText("保存后的标签");
        window.favorite_->click();
        QTRY_VERIFY(window.selected_.favorite && window.favorite_->isEnabled());
        // Deliver the actual old SQL result after the newer save was confirmed.
        QVERIFY(QMetaObject::invokeMethod(oldQuery, "finished", Qt::DirectConnection));
        QTRY_VERIFY(!window.loading_);
        const int row = window.model_->rowForTrack(savedTrack);
        QVERIFY(row >= 0);
        QVERIFY(window.model_->tracks[row].favorite);
        QCOMPARE(window.model_->tracks[row].tags, QString("保存后的标签"));
        QVERIFY(window.selected_.favorite);
    }
    void failedSearchKeepsRowsAndOffersWorkingRetry() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 2);
        const auto priorIds = QList<qint64>{window.model_->tracks[0].id, window.model_->tracks[1].id};
        const auto database = window.databasePath_;
        window.databasePath_ = data.path();
        window.search_->setText("雨夜");
        QTRY_VERIFY(!window.lastSearchError_.isEmpty());
        QVERIFY(!window.loading_);
        QCOMPARE(window.model_->rowCount(), 2);
        QCOMPARE(window.model_->tracks[0].id, priorIds[0]);
        QCOMPARE(window.model_->tracks[1].id, priorIds[1]);
        QCOMPARE(window.resultStack_->currentWidget(), window.table_);
        QVERIFY(window.table_->isVisible());
        QVERIFY(window.searchFeedback_->isVisible());
        QVERIFY(window.searchNotice_->text().contains("保留上次结果"));
        QVERIFY(!window.searchNotice_->toolTip().isEmpty());
        QVERIFY(window.retrySearch_->isVisible() && window.retrySearch_->isEnabled());
        window.databasePath_ = database;
        window.retrySearch_->click();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QCOMPARE(window.model_->tracks[0].name, QString("雨夜.wav"));
        QVERIFY(window.lastSearchError_.isEmpty());
        QVERIFY(!window.searchFeedback_->isVisible());
        QVERIFY(!window.retrySearch_->isVisible());
    }
    void pendingSearchLabelsRetainedResultsUntilReplacementIsReady() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 2);
        const QRect priorTable(window.table_->mapTo(&window, QPoint()), window.table_->size());
        QSemaphore blocked, release;
        window.searchPool_.start([&] {
            blocked.release();
            release.acquire();
        });
        auto unblock = qScopeGuard([&] {
            release.release();
            window.searchPool_.waitForDone();
        });
        QTRY_VERIFY(blocked.available() > 0);
        window.search_->setText("雨夜");
        QTRY_VERIFY(window.loading_);
        QCOMPARE(window.model_->rowCount(), 2);
        QVERIFY(window.table_->isVisible());
        QVERIFY(!window.searchFeedback_->isVisible());
        QVERIFY(window.results_->text().contains("搜索中"));
        QVERIFY(window.results_->text().contains("上次结果"));
        QCOMPARE(QRect(window.table_->mapTo(&window, QPoint()), window.table_->size()), priorTable);
        QVERIFY(!window.retrySearch_->isVisible());
        release.release();
        unblock.dismiss();
        QTRY_COMPARE(window.model_->rowCount(), 1);
        QVERIFY(!window.loading_);
        QVERIFY(!window.searchFeedback_->isVisible());
    }
    void noMatchesOfferClearingQueryAndRestoreResults() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path());
        window.show();
        QTRY_COMPARE(window.model_->rowCount(), 2);
        window.search_->setText("没有这个素材");
        QTRY_VERIFY(!window.loading_ && window.model_->rowCount() == 0);
        QVERIFY(window.lastSearchError_.isEmpty());
        QVERIFY(!window.table_->isVisible());
        QVERIFY(window.emptyTitle_->isVisible());
        QVERIFY(window.emptyTitle_->text().contains("没有匹配"));
        QCOMPARE(window.emptyAction_->text(), QString("清除关键词"));
        QVERIFY(window.emptyAction_->isVisible() && window.emptyAction_->isEnabled());
        window.emptyAction_->click();
        QVERIFY(window.search_->text().isEmpty());
        QTRY_COMPARE(window.model_->rowCount(), 2);
        QVERIFY(window.table_->isVisible());
        QVERIFY(!window.emptyTitle_->isVisible());
    }
    void emptyLibraryOffersAddingDirectory() {
        QTemporaryDir data;
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        QCOMPARE(window.model_->rowCount(), 0);
        QVERIFY(window.libraries_.isEmpty());
        QVERIFY(window.emptyTitle_->isVisible());
        QCOMPARE(window.emptyTitle_->text(), QString("从一个音乐文件夹开始"));
        QCOMPARE(window.emptyAction_->text(), QString("添加音乐目录"));
        QVERIFY(window.emptyAction_->isVisible() && window.emptyAction_->isEnabled());
        bool dialogOpened = false;
        QTimer::singleShot(0, &window, [&] {
            for (auto *dialog : window.findChildren<QDialog *>()) {
                if (dialog->windowTitle() == "添加音乐目录") {
                    dialogOpened = true;
                    dialog->reject();
                }
            }
        });
        window.emptyAction_->click();
        QVERIFY(dialogOpened);
    }
    void preferencesPersistAcrossReopening() {
        QTemporaryDir data;
        QSize previousSize;
        QList<int> previousSplit;
        {
            Window window(data.path());
            window.show();
            QTRY_VERIFY(window.indexReady_ && !window.loading_);
            const auto available = QGuiApplication::primaryScreen()->availableGeometry();
            window.resize(qMax(980, qMin(1100, available.width() - 40)),
                          qMax(640, qMin(720, available.height() - 80)));
            window.volume_->setValue(23);
            window.autoPlay_->setChecked(true);
            window.splitter_->setSizes({280, 700});
            QTest::qWait(30);
            previousSize = window.size();
            previousSplit = window.splitter_->sizes();
        }
        QSettings saved(data.filePath("browser.ini"), QSettings::IniFormat);
        QCOMPARE(saved.value("volume").toInt(), 23);
        QVERIFY(saved.value("autoPlay").toBool());
        QVERIFY(!saved.value("geometry").toByteArray().isEmpty());
        QVERIFY(!saved.value("splitter").toByteArray().isEmpty());
        Window reopened(data.path());
        reopened.show();
        QTRY_VERIFY(reopened.indexReady_ && !reopened.loading_);
        QCOMPARE(reopened.volume_->value(), 23);
        QVERIFY(reopened.autoPlay_->isChecked());
        QCOMPARE(reopened.size(), previousSize);
        const auto split = reopened.splitter_->sizes();
        QCOMPARE(split.size(), previousSplit.size());
        for (int i = 0; i < split.size(); ++i)
            QVERIFY(qAbs(split[i] - previousSplit[i]) <= 2);
    }
    void invalidSavedPreferencesClampVolumeAndKeepUsableLayout_data() {
        QTest::addColumn<int>("volume");
        QTest::addColumn<int>("expected");
        QTest::newRow("negative-volume") << -12 << 0;
        QTest::newRow("oversized-volume") << 400 << 100;
    }
    void invalidSavedPreferencesClampVolumeAndKeepUsableLayout() {
        QFETCH(int, volume);
        QFETCH(int, expected);
        QTemporaryDir data;
        {
            QSettings settings(data.filePath("browser.ini"), QSettings::IniFormat);
            settings.setValue("volume", volume);
            settings.setValue("geometry", QByteArray("invalid-geometry"));
            settings.setValue("splitter", QByteArray("invalid-layout"));
        }
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        QCOMPARE(window.volume_->value(), expected);
        QVERIFY(window.width() >= window.minimumWidth());
        QVERIFY(window.height() >= window.minimumHeight());
        const auto sizes = window.splitter_->sizes();
        QCOMPARE(sizes.size(), 2);
        QVERIFY(sizes[0] > 100 && sizes[1] > 300);
        QVERIFY(window.search_->isVisible());
    }
};

int main(int argc, char **argv) {
    if (argc > 2 && QByteArray(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    WindowUxTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "window_ux_tests.moc"
