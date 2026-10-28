#include "failure_store.h"
#include "window.h"
#include "workers.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

class FailureUiTests : public QObject {
    Q_OBJECT
    static bool seed(const QString &root, bool failureRecords = true, bool referenceRecord = true) {
        Index db(QDir(root).filePath("library.sqlite"));
        if (!db.open() || !db.saveLibrary({"qa", "测试", root, ""}))
            return false;
        QList<Track> tracks;
        for (const auto &name : {"A-network.wav", "B-damaged.wav", "C-reference.wav"}) {
            Track track;
            track.libraryId = "qa";
            track.name = track.relativePath = QString::fromLatin1(name);
            track.folder = ".";
            track.size = 9644;
            track.modified = 1000;
            tracks << track;
        }
        if (!db.upsert(tracks))
            return false;
        tracks = db.search("").tracks;
        PeakBuilder builder(48000, 1);
        float samples[256] = {.2f};
        builder.append(samples, 256);
        for (int i = 0; i < tracks.size(); ++i)
            if (!saveWaveView(QDir(root).filePath("peaks"), WaveQueue::key(tracks[i]), builder.finish(),
                              i == 2))
                return false;
        FailureStore store(QDir(root).filePath("waveform-failures.sqlite"),
                           QDir(root).filePath("library.sqlite"));
        QString error;
        if (!store.open(&error))
            return false;
        QList<FailureOutcome> outcomes;
        if (failureRecords)
            outcomes = {{tracks[0], false, false, "source_unavailable", "原文件不可访问", 0},
                        {tracks[1], false, false, "invalid_audio", "音频有损坏", 0}};
        if (referenceRecord)
            outcomes << FailureOutcome{tracks[2], true, true, "reference", "仅供参考的波形", 0};
        return store.apply(outcomes, &error);
    }

    static void holdBackgroundWork(Window &window) {
        // Exercise the real audition-priority policy without opening any audio file.
        window.isPlaying_ = true;
        window.updateWorkPolicy();
    }
  private slots:
    void pathRepairDialogExplainsHardlinksAndLocalFallback() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path()); window.show();
        QTRY_VERIFY(window.indexReady_);
        window.showWaveFailures();
        auto *entry = window.failureDialog_->findChild<QPushButton *>("repairNasPaths");
        QVERIFY(entry); entry->click();
        auto *dialog = window.findChild<QDialog *>("nasPathRepairDialog");
        QVERIFY(dialog);
        QVERIFY(dialog->findChild<QLineEdit *>("pathRepairLocalCache"));
        bool explains = false;
        for (auto *label : dialog->findChildren<QLabel *>())
            explains |= label->text().contains("硬链接") && label->text().contains("本机");
        QVERIFY(explains);
    }
    void referenceDiagnosticsKeepAccessAndDecodeCausesSeparate() {
        QTemporaryDir temporary;
        QVERIFY(seed(temporary.path()));
        Index db(temporary.filePath("library.sqlite"));
        QVERIFY(db.open());
        const auto tracks = db.search("").tracks;
        FailureStore store(temporary.filePath("waveform-failures.sqlite"), temporary.filePath("library.sqlite"));
        QString error;
        QVERIFY(store.open(&error));
        QVERIFY(store.apply({{tracks[0], false, true, "source_unavailable", "读取中断，部分波形可用", 0},
                             {tracks[2], true, true, "decode_warning", "已解出可用音频，有异常", 0}}, &error));
        auto summary = store.summary(&error);
        QCOMPARE(summary["knownFailures"].toInteger(), 1);
        QCOMPARE(summary["knownReferences"].toInteger(), 2);
        QCOMPARE(summary["categories"].toObject()["damaged"].toInteger(), 1);
        QCOMPARE(summary["referenceCategories"].toObject()["network"].toInteger(), 1);
        QCOMPARE(summary["referenceCategories"].toObject()["decode_warning"].toInteger(), 1);
        const auto page = store.page(0, 100, "network", true, &error);
        const auto items = page["items"].toArray();
        QCOMPARE(items.size(), 1);
        QCOMPARE(items[0].toObject()["kind"].toString(), QString("source_unavailable"));
    }
    void removedItemsDoNotBlockLaterRetryPages_data() {
        QTest::addColumn<bool>("references");
        QTest::newRow("failures") << false;
        QTest::newRow("references") << true;
    }
    void removedItemsDoNotBlockLaterRetryPages() {
        QFETCH(bool, references);
        QTemporaryDir temporary;
        const auto root = temporary.filePath("音乐 记录");
        QVERIFY(QDir().mkpath(root));
        const auto indexPath = QDir(root).filePath("library.sqlite");
        Index db(indexPath);
        QVERIFY(db.open());
        QVERIFY(db.saveLibrary({"qa", "保留素材库", root, ""}));
        QVERIFY(db.saveLibrary({"removed", "移除素材库", root, ""}));
        QList<Track> tracks;
        for (int i = 0; i < 113; ++i) {
            Track track;
            track.libraryId = i == 111 ? "removed" : "qa";
            track.relativePath = i < 110 ? QString("excluded/%1.wav").arg(i)
                                         : QString("track-%1.wav").arg(i);
            track.name = QFileInfo(track.relativePath).fileName();
            track.folder = QFileInfo(track.relativePath).path();
            track.size = 9644;
            track.modified = 1000;
            tracks << track;
        }
        QVERIFY(db.upsert(tracks));
        tracks = db.tracksAfter(0, 200);
        QCOMPARE(tracks.size(), 113);
        FailureStore store(QDir(root).filePath("failures.sqlite"), indexPath);
        QString error;
        QVERIFY2(store.open(&error), qPrintable(error));
        QList<FailureOutcome> outcomes;
        for (const auto &track : tracks)
            outcomes << FailureOutcome{track, references, references, "timeout", "历史记录", 0};
        QVERIFY2(store.apply(outcomes, &error), qPrintable(error));
        QVERIFY(db.excludeFolder("qa", "excluded", &error));
        QVERIFY(db.excludeTrack("qa", "track-110.wav", &error));
        QVERIFY(db.removeLibrary("removed", &error));
        const auto upper = store.retryUpper(&error, references);
        const auto retry = store.retryPage(0, upper, 100, &error, references);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(retry.size(), 1);
        QCOMPARE(retry.first().track.relativePath, QString("track-112.wav"));
        auto summary = store.summary(&error);
        QCOMPARE(summary[references ? "knownReferences" : "knownFailures"].toInteger(), 1);
        QCOMPARE(summary["inactiveRecords"].toInteger(), 112);
        auto page = store.page(0, 100, {}, references, &error);
        QCOMPARE(page["total"].toInteger(), 1);
        QCOMPARE(page["items"].toArray().size(), 1);
        QVERIFY(!page["more"].toBool());
        // History survives exclusion; restoring catalog membership restores its record.
        QVERIFY(db.restoreExcludedItem("qa", "track-110.wav", false, &error));
        const auto restore = std::find_if(tracks.cbegin(), tracks.cend(), [](const Track &track) {
            return track.relativePath == "track-110.wav";
        });
        QVERIFY(restore != tracks.cend());
        QVERIFY(db.upsert({*restore}));
        QCOMPARE(store.summary(&error)["inactiveRecords"].toInteger(), 111);
        QCOMPARE(store.retryPage(0, upper, 100, &error, references).size(), 2);
    }
    void missingCatalogDoesNotCreateDatabaseOrUseSnapshot() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        const auto missing = data.filePath("missing catalog.sqlite");
        FailureStore store(data.filePath("waveform-failures.sqlite"), missing);
        QString error;
        QVERIFY(store.open(&error));
        const auto retry = store.retryPage(0, 1000, 100, &error);
        QVERIFY(retry.isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(missing));
        error.clear();
        QVERIFY(store.summary(&error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void emptyProfileCanInitializeBeforeCatalogExists() {
        QTemporaryDir data;
        const auto index = data.filePath("not-created.sqlite");
        FailureStore store(data.filePath("failures.sqlite"), index);
        QString error;
        QVERIFY(store.open(&error));
        QVERIFY(store.importLegacy({}, 0, &error));
        QCOMPARE(store.summary(&error)["knownFailures"].toInteger(), 0);
        QVERIFY(error.isEmpty());
        QVERIFY(!QFile::exists(index));
    }
    void unreadableCatalogReportsErrorWithoutDiscardingHistory() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        const auto broken = data.filePath("broken catalog.sqlite");
        QFile file(broken);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a SQLite database");
        file.close();
        QString error;
        {
            FailureStore store(data.filePath("waveform-failures.sqlite"), broken);
            QVERIFY(store.open(&error));
            QVERIFY(store.retryPage(0, 1000, 100, &error).isEmpty());
            QVERIFY(!error.isEmpty());
        }
        FailureStore original(data.filePath("waveform-failures.sqlite"), data.filePath("library.sqlite"));
        error.clear();
        QVERIFY(original.open(&error));
        QCOMPARE(original.summary(&error)["knownFailures"].toInteger(), 2);
        QVERIFY(error.isEmpty());
    }
    void catalogRefreshUpdatesFailureCountsWithoutReadingAudio() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        QFile checkpoint(data.filePath("progress.json"));
        QVERIFY(checkpoint.open(QIODevice::WriteOnly));
        checkpoint.write(QJsonDocument(QJsonObject{{"schema", 2}, {"mode", "failed"},
                                                   {"retryUpper", 2}, {"retryCursor", 2},
                                                   {"retryDone", 2}, {"retryTotal", 2}}).toJson());
        checkpoint.close();
        WaveQueue queue(data.filePath("peaks"));
        queue.setWorkContext(true, false, false);
        queue.configurePreparation(data.filePath("library.sqlite"), data.filePath("progress.json"));
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 2);
        Index db(data.filePath("library.sqlite"));
        QVERIFY(db.open());
        QVERIFY(db.removeLibrary("qa"));
        queue.refreshIndex();
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 0);
        QTRY_COMPARE(queue.preparation()["knownReferences"].toInteger(), 0);
        QCOMPARE(queue.preparation()["inactiveRecords"].toInteger(), 3);
        QCOMPARE(queue.preparation()["retryUpper"].toInteger(), 0);
        // Reintroduced catalog members must not be skipped by the old end cursor.
        QVERIFY(seed(data.path()));
        queue.refreshIndex();
        QTRY_COMPARE(queue.preparation()["knownFailures"].toInteger(), 2);
        queue.retryFailed();
        QTRY_COMPARE(queue.preparation()["retryTotal"].toInteger(), 2);
        QCOMPARE(queue.preparation()["retryCursor"].toInteger(), 0);
        QCOMPARE(queue.preparation()["retryDone"].toInteger(), 0);
        QVERIFY(queue.findChildren<QProcess *>().isEmpty());
    }
    void filterAndReferenceViewRejectLatePreviousPage() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && window.waveQueue_->preparation()["failuresReady"].toBool());
        QSignalSpy pages(window.waveQueue_, &WaveQueue::failurePageReady);
        window.showWaveFailures();
        auto *dialog = window.failureDialog_.data();
        QVERIFY(dialog);
        QVERIFY(dialog->findChild<QPushButton *>("repairNasPaths"));
        auto *table = dialog->findChild<QTableWidget *>("failureRecords");
        auto *category = dialog->findChild<QComboBox *>("failureCategory");
        auto *kind = dialog->findChild<QComboBox *>("failureRecordKind");
        QVERIFY(table && category && kind);
        QTRY_COMPARE(table->rowCount(), 2);
        QVERIFY(!pages.isEmpty());
        auto stale = pages.last()[0].toJsonObject();
        stale["items"] = QJsonArray{QJsonObject{{"name", "不应显示旧页面"}}};
        category->setCurrentIndex(category->findData("damaged"));
        window.waveQueue_->failurePageReady(stale);
        QVERIFY(table->item(0, 0)->text() != "不应显示旧页面");
        QTRY_COMPARE(table->rowCount(), 1);
        QCOMPARE(table->item(0, 0)->text(), QString("B-damaged.wav"));
        stale = pages.last()[0].toJsonObject();
        stale["items"] = QJsonArray{QJsonObject{{"name", "不应显示旧异常"}}};
        kind->setCurrentIndex(1);
        window.waveQueue_->failurePageReady(stale);
        QTRY_VERIFY(table->rowCount() == 1 && table->item(0, 0)->text() == "C-reference.wav");
        QVERIFY(category->isEnabled());
        category->setCurrentIndex(category->findData("network"));
        QTRY_COMPARE(table->rowCount(), 0);
        window.showWaveFailures();
        QCOMPARE(window.failureDialog_.data(), dialog);
        dialog->close();
        QTRY_VERIFY(!window.failureDialog_);
    }
    void historicalGapAndRetryReadinessAreVisible() {
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        QFile state(data.filePath("wave-preparation.json"));
        QVERIFY(state.open(QIODevice::WriteOnly));
        state.write(QJsonDocument(QJsonObject{{"schema", 2}, {"failed", 8}, {"reference", 7}}).toJson());
        state.close();
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && window.waveQueue_->preparation()["failuresReady"].toBool());
        QTRY_COMPARE(window.waveQueue_->preparation()["legacyUntracked"].toInteger(), 6);
        window.showWaveFailures();
        auto *dialog = window.failureDialog_.data();
        QVERIFY(dialog);
        auto *retry = dialog->findChild<QPushButton *>("retryFailedWaves");
        QVERIFY(retry && retry->isEnabled());
        bool gap = false, referenceNotice = false;
        for (const auto *label : dialog->findChildren<QLabel *>()) {
            gap |= label->text().contains("6 条历史失败没有文件明细");
            referenceNotice |= label->text().contains("旧版参考波形明细");
        }
        QVERIFY(gap && referenceNotice);
        holdBackgroundWork(window);
        retry->click();
        QTRY_VERIFY(window.waveQueue_->preparation()["running"].toBool());
        QVERIFY(!retry->isEnabled());
        QCOMPARE(window.waveQueue_->preparation()["mode"].toString(), QString("failed"));
        window.waveQueue_->pausePreparation();
        QTRY_VERIFY(retry->isEnabled());
    }
    void settingsReferenceActionKeepsRateAndResumes_data() {
        QTest::addColumn<qint64>("rate");
        QTest::newRow("default-low-impact") << qint64(1048576);
        QTest::newRow("configured-faster") << qint64(10 * 1048576);
    }
    void settingsReferenceActionKeepsRateAndResumes() {
        QFETCH(qint64, rate);
        QTemporaryDir data;
        QVERIFY(seed(data.path()));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && window.waveQueue_->preparation()["failuresReady"].toBool());
        window.showSettings(1);
        auto *settings = window.settingsDialog_;
        QVERIFY(settings);
        auto *reanalyze = settings->findChild<QPushButton *>("reanalyzeReferenceWaves");
        auto *pause = settings->findChild<QPushButton *>("pauseWavePreparation");
        auto *speed = settings->findChild<QComboBox *>("backgroundReadRate");
        QVERIFY(reanalyze && pause && speed);
        QCOMPARE(reanalyze->text(), QString("重新分析参考波形"));
        QVERIFY(reanalyze->isEnabled());
        QCOMPARE(speed->currentData().toLongLong(), qint64(1048576));
        const int rateIndex = speed->findData(rate);
        QVERIFY(rateIndex >= 0);
        speed->setCurrentIndex(rateIndex);
        holdBackgroundWork(window);

        reanalyze->click();
        QTRY_COMPARE(window.waveQueue_->preparation()["retryTotal"].toInteger(), 1);
        auto state = window.waveQueue_->preparation();
        QCOMPARE(state["mode"].toString(), QString("reference"));
        QCOMPARE(state["rate"].toInteger(), rate);
        QVERIFY(state["running"].toBool() && state["deferred"].toBool());
        QCOMPARE(state["retryDone"].toInteger(), 0);
        QCOMPARE(state["knownFailures"].toInteger(), 2);
        QVERIFY(state["currentPath"].toString().isEmpty());
        QCOMPARE(state["readBytes"].toInteger(), 0);
        QVERIFY(window.waveQueue_->findChildren<QProcess *>().isEmpty());
        QVERIFY(!window.player_);
        QVERIFY(!reanalyze->isEnabled());
        QVERIFY(speed->isEnabled());
        QVERIFY(pause->isEnabled());

        const auto upper = state["retryUpper"].toInteger();
        QVERIFY(upper > 0);
        // Changes apply immediately to a running (deferred) task without resetting its range.
        const qint64 updatedRate = rate == 1048576 ? 10 * 1048576 : 1048576;
        speed->setCurrentIndex(speed->findData(updatedRate));
        QTRY_COMPARE(window.waveQueue_->preparation()["rate"].toInteger(), updatedRate);
        QCOMPARE(window.waveQueue_->preparation()["retryUpper"].toInteger(), upper);
        QCOMPARE(window.waveQueue_->preparation()["retryDone"].toInteger(), 0);
        QCOMPARE(window.waveQueue_->preparation()["mode"].toString(), QString("reference"));
        pause->click();
        QTRY_VERIFY(!window.waveQueue_->preparation()["running"].toBool());
        QVERIFY(reanalyze->isEnabled() && speed->isEnabled());
        QVERIFY(!pause->isEnabled());
        reanalyze->click();
        QTRY_VERIFY(window.waveQueue_->preparation()["running"].toBool());
        state = window.waveQueue_->preparation();
        QCOMPARE(state["mode"].toString(), QString("reference"));
        QCOMPARE(state["rate"].toInteger(), updatedRate);
        QCOMPARE(state["retryUpper"].toInteger(), upper);
        QCOMPARE(state["retryTotal"].toInteger(), 1);
        QCOMPARE(state["retryDone"].toInteger(), 0);
        QVERIFY(state["deferred"].toBool());
        QVERIFY(window.waveQueue_->findChildren<QProcess *>().isEmpty());
        pause->click();
    }
    void recordKindControlsReadinessAndRetryMode_data() {
        QTest::addColumn<bool>("failures");
        QTest::addColumn<bool>("references");
        QTest::newRow("both-kinds") << true << true;
        QTest::newRow("failures-only") << true << false;
        QTest::newRow("references-only") << false << true;
    }
    void recordKindControlsReadinessAndRetryMode() {
        QFETCH(bool, failures);
        QFETCH(bool, references);
        QTemporaryDir data;
        QVERIFY(seed(data.path(), failures, references));
        Window window(data.path());
        window.show();
        QTRY_VERIFY(window.indexReady_ && window.waveQueue_->preparation()["failuresReady"].toBool());
        auto *settingsReference = window.settingsDialog_->findChild<QPushButton *>("reanalyzeReferenceWaves");
        QVERIFY(settingsReference);
        QCOMPARE(settingsReference->isEnabled(), references);
        window.preparationSpeed_->setCurrentIndex(window.preparationSpeed_->findData(10 * 1048576));
        holdBackgroundWork(window);
        window.showWaveFailures();
        auto *dialog = window.failureDialog_.data();
        QVERIFY(dialog);
        auto *retry = dialog->findChild<QPushButton *>("retryFailedWaves");
        auto *kind = dialog->findChild<QComboBox *>("failureRecordKind");
        QVERIFY(retry && kind);
        QCOMPARE(retry->text(), QString("只重试失败"));
        QCOMPARE(retry->isEnabled(), failures);

        kind->setCurrentIndex(kind->findData(true));
        QTRY_COMPARE(retry->text(), QString("重新分析参考波形"));
        QCOMPARE(retry->isEnabled(), references);
        if (references) {
            retry->click();
            QTRY_COMPARE(window.waveQueue_->preparation()["retryTotal"].toInteger(), 1);
            const auto state = window.waveQueue_->preparation();
            QCOMPARE(state["mode"].toString(), QString("reference"));
            QCOMPARE(state["rate"].toInteger(), qint64(10 * 1048576));
            QVERIFY(state["running"].toBool() && state["deferred"].toBool());
            QVERIFY(!retry->isEnabled());
            QCOMPARE(state["retryDone"].toInteger(), 0);
            QVERIFY(window.waveQueue_->findChildren<QProcess *>().isEmpty());
            kind->setCurrentIndex(kind->findData(false));
            QVERIFY(!retry->isEnabled());
            QCOMPARE(window.waveQueue_->preparation()["mode"].toString(), QString("reference"));
            kind->setCurrentIndex(kind->findData(true));
            window.waveQueue_->pausePreparation();
            QTRY_VERIFY(retry->isEnabled());
        }

        kind->setCurrentIndex(kind->findData(false));
        QTRY_COMPARE(retry->text(), QString("只重试失败"));
        QCOMPARE(retry->isEnabled(), failures);
        if (failures) {
            retry->click();
            QTRY_COMPARE(window.waveQueue_->preparation()["retryTotal"].toInteger(), 2);
            const auto state = window.waveQueue_->preparation();
            QCOMPARE(state["mode"].toString(), QString("failed"));
            QCOMPARE(state["rate"].toInteger(), qint64(10 * 1048576));
            QVERIFY(state["running"].toBool() && state["deferred"].toBool());
            QVERIFY(!retry->isEnabled());
            QCOMPARE(state["retryDone"].toInteger(), 0);
            QVERIFY(window.waveQueue_->findChildren<QProcess *>().isEmpty());
            window.waveQueue_->pausePreparation();
        }
        QVERIFY(!window.player_);
    }
};
int main(int argc, char **argv) {
    if (argc > 2 && QByteArray(argv[1]) == "--worker") {
        QCoreApplication app(argc, argv);
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    FailureUiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "failure_ui_tests.moc"
