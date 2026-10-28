#include "folder_model.h"
#include <QPersistentModelIndex>
#include <QTemporaryDir>
#include <QtTest>

class FolderModelRemovalTests : public QObject {
    Q_OBJECT
  private slots:
    void removingOneLibraryPreservesOtherLibraryIndex() {
        QTemporaryDir data;
        FolderModel model(data.filePath("index.sqlite"));
        model.setLibraries({{"a", "A", data.path(), ""}, {"b", "B", data.path(), ""}});
        QCOMPARE(model.rowCount(), 4); // two virtual scopes, then two libraries
        const QPersistentModelIndex b(model.index(3, 0));
        QCOMPARE(b.data(FolderModel::LibraryRole).toString(), QString("b"));
        model.setLibraries({{"b", "B", data.path(), ""}});
        QCOMPARE(model.rowCount(), 3);
        QVERIFY(b.isValid());
        QCOMPARE(b.data(FolderModel::LibraryRole).toString(), QString("b"));
        QCOMPARE(model.index(2, 0).data(FolderModel::LibraryRole).toString(), QString("b"));
    }

    void removingFolderPrunesOnlyItsSubtree() {
        QTemporaryDir data;
        FolderModel model(data.filePath("index.sqlite"));
        model.setLibraries({{"a", "A", data.path(), ""}});
        const auto a = model.index(2, 0);
        const auto first = model.materialize({{"a", "", "", "A", "A", true},
                                              {"a", "scene", "", "scene", "A", true},
                                              {"a", "scene/child", "scene", "child", "A", false}});
        QVERIFY(first.isValid());
        const QPersistentModelIndex other(model.materialize({{"a", "", "", "A", "A", true},
                                                              {"a", "other", "", "other", "A", false}}));
        QCOMPARE(model.rowCount(a), 2);
        QVERIFY(model.removeFolderSubtree("a", "scene"));
        QCOMPARE(model.rowCount(a), 1);
        QVERIFY(other.isValid());
        QCOMPARE(other.data(FolderModel::PathRole).toString(), QString("other"));
        QVERIFY(!model.removeFolderSubtree("a", "scene"));
        QVERIFY(!model.removeFolderSubtree("a", ""));
    }

    void pendingPageCannotBringBackExcludedFolder() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        {
            Index db(data.filePath("index.sqlite"));
            QVERIFY(db.open());
            QVERIFY(db.saveLibrary({"a", "A", data.path(), ""}));
            QVERIFY(db.upsertFolders("a", {"hidden", "visible"}));
        }
        FolderModel model(data.filePath("index.sqlite"));
        model.setLibraries({{"a", "A", data.path(), ""}});
        const auto library = model.index(2, 0);
        model.fetchMore(library);
        {
            Index db(data.filePath("index.sqlite"));
            QVERIFY(db.open());
            QVERIFY(db.excludeFolder("a", "hidden"));
        }
        model.removeFolderSubtree("a", "hidden");
        QTRY_COMPARE(model.rowCount(library), 1);
        QCOMPARE(model.index(0, 0, library).data(FolderModel::PathRole).toString(),
                 QString("visible"));
        QTest::qWait(50);
        QCOMPARE(model.rowCount(library), 1);
    }
};

QTEST_MAIN(FolderModelRemovalTests)
#include "folder_model_removal_tests.moc"
