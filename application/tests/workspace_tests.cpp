#include "workspace_panel.h"
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QMimeData>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

class WorkspaceTests : public QObject {
    Q_OBJECT
    static Track track(int number, const QString &root = "//nas/music") {
        Track t;
        t.id = number;
        t.libraryId = "library-a";
        t.root = root;
        t.relativePath = QString("剧情/音乐 %1.wav").arg(number);
        t.name = QString("音乐 %1.wav").arg(number);
        t.folder = "剧情";
        t.size = 1000 + number;
        t.modified = 123456789;
        t.duration = 180000;
        t.favorite = true;
        t.tags = "回忆 温暖";
        return t;
    }

  private slots:
    void candidateListsPersistDeduplicateAndRemoveReferencesOnly() {
        QTemporaryDir data;
        const auto database = data.filePath("workspace.sqlite");
        const auto source = data.filePath("原音乐.wav");
        {
            QFile audio(source);
            QVERIFY(audio.open(QIODevice::WriteOnly));
            QCOMPARE(audio.write("unchanged original music"), qint64(24));
        }
        auto a = track(1, data.path());
        a.relativePath = a.name = "原音乐.wav";
        a.folder = ".";
        const auto b = track(2);
        qint64 defaultList = 0, second = 0;
        {
            WorkspaceStore store(database);
            QVERIFY2(store.open(), qPrintable(store.lastError()));
            QVERIFY(store.candidateLists().isEmpty());
            // Seed legacy data explicitly: the current UI no longer creates lists.
            defaultList = store.createList("本剧候选");
            QVERIFY(defaultList);
            second = store.createList("第二部短剧");
            QVERIFY(second);
            QVERIFY(store.addCandidate(defaultList, a));
            QVERIFY(store.addCandidate(defaultList, a));
            QVERIFY(store.addCandidate(defaultList, b));
            QVERIFY(store.addCandidate(second, a));
            QCOMPARE(store.candidates(defaultList).size(), 2);
            QCOMPARE(store.candidates(second).size(), 1);
            QVERIFY(store.renameList(second, "第二部 · 定稿"));
        }
        {
            WorkspaceStore store(database);
            QVERIFY(store.open());
            QCOMPARE(store.candidateLists().size(), 2);
            QCOMPARE(store.candidateLists()[1].name, QString("第二部 · 定稿"));
            const auto saved = store.candidates(defaultList);
            QCOMPARE(saved.size(), 2);
            QCOMPARE(saved[0].absolutePath(), a.absolutePath());
            QCOMPARE(saved[0].id, a.id);
            QCOMPARE(saved[0].size, a.size);
            QCOMPARE(saved[0].modified, a.modified);
            QCOMPARE(saved[0].duration, a.duration);
            QCOMPARE(saved[0].tags, a.tags);
            QCOMPARE(saved[0].favorite, a.favorite);
            QVERIFY(store.removeCandidate(defaultList, a.libraryId, a.relativePath));
            QCOMPARE(store.candidates(defaultList).size(), 1);
            QCOMPARE(store.candidates(second).size(), 1);
            QVERIFY(store.removeList(second));
            QVERIFY(store.candidates(second).isEmpty());
            QCOMPARE(store.candidateLists().size(), 1);
            QFile audio(source);
            QVERIFY(audio.open(QIODevice::ReadOnly));
            QCOMPARE(audio.readAll(), QByteArray("unchanged original music"));
            QVERIFY(store.removeList(defaultList));
            QVERIFY(store.candidateLists().isEmpty());
        }
    }
    void candidateIdentityUsesLibraryAndRelativePathNotLocalRowId() {
        QTemporaryDir data;
        WorkspaceStore store(data.filePath("workspace.sqlite"));
        QVERIFY(store.open());
        const auto list = store.createList("本剧候选");
        auto a = track(1);
        QVERIFY(store.addCandidate(list, a));
        a.id = 999;
        a.duration = 250000;
        QVERIFY(store.addCandidate(list, a));
        QCOMPARE(store.candidates(list).size(), 1);
        QCOMPARE(store.candidates(list)[0].id, qint64(999));
        auto otherLibrary = a;
        otherLibrary.libraryId = "library-b";
        QVERIFY(store.addCandidate(list, otherLibrary));
        QCOMPARE(store.candidates(list).size(), 2);
        QVERIFY(!store.createList("  "));
        QVERIFY(!store.createList("本剧候选"));
        QVERIFY(!store.addCandidate(987654, a));
        QCOMPARE(store.candidates(list).size(), 2);
        a.relativePath = "../escape.wav";
        QVERIFY(!store.addCandidate(list, a));
    }
    void recentlyAuditionedIsOrderedUniqueBoundedAndPersistent() {
        QTemporaryDir data;
        const auto database = data.filePath("workspace.sqlite");
        {
            WorkspaceStore store(database);
            QVERIFY(store.open());
            QVERIFY(store.recents().isEmpty());
            for (int i = 1; i <= 106; ++i)
                QVERIFY2(store.recordAudition(track(i)), qPrintable(store.lastError()));
            const auto recent = store.recents();
            QCOMPARE(recent.size(), WorkspaceStore::RecentLimit);
            QCOMPARE(recent.first().id, qint64(106));
            QCOMPARE(recent.last().id, qint64(7));
            auto replayed = track(20);
            replayed.id = 2000;
            QVERIFY(store.recordAudition(replayed));
            QCOMPARE(store.recents().size(), 100);
            QCOMPARE(store.recents().first().id, qint64(2000));
            QCOMPARE(store.recents()[1].id, qint64(106));
        }
        WorkspaceStore reopened(database);
        QVERIFY(reopened.open());
        QCOMPARE(reopened.recents().size(), 100);
        QCOMPARE(reopened.recents().first().id, qint64(2000));
        const auto replayed = track(20);
        QVERIFY(reopened.removeRecent(replayed.libraryId, replayed.relativePath));
        QCOMPARE(reopened.recents().size(), 99);
        QCOMPARE(reopened.recents().first().id, qint64(106));
    }
    void pinnedFoldersKeepLogicalIdentityOrderingAndPersist() {
        QTemporaryDir data;
        const auto database = data.filePath("workspace.sqlite");
        {
            WorkspaceStore store(database);
            QVERIFY(store.open());
            QVERIFY(store.pinFolder({"a", "剧情/回忆", "回忆", "//nas/a"}));
            QVERIFY(store.pinFolder({"b", "剧情/回忆", "回忆", "//nas/b"}));
            QVERIFY(store.pinFolder({"a", ".", "根目录", "//nas/a"}));
            QVERIFY(store.pinFolder({"a", "剧情/回忆", "更新名称", "Z:/music"}));
            const auto folders = store.pinnedFolders();
            QCOMPARE(folders.size(), 3);
            QCOMPARE(folders[0].name, QString("更新名称"));
            QCOMPARE(folders[0].root, QString("Z:/music"));
            QCOMPARE(folders[1].libraryId, QString("b"));
            QVERIFY(folders[2].path.isEmpty());
            QVERIFY(!store.pinFolder({"a", "../private", "无效", "//nas/a"}));
        }
        WorkspaceStore reopened(database);
        QVERIFY(reopened.open());
        QCOMPARE(reopened.pinnedFolders().size(), 3);
        QVERIFY(reopened.unpinFolder("a", "剧情/回忆"));
        QCOMPARE(reopened.pinnedFolders().size(), 2);
        QCOMPARE(reopened.pinnedFolders()[0].libraryId, QString("b"));
        QVERIFY(reopened.unpinFolder("a", "."));
        QCOMPARE(reopened.pinnedFolders().size(), 1);
    }
    void failedRecentWriteRollsBackRemovalOfPreviousReference() {
        QTemporaryDir data;
        const auto path = data.filePath("workspace.sqlite");
        WorkspaceStore store(path);
        QVERIFY(store.open());
        QVERIFY(store.recordAudition(track(1)));
        QVERIFY(store.recordAudition(track(2)));
        const auto connection = QString("workspace-atomic-test");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TRIGGER reject_recent_insert BEFORE INSERT ON recent_tracks "
                           "BEGIN SELECT RAISE(ABORT,'simulated write failure'); END"));
        }
        QSqlDatabase::removeDatabase(connection);
        QVERIFY(!store.recordAudition(track(1)));
        QVERIFY(!store.lastError().isEmpty());
        const auto after = store.recents();
        QCOMPARE(after.size(), 2);
        QCOMPARE(after[0].id, qint64(2));
        QCOMPARE(after[1].id, qint64(1));
    }
    void trackListDragUsesOriginalUrlsAndFoldersCannotDrag() {
        WorkspaceTrackList list;
        const auto a = track(1);
        list.setTrackRows({a, track(2)});
        list.setCurrentRow(0);
        std::unique_ptr<QMimeData> mime(list.originalMimeData());
        QCOMPARE(mime->urls(), originalFileUrls({a}));
        QCOMPARE(mime->urls()[0].host(), QString("nas"));
        QVERIFY(mime->urls()[0].toString().contains("music"));
        list.setFolderRows({{"a", "剧情", "剧情", "//nas/music"}});
        list.setCurrentRow(0);
        std::unique_ptr<QMimeData> folderMime(list.originalMimeData());
        QVERIFY(!folderMime->hasUrls());
        QVERIFY(!list.dragEnabled());
    }
    void panelTracksModesActivationAndRecentRecordingAreExplicit() {
        QTemporaryDir data;
        WorkspacePanel panel(data.filePath("workspace.sqlite"));
        panel.resize(180, 300);
        panel.show();
        auto *mode = panel.findChild<QComboBox *>("workspaceMode");
        QCOMPARE(mode->count(), 2);
        QCOMPARE(mode->currentText(), QString("最近试听"));
        QVERIFY(panel.recordAudition(track(1)));
        auto *rows = panel.findChild<WorkspaceTrackList *>("workspaceRows");
        QVERIFY(rows);
        QCOMPARE(rows->count(), 1);
        QCOMPARE(panel.store().recents().size(), 1);
        Track activated;
        connect(&panel, &WorkspacePanel::trackActivated, &panel, [&](const Track &t) {
            activated = t;
            // A root callback can update this panel immediately without invalidating its signal argument.
            panel.recordAudition(t);
        });
        auto activateCurrentRow = [rows] {
            QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier,
                              rows->visualItemRect(rows->currentItem()).center());
            QTest::mouseDClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier,
                              rows->visualItemRect(rows->currentItem()).center());
        };
        rows->setCurrentRow(0);
        activateCurrentRow();
        QCOMPARE(activated.id, qint64(1));
        QCOMPARE(panel.store().recents().size(), 1);
        panel.setMode(WorkspacePanel::Recent);
        QCOMPARE(rows->count(), 1);
        QVERIFY(rows->item(0)->toolTip().contains(track(1).absolutePath()));
        QVERIFY(panel.pinFolder("a", "剧情/回忆", "回忆", "//nas/music"));
        panel.setMode(WorkspacePanel::Folders);
        QCOMPARE(rows->count(), 1);
        QString library, path;
        connect(&panel, &WorkspacePanel::folderActivated, &panel, [&](const QString &l, const QString &p) {
            library = l;
            path = p;
        });
        rows->setCurrentRow(0);
        activateCurrentRow();
        QCOMPARE(library, QString("a"));
        QCOMPARE(path, QString("剧情/回忆"));
        QVERIFY(panel.width() <= 220);
    }
    void newerWorkspaceSchemaIsNotOverwritten() {
        QTemporaryDir data;
        const auto path = data.filePath("workspace.sqlite");
        const auto connection = QString("workspace-newer-test");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("PRAGMA user_version=9"));
        }
        QSqlDatabase::removeDatabase(connection);
        WorkspaceStore store(path);
        QVERIFY(!store.open());
        QVERIFY(store.lastError().contains("版本较新"));
        QVERIFY(store.candidateLists().isEmpty());
    }
};

QTEST_MAIN(WorkspaceTests)
#include "workspace_tests.moc"
