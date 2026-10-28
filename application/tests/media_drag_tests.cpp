#include "media_drag.h"
#include "workspace_panel.h"
#include "window.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

namespace {
const QByteArray abcSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
bool writeFile(const QString &path, const QByteArray &data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
Track seed(Index &db, const QString &root) {
    Track track;
    track.libraryId = "library";
    track.relativePath = "日本語/原曲.wav";
    track.name = "原曲.wav";
    track.folder = "日本語";
    track.size = 0;
    track.modified = 1000;
    if (!db.open() || !db.saveLibrary({"library", "Music", root, ""}) || !db.upsert({track}))
        return {};
    return db.trackByIdentity("library", track.relativePath).value_or(Track{});
}
} // namespace

class MediaDragTests : public QObject {
    Q_OBJECT
  private slots:
    void staleWorkspaceSnapshotAndModelMimeUseCurrentVerifiedCopy() {
        QTemporaryDir temp;
        const auto database = temp.filePath("index.sqlite");
        const auto copy = temp.filePath("safe.wav");
        QVERIFY(writeFile(copy, "abc"));
        Index db(database);
        const auto snapshot = seed(db, temp.filePath("offline-originals"));
        QVERIFY(snapshot.id);
        QVERIFY(db.setCompatibleSource(snapshot, copy, abcSha));
        WorkspaceTrackList list;
        list.setCatalogPath(database);
        list.setTrackRows({snapshot});
        list.setCurrentRow(0);
        std::unique_ptr<QMimeData> workspaceMime(list.originalMimeData());
        QCOMPARE(workspaceMime->urls(), QList<QUrl>{QUrl::fromLocalFile(copy)});
        TrackModel model;
        model.setCatalogPath(database);
        model.replace({snapshot});
        std::unique_ptr<QMimeData> modelMime(model.mimeData({model.index(0, TrackModel::Name)}));
        QCOMPARE(modelMime->urls(), QList<QUrl>{QUrl::fromLocalFile(copy)});
        auto future = verifiedMediaUrlsAsync({snapshot}, database);
        future.waitForFinished();
        const auto result = future.result();
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.urls, QList<QUrl>{QUrl::fromLocalFile(copy)});
        QString error;
        QCOMPARE(prepareMediaDragUrls(nullptr, {snapshot}, database, &error),
                 QList<QUrl>{QUrl::fromLocalFile(copy)});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(originalFileUrls({snapshot}),
                 QList<QUrl>{QUrl::fromLocalFile(temp.filePath("offline-originals/日本語/原曲.wav"))});
        QCOMPARE(snapshot.absolutePath(), temp.filePath("offline-originals/日本語/原曲.wav"));
    }
    void sameMetadataMiddleCorruptionBlocksVerifiedDragUrls() {
        QTemporaryDir temp;
        const auto database = temp.filePath("index.sqlite");
        const auto copy = temp.filePath("safe.wav");
        const QByteArray bytes(256 * 1024, 'a');
        QVERIFY(writeFile(copy, bytes));
        Index db(database);
        const auto snapshot = seed(db, temp.filePath("offline-originals"));
        QVERIFY(snapshot.id);
        QVERIFY(db.setCompatibleSource(snapshot, copy,
                                       QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)));
        const auto modified = QFileInfo(copy).lastModified();
        QFile file(copy);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.seek(130000));
        QCOMPARE(file.write("b"), qint64(1));
        QVERIFY(file.flush());
        QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime));
        file.close();
        QCOMPARE(QFileInfo(copy).size(), qint64(bytes.size()));
        QCOMPARE(QFileInfo(copy).lastModified().toMSecsSinceEpoch(), modified.toMSecsSinceEpoch());
        auto future = verifiedMediaUrlsAsync({snapshot}, database);
        future.waitForFinished();
        QVERIFY(future.result().urls.isEmpty());
        QVERIFY(!future.result().error.isEmpty());
        QString error;
        QVERIFY(prepareMediaDragUrls(nullptr, {snapshot}, database, &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void excludedCurrentTrackCannotFallBackToStaleWorkspacePath() {
        QTemporaryDir temp;
        const auto database = temp.filePath("index.sqlite");
        Index db(database);
        const auto snapshot = seed(db, temp.filePath("offline-originals"));
        QVERIFY(snapshot.id);
        QVERIFY(db.excludeTrack(snapshot.libraryId, snapshot.relativePath));
        auto future = verifiedMediaUrlsAsync({snapshot}, database);
        future.waitForFinished();
        QVERIFY(future.result().urls.isEmpty());
        QVERIFY(!future.result().error.isEmpty());
    }
};

QTEST_MAIN(MediaDragTests)
#include "media_drag_tests.moc"
