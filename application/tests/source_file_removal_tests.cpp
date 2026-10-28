#include "source_file_removal.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QtTest>

class SourceFileRemovalTests : public QObject {
    Q_OBJECT
  private slots:
    void validatesOnlyExistingItemsInsideTheLibrary() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        const auto libraryRoot = QDir(data.path()).filePath("music");
        QVERIFY(QDir().mkpath(QDir(libraryRoot).filePath("scene")));
        QFile song(QDir(libraryRoot).filePath("scene/cue.wav"));
        QVERIFY(song.open(QIODevice::WriteOnly));
        song.write("sample");
        song.close();
        QFile outside(QDir(data.path()).filePath("outside.wav"));
        QVERIFY(outside.open(QIODevice::WriteOnly));
        outside.close();
        const Library library{"qa", "music", libraryRoot, ""};
        QString error;
        QCOMPARE(QFileInfo(validatedOriginalPath(library, "scene/cue.wav", false, &error))
                     .canonicalFilePath(),
                 QFileInfo(song.fileName()).canonicalFilePath());
        QVERIFY(error.isEmpty());
        QCOMPARE(QFileInfo(validatedOriginalPath(library, "scene", true, &error))
                     .canonicalFilePath(),
                 QFileInfo(QDir(libraryRoot).filePath("scene")).canonicalFilePath());
        for (const auto &invalid : {QString(), QString("."), QString("../outside.wav"),
                                    QString("scene/../../outside.wav"), QString("scene\\cue.wav"),
                                    QString("scene/missing.wav")}) {
            QVERIFY2(validatedOriginalPath(library, invalid, false, &error).isEmpty(),
                     qPrintable(invalid));
            QVERIFY(!error.isEmpty());
        }
        QVERIFY(validatedOriginalPath(library, "scene", false, &error).isEmpty());
        QVERIFY(validatedOriginalPath(library, "scene/cue.wav", true, &error).isEmpty());
        QVERIFY(song.exists());
        QVERIFY(outside.exists());
    }

    void refusesWholeLibraryWhenItContainsAnotherLibraryOrSharedCache() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        const auto root = data.filePath("music");
        const auto nested = QDir(root).filePath("nested");
        const auto cache = QDir(root).filePath("audio-cache");
        QVERIFY(QDir().mkpath(nested));
        QVERIFY(QDir().mkpath(cache));
        const Library music{"music", "music", root, ""};
        const Library child{"child", "child", nested, ""};
        QString error;
        QVERIFY(!moveOriginalToTrash(music, "nested", true, {music, child}, &error));
        QVERIFY(error.contains("另一个"));
        QVERIFY(QDir(nested).exists());
        QVERIFY(!moveLibraryRootToTrash(music, {music, child}, &error));
        QVERIFY(error.contains("另一个"));
        QVERIFY(QDir(root).exists());
        const Library cached{"music", "music", root, cache};
        QVERIFY(!moveLibraryRootToTrash(cached, {cached}, &error));
        QVERIFY(error.contains("缓存"));
        QVERIFY(QDir(root).exists());
    }

    void aVolumeRootMayDescribeChildrenButCannotBeTrashed() {
        QTemporaryDir data;
        QVERIFY(data.isValid());
        QFile song(data.filePath("root-boundary.wav"));
        QVERIFY(song.open(QIODevice::WriteOnly));
        song.close();
        const auto volumeRoot = QStorageInfo(data.path()).rootPath();
        QVERIFY(!volumeRoot.isEmpty());
        const Library volume{"volume", "volume", volumeRoot, ""};
        // macOS temporary paths may use /var -> /private/var. Resolve that system
        // alias so this case tests a volume descendant without a symlink component.
        const auto relative =
            QDir(volumeRoot).relativeFilePath(QFileInfo(song.fileName()).canonicalFilePath());
        QString error;
        QCOMPARE(QFileInfo(validatedOriginalPath(volume, relative, false, &error))
                     .canonicalFilePath(),
                 QFileInfo(song.fileName()).canonicalFilePath());
        QVERIFY(error.isEmpty());
        QVERIFY(!moveLibraryRootToTrash(volume, {volume}, &error));
        QVERIFY(!error.isEmpty());
        const Library nested{"nested", "nested", data.path(), ""};
        QVERIFY(!moveOriginalToTrash(nested, "root-boundary.wav", false, {nested, volume}, &error));
        QVERIFY(error.contains("另一个"));
        QVERIFY(song.exists());
    }
};

QTEST_MAIN(SourceFileRemovalTests)
#include "source_file_removal_tests.moc"
