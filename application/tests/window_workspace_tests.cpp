#include "window.h"
#include "workers.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSlider>
#include <QTemporaryDir>
#include <QTreeView>
#include <QtTest>

class Window08Tests : public QObject {
    Q_OBJECT
    static QList<Track> seed(const QString &root) {
        Index db(QDir(root).filePath("library.sqlite"));
        if (!db.open() || !db.saveLibrary({"qa", "测试素材库", root, ""}))
            return {};
        QList<Track> tracks;
        for (const auto &name : {QString("剧情/回忆/甲.wav"), QString("剧情/紧张/乙.wav")}) {
            Track t;
            t.libraryId = "qa";
            t.relativePath = name;
            t.name = QFileInfo(name).fileName();
            t.folder = QFileInfo(name).path();
            t.root = root;
            t.size = 1000;
            t.modified = 1000;
            tracks << t;
        }
        if (!db.upsert(tracks))
            return {};
        return db.search("").tracks;
    }

  private slots:
    void workspaceDefaultsCollapsedAndKeepsItsOwnVisibilityAndList() {
        QTemporaryDir data;
        QVERIFY(!seed(data.path()).isEmpty());
        qint64 candidate = 0;
        {
            Window window(data.path());
            window.show();
            QTRY_VERIFY(window.indexReady_ && !window.loading_);
            QVERIFY(window.workspace_);
            QVERIFY(window.workspaceToggle_);
            QVERIFY(!window.workspaceToggle_->isChecked());
            QVERIFY(!window.workspace_->isVisible());
            candidate = window.workspace_->store().createList("第三集");
            QVERIFY(candidate);
            window.workspace_->refresh();
            window.workspace_->setCurrentCandidateList(candidate);
            window.workspaceToggle_->setChecked(true);
            QVERIFY(window.workspace_->isVisible());
            QVERIFY(window.workspace_->height() <= 230);
        }
        Window reopened(data.path());
        reopened.show();
        QTRY_VERIFY(reopened.indexReady_ && !reopened.loading_);
        QVERIFY(reopened.workspaceToggle_->isChecked());
        QVERIFY(reopened.workspace_->isVisible());
        QCOMPARE(reopened.workspace_->currentCandidateList(), candidate);
    }
    void workspaceActivationResolvesCurrentVersionByIdentity() {
        QTemporaryDir data;
        const auto tracks = seed(data.path());
        QCOMPARE(tracks.size(), 2);
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_ && window.model_->rowCount() == 2);
        window.volume_->setValue(0);
        auto snapshot = tracks[0];
        snapshot.id = tracks[1].id; // A stale/reused row ID must never activate another music file.
        snapshot.root = "//stale-server/old-library";
        snapshot.name = "旧名称";
        auto updated = tracks[0];
        updated.size = 2222;
        updated.modified = 3333;
        {
            Index db(data.filePath("library.sqlite"));
            QVERIFY(db.open());
            QVERIFY(db.upsert({updated}));
        }
        QVERIFY(window.workspace_->addCandidate(snapshot));
        window.workspace_->trackActivated(snapshot);
        QTRY_COMPARE(window.selected_.relativePath, updated.relativePath);
        QCOMPARE(window.selected_.id, tracks[0].id);
        QCOMPARE(window.selected_.root, data.path());
        QCOMPARE(window.selected_.size, qint64(2222));
        QCOMPARE(window.selected_.modified, qint64(3333));
        QCOMPARE(window.selected_.name, tracks[0].name);
    }
    void newerMainListChoiceCancelsPendingWorkspaceActivation() {
        QTemporaryDir data;
        const auto tracks = seed(data.path());
        QCOMPARE(tracks.size(), 2);
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_ && window.model_->rowCount() == 2);
        window.volume_->setValue(0);
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
        window.workspace_->trackActivated(tracks[0]);
        window.selectTrackValue(tracks[1], 0.0);
        QCOMPARE(window.selected_.id, tracks[1].id);
        release.release();
        unblock.dismiss();
        window.searchPool_.waitForDone();
        QTest::qWait(100);
        QCOMPARE(window.selected_.id, tracks[1].id);
    }
    void missingWorkspaceReferenceDoesNotReplaceCurrentSong() {
        QTemporaryDir data;
        const auto tracks = seed(data.path());
        QCOMPARE(tracks.size(), 2);
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        window.volume_->setValue(0);
        window.selectTrackValue(tracks[1]);
        auto missing = tracks[0];
        missing.relativePath = "已移走/找不到.wav";
        QVERIFY(window.workspace_->addCandidate(missing));
        const auto token = window.workspaceRequest_;
        window.workspace_->trackActivated(missing);
        QVERIFY(window.workspaceRequest_ > token);
        window.searchPool_.waitForDone();
        QTest::qWait(100);
        QCOMPARE(window.selected_.id, tracks[1].id);
        QCOMPARE(window.workspace_->store().candidates(window.workspace_->currentCandidateList()).size(), 1);
    }
    void pinnedFolderUsesExistingTreeNavigation() {
        QTemporaryDir data;
        QVERIFY(!seed(data.path()).isEmpty());
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        QVERIFY(window.workspace_->pinFolder("qa", "剧情/回忆", "回忆", data.path()));
        window.workspace_->folderActivated("qa", "剧情/回忆");
        QTRY_COMPARE(window.scopeLibrary_, QString("qa"));
        QTRY_COMPARE(window.scopeFolder_, QString("剧情/回忆"));
        QTRY_VERIFY(!window.loading_ && window.model_->rowCount() == 1);
        QCOMPARE(window.tree_->currentIndex().data(FolderModel::PathRole).toString(), QString("剧情/回忆"));
    }
};

int main(int argc, char **argv) {
    if (argc > 2 && QByteArray(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    Window08Tests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "window_workspace_tests.moc"
