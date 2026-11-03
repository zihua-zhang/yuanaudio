#include "window.h"
#include "workers.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSettings>
#include <QTimer>
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
    void trackMenuOffersFinderAndNoCandidateActions() {
        QTemporaryDir data;
        QVERIFY(!seed(data.path()).isEmpty());
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_ && window.model_->rowCount() == 2);
        QStringList actions;
        QTimer::singleShot(0, &window, [&] {
            for (auto *menu : window.findChildren<QMenu *>()) {
                if (!menu->isVisible()) continue;
                for (auto *action : menu->actions()) actions << action->text();
                menu->close();
            }
        });
        const auto point = window.table_->visualRect(window.model_->index(1, 1)).center();
        window.table_->customContextMenuRequested(point);
#ifdef Q_OS_MACOS
        QVERIFY(actions.contains("在 Finder 中显示"));
#else
        QVERIFY(actions.contains("在文件管理器中显示"));
#endif
        for (const auto &action : actions) QVERIFY(!action.contains("候选"));
    }
    void retiredCandidateModeFallsBackToRecentWithoutDeletingLegacyData() {
        QTemporaryDir data;
        const auto tracks = seed(data.path());
        qint64 legacyList = 0;
        {
            WorkspaceStore store(data.filePath("workspace.sqlite"));
            QVERIFY(store.open());
            legacyList = store.createList("旧项目候选");
            QVERIFY(store.addCandidate(legacyList, tracks[0]));
            QVERIFY(store.recordAudition(tracks[1]));
            QSettings settings(data.filePath("workspace-ui.ini"), QSettings::IniFormat);
            settings.setValue("mode", 0); // Old Candidates mode.
            settings.setValue("expanded", true);
        }
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        auto *mode = window.workspace_->findChild<QComboBox *>("workspaceMode");
        QVERIFY(mode);
        QCOMPARE(mode->count(), 2);
        QCOMPARE(mode->currentText(), QString("最近试听"));
        auto *rows = window.workspace_->findChild<WorkspaceTrackList *>("workspaceRows");
        QCOMPARE(rows->count(), 1);
        QVERIFY(rows->item(0)->text().contains(tracks[1].name));
        QCOMPARE(window.workspace_->store().candidates(legacyList).size(), 1);
    }
    void workspaceDefaultsCollapsedAndKeepsItsOwnVisibilityAndMode() {
        QTemporaryDir data;
        QVERIFY(!seed(data.path()).isEmpty());
        {
            Window window(data.path());
            window.show();
            QTRY_VERIFY(window.indexReady_ && !window.loading_);
            QVERIFY(window.workspace_);
            QVERIFY(window.workspaceToggle_);
            QVERIFY(!window.workspaceToggle_->isChecked());
            QVERIFY(!window.workspace_->isVisible());
            window.workspace_->setMode(WorkspacePanel::Folders);
            window.workspaceToggle_->setChecked(true);
            QVERIFY(window.workspace_->isVisible());
            QVERIFY(window.workspace_->height() <= 230);
        }
        Window reopened(data.path());
        reopened.show();
        QTRY_VERIFY(reopened.indexReady_ && !reopened.loading_);
        QVERIFY(reopened.workspaceToggle_->isChecked());
        QVERIFY(reopened.workspace_->isVisible());
        QCOMPARE(reopened.workspace_->findChild<QComboBox *>("workspaceMode")->currentText(), QString("常用目录"));
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
        QVERIFY(window.workspace_->recordAudition(snapshot));
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
        QVERIFY(window.workspace_->recordAudition(missing));
        const auto token = window.workspaceRequest_;
        window.workspace_->trackActivated(missing);
        QVERIFY(window.workspaceRequest_ > token);
        window.searchPool_.waitForDone();
        QTest::qWait(100);
        QCOMPARE(window.selected_.id, tracks[1].id);
        QVERIFY(!window.workspace_->store().recents().isEmpty());
    }
    void missingOriginalReportsUnavailableWithoutLaunchingFileManager() {
        QTemporaryDir data;
        const auto tracks = seed(data.path());
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && !window.loading_);
        const auto before = window.findChildren<QProcess *>().size();
        window.revealOriginalFile(tracks[0]);
        QTRY_VERIFY(window.status_->text().contains("原文件不存在或暂时不可访问"));
        QCOMPARE(window.findChildren<QProcess *>().size(), before);
        QVERIFY(window.status_->text().contains(tracks[0].absolutePath()));
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
