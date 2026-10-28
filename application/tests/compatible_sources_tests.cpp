#include "index.h"
#include "media_source.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

namespace {
const QByteArray abcSha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
bool writeFile(const QString &path, const QByteArray &data) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
Track indexedTrack(const QString &root) {
    Track track;
    track.libraryId = "library";
    track.relativePath = "日文/原曲.mp3";
    track.name = "原曲.mp3";
    track.folder = "日文";
    track.root = root;
    // An SMB-indexed spelling can be unreadable now. Registering a previously verified
    // copy must bind to this catalog version without requiring stat() of the original.
    track.size = 987654;
    track.modified = 123456789;
    return track;
}
} // namespace

class CompatibleSourcesTests : public QObject {
    Q_OBJECT
  private slots:
    void registeringVerifiedMappingSeedsTheProfileReceipt() {
        QTemporaryDir temp;
        const auto copy = temp.filePath("compatible.mp3");
        const auto database = temp.filePath("index.sqlite");
        QVERIFY(writeFile(copy, "abc"));
        {
            Index db(database);
            QVERIFY(db.open());
            QVERIFY(db.saveLibrary({"library", "Music", temp.path(), ""}));
            QVERIFY(db.upsert({indexedTrack(temp.path())}));
            const auto original = *db.trackByIdentity("library", "日文/原曲.mp3");
            QVERIFY(db.setCompatibleSource(original, copy, abcSha));
        }
        Index reopened(database);
        QVERIFY(reopened.open());
        const auto mapped = reopened.trackByIdentity("library", "日文/原曲.mp3");
        QVERIFY(mapped);
        qint64 read = 0;
        QString error;
        QCOMPARE(verifiedMediaPath(*mapped, &error, [&](qint64 bytes) { read = qMax(read, bytes); },
                                   temp.filePath("media-verification.sqlite")), copy);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(read, qint64(0));
    }
    void compatibleSourceKeepsOriginalIdentityAndPersonalMetadata() {
        QTemporaryDir temp;
        const auto root = temp.filePath("originals");
        QVERIFY(QDir().mkpath(root));
        const auto copy = temp.filePath("compatible.mp3");
        QVERIFY(writeFile(copy, "abc"));
        const auto database = temp.filePath("index.db");
        Track original;
        {
            Index db(database);
            QVERIFY(db.open());
            QVERIFY(db.saveLibrary({"library", "Music", root, ""}));
            QVERIFY(db.upsert({indexedTrack(root)}));
            original = *db.trackByIdentity("library", "日文/原曲.mp3");
            QVERIFY(db.setPersonal(original, true, "第七集"));
            QVERIFY(db.setDuration(original, 15000));
            QString error;
            QVERIFY2(db.setCompatibleSource(original, copy, abcSha, &error), qPrintable(error));
        }
        Index reopened(database);
        QVERIFY(reopened.open());
        const auto stored = reopened.trackByIdentity("library", "日文/原曲.mp3");
        QVERIFY(stored);
        QCOMPARE(stored->id, original.id);
        QCOMPARE(stored->size, qint64(987654));
        QCOMPARE(stored->modified, qint64(123456789));
        QCOMPARE(stored->duration, qint64(15000));
        QVERIFY(stored->favorite);
        QCOMPARE(stored->tags, QString("第七集"));
        QCOMPARE(stored->absolutePath(), root + "/日文/原曲.mp3");
        QCOMPARE(stored->sourcePath(), copy);
        QCOMPARE(stored->compatibleSha256, abcSha);
        QCOMPARE(originalFileUrls({*stored}), QList<QUrl>{QUrl::fromLocalFile(root + "/日文/原曲.mp3")});
        QVERIFY(stored->sourceMatches());
        const auto searched = reopened.search("第七集", {}, {}, true);
        QCOMPARE(searched.tracks.size(), 1);
        QCOMPARE(searched.tracks.first().sourcePath(), copy);
        const auto batch = reopened.tracksAfter(0);
        QCOMPARE(batch.size(), 1);
        QCOMPARE(batch.first().sourcePath(), copy);
    }
    void incorrectFullHashCannotReplaceVerifiedMapping() {
        QTemporaryDir temp;
        const auto copy = temp.filePath("compatible.mp3");
        const auto badCopy = temp.filePath("unverified.mp3");
        QVERIFY(writeFile(copy, "abc"));
        QVERIFY(writeFile(badCopy, "abd"));
        const auto database = temp.filePath("index.db");
        Index db(database);
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library", "Music", temp.path(), ""}));
        QVERIFY(db.upsert({indexedTrack(temp.path())}));
        const auto original = *db.trackByIdentity("library", "日文/原曲.mp3");
        QVERIFY(db.setCompatibleSource(original, copy, abcSha));
        QString error;
        QVERIFY(!db.setCompatibleSource(original, badCopy, abcSha, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(db.trackByIdentity("library", "日文/原曲.mp3")->sourcePath(), copy);
        QVERIFY(!db.setCompatibleSource(original, copy, "not-a-sha256", &error));
    }
    void changedCatalogVersionInvalidatesMappingAndRejectsOldRepair() {
        QTemporaryDir temp;
        const auto copy = temp.filePath("compatible.mp3");
        QVERIFY(writeFile(copy, "abc"));
        Index db(temp.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library", "Music", temp.path(), ""}));
        QVERIFY(db.upsert({indexedTrack(temp.path())}));
        const auto original = *db.trackByIdentity("library", "日文/原曲.mp3");
        QVERIFY(db.setCompatibleSource(original, copy, abcSha));
        auto changed = original;
        ++changed.modified;
        QVERIFY(db.upsert({changed}));
        auto current = *db.trackByIdentity("library", "日文/原曲.mp3");
        QVERIFY(current.compatiblePath.isEmpty());
        QCOMPARE(current.sourcePath(), current.absolutePath());
        QString error;
        QVERIFY(!db.setCompatibleSource(original, copy, abcSha, &error));
        QVERIFY(!error.isEmpty());
        // Even if an older app later scans the old size/stamp again, the invalidated
        // association must not silently come back to life.
        QVERIFY(db.upsert({original}));
        QVERIFY(db.trackByIdentity("library", "日文/原曲.mp3")->compatiblePath.isEmpty());
    }
    void rootRebindCannotReuseMappingForDifferentOriginals() {
        QTemporaryDir temp;
        const auto copy = temp.filePath("compatible.mp3");
        QVERIFY(writeFile(copy, "abc"));
        const auto database = temp.filePath("index.db");
        Index db(database);
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library", "Music", temp.filePath("A"), ""}));
        QVERIFY(db.upsert({indexedTrack(temp.filePath("A"))}));
        const auto original = *db.trackByIdentity("library", "日文/原曲.mp3");
        QVERIFY(db.setCompatibleSource(original, copy, abcSha));
        // The public configuration guard already prevents this. Simulate a catalog
        // restored/rebound by an older client to verify the mapping's own root guard.
        const auto name = QStringLiteral("rebound-catalog");
        {
            auto connection = QSqlDatabase::addDatabase("QSQLITE", name);
            connection.setDatabaseName(database);
            QVERIFY(connection.open());
            QSqlQuery query(connection);
            query.prepare("UPDATE libraries SET root=? WHERE id='library'");
            query.addBindValue(temp.filePath("B"));
            QVERIFY(query.exec());
        }
        QSqlDatabase::removeDatabase(name);
        const auto rebound = *db.trackByIdentity("library", "日文/原曲.mp3");
        QVERIFY(rebound.compatiblePath.isEmpty());
        QVERIFY(!db.setCompatibleSource(original, copy, abcSha));
    }
    void missingOrChangedCompatibleFileFailsValidation() {
        QTemporaryDir temp;
        const auto copy = temp.filePath("compatible.mp3");
        QVERIFY(writeFile(copy, "abc"));
        Index db(temp.filePath("index.db"));
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"library", "Music", temp.path(), ""}));
        QVERIFY(db.upsert({indexedTrack(temp.path())}));
        const auto original = *db.trackByIdentity("library", "日文/原曲.mp3");
        QVERIFY(db.setCompatibleSource(original, copy, abcSha));
        const auto mapped = *db.trackByIdentity("library", "日文/原曲.mp3");
        QVERIFY(mapped.sourceMatches());
        QVERIFY(writeFile(copy, "abcd"));
        QString error;
        QVERIFY(!mapped.sourceMatches(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(QFile::remove(copy));
        QVERIFY(!mapped.sourceMatches(&error));
        // No mapping does not probe an unavailable original in the UI.
        QVERIFY(original.sourceMatches());
    }
    void additiveMappingDoesNotRaiseCatalogVersion() {
        QTemporaryDir temp;
        const auto database = temp.filePath("index.db");
        Index db(database);
        QVERIFY(db.open());
        // Read the real catalog to protect rollback compatibility with 0.12.2.
        const auto name = QStringLiteral("version-check");
        {
            auto connection = QSqlDatabase::addDatabase("QSQLITE", name);
            connection.setDatabaseName(database);
            QVERIFY(connection.open());
            QSqlQuery query(connection);
            QVERIFY(query.exec("PRAGMA user_version"));
            QVERIFY(query.next());
            QCOMPARE(query.value(0).toInt(), 6);
        }
        QSqlDatabase::removeDatabase(name);
    }
};

QTEST_GUILESS_MAIN(CompatibleSourcesTests)
#include "compatible_sources_tests.moc"
