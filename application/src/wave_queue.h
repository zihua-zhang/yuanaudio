#pragma once
#include "cache.h"
#include "failure_store.h"
#include "index.h"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <array>
#include <memory>

struct RowWave {
    WaveData data;
    QString message, failureKind;
    bool complete = false, failed = false, approximate = false;
    quint64 touched = 0;
    qint64 estimatedDuration = 0;
    bool stored = false;
    bool verificationDue = false;
};

// A priority worker plus a bounded, ordered background pool. No source I/O on the GUI thread.
class WaveQueue : public QObject {
    Q_OBJECT
    friend class WindowTests;
    friend class QueueRefreshTests;
    friend class QueuePolicyTests;

  public:
    explicit WaveQueue(QString localCache, QObject *parent = nullptr);
    ~WaveQueue() override;
    void setLibraries(const QList<Library> &libraries);
    void setVisible(const QList<Track> &tracks);
    void select(const Track &track, bool retry = false);
    void invalidate();
    void refreshIndex(bool membershipChanged = true);
    const RowWave *find(const Track &track) const;
    static QString key(const Track &track);
    void configurePreparation(const QString &database, const QString &stateFile);
    void prepareAll(qint64 bytesPerSecond = 10 * 1048576, bool restart = false);
    void pausePreparation();
    void setPreparationConcurrency(int count);
    void setWorkContext(bool playing, bool scanning, bool editingBusy);
    bool hasBackgroundWork() const;
    // -1 retains the current setting; zero explicitly means unthrottled.
    void retryFailed(qint64 bytesPerSecond = -1);
    void retryReferences(qint64 bytesPerSecond = -1);
    quint64 requestFailurePage(int offset = 0, int limit = 100, const QString &category = {},
                               bool references = false);
    QJsonObject preparation() const;
  public slots:
    void skipCurrent();
  signals:
    void changed(const Track &track);
    void warning(const Track &track, const QString &message);
    void preparationChanged();
    void failurePageReady(const QJsonObject &page);
    void jobFinished(const Track &track, bool success);

  private:
    QString localCache_;
    QList<Library> libraries_;
    QList<Track> visible_;
    Track selected_, active_;
    QHash<QString, RowWave> entries_;
    QProcess *process_ = nullptr;
    bool cancelled_ = false, background_ = false;
    struct ParallelSlot {
        WaveQueue *worker = nullptr;
        Track track;
        bool done = false, success = false;
    };
    std::array<ParallelSlot, 4> parallel_{};
    int preparationConcurrency_ = 1;
    bool parallelWorker_ = false, parallelPublish_ = false;
    void scheduleParallel();
    void stopParallel(bool discardCompleted = false);
    bool parallelOwns(const Track &track) const;
    int parallelActiveCount() const;
    bool drainParallel();
    qint64 backgroundJobRate() const;
    quint64 clock_ = 0;
    QThreadPool localPool_;
    QThreadPool failurePool_;
    QSet<QString> localChecked_;
    qint64 verifyViewsBefore_ = 0;
    std::shared_ptr<std::atomic<quint64>> localEpoch_ = std::make_shared<std::atomic<quint64>>(0);
    void readLocal();
    QString preparationDatabase_, preparationFile_, preparationError_;
    QString failureFile_, deferReason_;
    bool contextPlaying_ = false, contextScanning_ = false, contextBusy_ = false;
    bool failuresReady_ = false, retryMode_ = false, retryActive_ = false, retryLoading_ = false;
    bool retryComplete_ = false;
    bool retryReferences_ = false, retryLocalLoading_ = false;
    QString retryFallbackKey_;
    qint64 retryCursor_ = 0, retryUpper_ = 0, retryDone_ = 0, retryFailed_ = 0, retryTotal_ = 0;
    qint64 retryConverted_ = 0, retryRemaining_ = 0;
    QList<FailureRecord> retryPage_;
    QJsonObject failureSummary_;
    QList<FailureOutcome> pendingOutcomes_;
    struct FailureWrites {
        // The single failure-pool thread owns the backlog. The GUI only reads the retry flag.
        QList<FailureOutcome> pending;
        std::atomic_bool retryNeeded{false};
    };
    std::shared_ptr<FailureWrites> failureWrites_ = std::make_shared<FailureWrites>();
    bool failureWriting_ = false, checkpointDirty_ = false;
    static constexpr int OutcomeChunkSize = 50;
    QTimer failureFlush_, workerWatchdog_;
    bool waiting_ = false;
    bool publishing_ = false;
    bool preserveActiveView_ = false;
    quint64 failureQueryRevision_ = 0;
    qint64 waitDeadline_ = 0;
    void initializeFailures();
    void flushOutcomes();
    void persistOutcome(const Track &track, bool success, bool reference, qint64 retryId = 0);
    void fetchRetries(bool start = false);
    void startRetry(bool references, qint64 bytesPerSecond);
    void loadReferenceFallback(const Track &track);
    void advanceRetry(const Track &track, bool success, bool reference);
    QJsonArray recentFailures_;
    qint64 readBytes_ = 0, decodedMs_ = 0;
    QList<QPair<Track, int>> batchPage_;
    qint64 batchCursor_ = 0, batchDone_ = 0, batchFailed_ = 0, batchTotal_ = 0;
    qint64 batchReference_ = 0;
    qint64 batchRate_ = 10 * 1048576;
    qint64 activeRate_ = 0;
    bool batchRunning_ = false, batchComplete_ = false, batchLoading_ = false, batchActive_ = false;
    bool catalogNeedsReview_ = false;
    quint64 batchEpoch_ = 0;
    void fetchBatch();
    void savePreparation();
    void advanceBatch(const Track &track, bool success, bool reference = false);
    void finishJob(bool success);
    void startProcess();
    QElapsedTimer jobClock_, activityClock_;
    QByteArray pending_;
    QString workerProgram_;
    void schedule();
    void cancel();
    void trim();
};
