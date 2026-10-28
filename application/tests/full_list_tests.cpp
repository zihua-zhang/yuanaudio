#include "full_list_fixture.h"
#include "window.h"
#include "workers.h"
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QtTest>

class FullListTests : public QObject {
    Q_OBJECT
    QTemporaryDir data_;
    QString path_;

  private slots:
    void initTestCase() {
        QVERIFY(data_.isValid());
        path_ = data_.filePath("library.sqlite");
        Index db(path_);
        QString error;
        QVERIFY2(seedFullList(db, data_.filePath("no-audio"), 100000, &error), qPrintable(error));
        QVERIFY(db.setPersonal(10001, true, "个人收藏"));
        QVERIFY(db.setPersonal(80001, true, "个人收藏"));
    }
    void fullSnapshotHasAllSixtyAndHundredThousandRows() {
        Index db(path_);
        QVERIFY(db.open());
        const auto sixty = db.searchAll("", "first");
        QVERIFY2(sixty.error.isEmpty(), qPrintable(sixty.error));
        QVERIFY(!sixty.more);
        QVERIFY(!sixty.cancelled);
        QCOMPARE(sixty.tracks.size(), 60000);
        QCOMPARE(sixty.tracks.first().name, QString("Music_000000.wav"));
        QCOMPARE(sixty.tracks.last().name, QString("Music_059999.wav"));
        const auto hundred = db.searchAll("");
        QCOMPARE(hundred.tracks.size(), 100000);
        QCOMPARE(hundred.tracks.last().name, QString("Music_099999.wav"));
        for (int i = 0; i < hundred.tracks.size(); ++i)
            QCOMPARE(hundred.tracks[i].id, qint64(i + 1));
    }
    void chineseTermsFoldersAndFavoritesRetainSearchSemantics() {
        Index db(path_);
        QVERIFY(db.open());
        QCOMPARE(db.searchAll("雨夜 Music", "first").tracks.size(), 30000);
        QCOMPARE(db.searchAll("", "first", "短剧/雨夜").tracks.size(), 30000);
        QCOMPARE(db.searchAll("", "first", "短剧/雨夜", false, false).tracks.size(), 24000);
        QCOMPARE(db.searchAll("", "first", "短剧/雨夜/回忆", false, false).tracks.size(), 6000);
        QCOMPARE(db.searchAll("个人收藏", {}, {}, true).tracks.size(), 2);
        QCOMPARE(db.searchAll("", "first", {}, true).tracks.size(), 1);
        QCOMPARE(db.searchAll("Music_099999.wav").tracks.size(), 1);
        QVERIFY(db.searchAll("不存在的配乐").tracks.isEmpty());
        QVERIFY(db.searchAll("", "first", "短剧/雨").tracks.isEmpty());
    }
    void cancellationDropsPartialRowsAndReleasesReadTransaction() {
        Index db(path_);
        QVERIFY(db.open());
        int checks = 0;
        const auto cancelled = db.searchAll("", {}, {}, false, true, [&] { return ++checks == 6; });
        QVERIFY(cancelled.cancelled);
        QVERIFY(cancelled.error.isEmpty());
        QVERIFY(cancelled.tracks.isEmpty());
        QCOMPARE(checks, 6);
        const auto after = db.searchAll("Music_059999.wav");
        QCOMPARE(after.tracks.size(), 1);
        QVERIFY(!after.cancelled);
        const auto beforeStart = db.searchAll("", {}, {}, false, true, [] { return true; });
        QVERIFY(beforeStart.cancelled);
        QVERIFY(beforeStart.tracks.isEmpty());
    }
    void snapshotDoesNotMixConcurrentIndexVersions() {
        Index reader(path_), writer(path_);
        QVERIFY(reader.open());
        QVERIFY(writer.open());
        Track changed = writer.searchAll("Music_099999.wav").tracks.first();
        ++changed.modified;
        int checks = 0;
        bool written = false;
        const auto before = reader.searchAll("", {}, {}, false, true, [&] {
            if (++checks == 5)
                written = writer.upsert({changed});
            return false;
        });
        QVERIFY(written);
        QCOMPARE(before.tracks.size(), 100000);
        QCOMPARE(before.tracks.last().modified, qint64(1000));
        const auto after = reader.searchAll("Music_099999.wav");
        QVERIFY(after.generation > before.generation);
        QCOMPARE(after.tracks.first().modified, changed.modified);
        changed.modified = 1000;
        QVERIFY(writer.upsert({changed}));
    }
    void rowLookupSurvivesReplaceAppendAndRejectsStaleFileVersion() {
        Index db(path_);
        QVERIFY(db.open());
        TrackModel model;
        model.replace(db.searchAll("").tracks);
        const auto last = model.tracks.last();
        QCOMPARE(model.rowForId(last.id), 99999);
        QCOMPARE(model.rowForTrack(last), 99999);
        auto old = last;
        ++old.modified;
        QCOMPARE(model.rowForTrack(old), -1);
        model.replace({last});
        QCOMPARE(model.rowForId(1), -1);
        QCOMPARE(model.rowForTrack(last), 0);
        Track added = last;
        added.id = 200000;
        model.append({added});
        QCOMPARE(model.rowForTrack(added), 1);
        model.replace({});
        QCOMPARE(model.rowForTrack(last), -1);
    }
    void windowReachesLastRowAndOnlyNewestQueuedSearchIsApplied() {
        Window window(data_.path());
        window.show();
        QTRY_VERIFY_WITH_TIMEOUT(!window.loading_ && window.model_->rowCount() == 100000, 15000);
        QVERIFY(!window.more_->isVisible());
        QVERIFY(!window.hasMore_);
        QVERIFY(window.results_->text().contains("100000"));
        window.table_->scrollToBottom();
        QTRY_VERIFY(window.table_->visualRect(window.model_->index(99999, TrackModel::Name))
                        .intersects(window.table_->viewport()->rect()));
        const int lastScroll = window.table_->verticalScrollBar()->value();
        {
            QSignalBlocker blocker(window.table_->selectionModel());
            window.table_->setCurrentIndex(window.model_->index(99999, TrackModel::Name));
        }
        window.refreshSearch();
        QTRY_VERIFY_WITH_TIMEOUT(!window.loading_, 15000);
        QCOMPARE(window.table_->currentIndex().row(), 99999);
        QCOMPARE(window.table_->verticalScrollBar()->value(), lastScroll);
        QSemaphore started, release;
        window.searchPool_.start([&] {
            started.release();
            release.acquire();
        });
        QVERIFY(started.tryAcquire(1, 2000));
        auto unblock = qScopeGuard([&] { release.release(); });
        QSignalSpy reset(window.model_, &QAbstractItemModel::modelReset);
        {
            QSignalBlocker blocker(window.search_);
            window.search_->setText("雨夜");
        }
        window.refreshSearch();
        {
            QSignalBlocker blocker(window.search_);
            window.search_->setText("Music_099999.wav");
        }
        window.refreshSearch();
        release.release();
        unblock.dismiss();
        QTRY_VERIFY_WITH_TIMEOUT(!window.loading_ && window.model_->rowCount() == 1, 15000);
        QCOMPARE(reset.size(), 1);
        QCOMPARE(window.model_->tracks.first().name, QString("Music_099999.wav"));
    }
};

int main(int argc, char **argv) {
    if (argc > 2 && QByteArray(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    FullListTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "full_list_tests.moc"
