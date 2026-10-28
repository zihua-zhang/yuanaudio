#include "wave_queue.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QtConcurrent>
#include <memory>

namespace {
qint64 preparationRate(qint64 rate) {
    return rate == 0 ? 0 : qBound(qint64(1048576), rate, qint64(10 * 1048576));
}
}

WaveQueue::WaveQueue(QString cache, QObject *parent) : QObject(parent), localCache_(std::move(cache)) {
    localPool_.setMaxThreadCount(1);
    failurePool_.setMaxThreadCount(1);
    workerProgram_ = QCoreApplication::applicationFilePath();
    failureFlush_.setSingleShot(true);
    connect(&failureFlush_, &QTimer::timeout, this, &WaveQueue::flushOutcomes);
}
WaveQueue::~WaveQueue() {
    ++sourceRepairGeneration_;
    stopParallel(true);
    for (auto &slot : parallel_) {
        delete slot.worker;
        slot.worker = nullptr;
    }
    failureFlush_.stop();
    flushOutcomes();
    ++*localEpoch_;
    localPool_.waitForDone();
    failurePool_.waitForDone();
    // Finished signals are not delivered while the destructor waits. Drain the latest
    // coalesced chunk/checkpoint after the one already running has committed.
    failureWriting_ = false;
    flushOutcomes();
    failurePool_.waitForDone();
    if (process_) {
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(1000);
    }
}
void WaveQueue::readLocal() {
    QList<Track> wanted = visible_;
    if (selected_.id)
        wanted.prepend(selected_);
    QList<Track> missing;
    for (const auto &t : wanted)
        if (!entries_.contains(key(t)) && !localChecked_.contains(key(t)))
            missing << t;
    if (missing.isEmpty())
        return;
    const auto generation = ++*localEpoch_;
    using Results = QList<QPair<Track, RowWave>>;
    auto *watcher = new QFutureWatcher<Results>(this);
    connect(watcher, &QFutureWatcher<Results>::finished, this, [this, watcher, generation] {
        const auto results = watcher->result();
        watcher->deleteLater();
        if (generation != localEpoch_->load())
            return;
        for (const auto &item : results) {
            const auto k = key(item.first);
            localChecked_.insert(k);
            if (item.second.complete && !entries_.contains(k)) {
                entries_[k] = item.second;
                entries_[k].touched = ++clock_;
                emit changed(item.first);
            }
        }
        trim();
        schedule();
    });
    watcher->setFuture(QtConcurrent::run(&localPool_, [cache = localCache_, missing, epoch = localEpoch_,
                                                       generation, verifyBefore = verifyViewsBefore_] {
        Results results;
        for (const auto &t : missing) {
            if (epoch->load() != generation)
                break;
            RowWave entry;
            qint64 verified = 0;
            QJsonObject diagnostics;
            if (loadWaveView(cache, key(t), &entry.data, &verified, &entry.approximate, &diagnostics)) {
                entry.complete = true;
                entry.stored = true;
                entry.verificationDue = verified <= verifyBefore ||
                                        QDateTime::currentMSecsSinceEpoch() - verified > 24LL * 3600000;
                entry.message = entry.approximate ? "本机参考波形 · 解码或完整性校验存在异常"
                                                  : "本机直读波形 · 原件状态以播放/刷新为准";
                if (!diagnostics.isEmpty()) {
                    entry.message = diagnostics["message"].toString(entry.message);
                    entry.estimatedDuration = diagnostics["declaredMs"].toInteger();
                    entry.failureKind = diagnostics["stopKind"].toString("decode_warning");
                }
            }
            results.append({t, std::move(entry)});
        }
        return results;
    }));
}
QString WaveQueue::key(const Track &t) {
    return waveKey(t.libraryId, t.relativePath, t.size, t.modified);
}
const RowWave *WaveQueue::find(const Track &t) const {
    const auto i = entries_.constFind(key(t));
    return i == entries_.cend() ? nullptr : &i.value();
}
void WaveQueue::setLibraries(const QList<Library> &libraries) {
    stopParallel(true);
    libraries_ = libraries;
}
void WaveQueue::setWorkContext(bool playing, bool scanning, bool editingBusy) {
    if (playing == contextPlaying_ && scanning == contextScanning_ && editingBusy == contextBusy_)
        return;
    contextPlaying_ = playing;
    contextScanning_ = scanning;
    contextBusy_ = editingBusy;
    deferReason_ = playing       ? "试听优先"
                   : scanning    ? "目录收录优先"
                   : editingBusy ? "剪辑任务优先"
                                 : QString();
    if (!deferReason_.isEmpty()) {
        stopParallel();
        if (active_.id && background_)
            cancel();
    }
    emit preparationChanged();
    schedule();
}
bool WaveQueue::hasBackgroundWork() const {
    if (batchRunning_ || (active_.id && background_))
        return true;
    for (const auto &track : visible_) {
        const auto it = entries_.constFind(key(track));
        if (it == entries_.cend() || (!it->failed && (!it->complete || it->verificationDue)))
            return true;
    }
    return false;
}
void WaveQueue::refreshIndex(bool membershipChanged) {
    // A committed catalog update changes scheduling, not the validity of every cached view.
    // Versioned keys naturally separate changed files; unchanged views stay immediately usable.
    batchRunning_ = false;
    sourceRepairPaused_ = sourceRepairResumeRunning_ = false;
    resumeAllAfterRetry_ = allRetryChecked_ = allRetryCheckPending_ = false;
    allPrefixReconcilePending_ = false;
    stopParallel(true);
    cancel();
    ++batchEpoch_;
    batchPage_.clear();
    retryPage_.clear();
    // Catalog membership changed. A paused retry's old range/count is no
    // longer authoritative; rebuild it from active records on the next start.
    retryUpper_ = 0;
    batchComplete_ = false;
    if (membershipChanged)
        catalogNeedsReview_ = true;
    ++*localEpoch_;
    localChecked_.clear();
    entries_.clear();
    visible_.clear();
    selected_ = {};
    // Refresh still verifies displayed originals, including same-size/same-time replacements,
    // but only after their existing local waveforms have been displayed.
    verifyViewsBefore_ = QDateTime::currentMSecsSinceEpoch();
    savePreparation();
    // Excluded/removed tracks must also leave retry totals immediately. This
    // reads only the local catalog; retained history never reopens their source.
    if (!failureFile_.isEmpty())
        initializeFailures();
}
void WaveQueue::invalidate() {
    sourceRepairPaused_ = sourceRepairResumeRunning_ = false;
    resumeAllAfterRetry_ = allRetryChecked_ = allRetryCheckPending_ = false;
    allPrefixReconcilePending_ = false;
    stopParallel(true);
    cancel();
    ++batchEpoch_;
    batchCursor_ = batchDone_ = batchFailed_ = batchTotal_ = 0;
    batchReference_ = 0;
    recentFailures_ = {};
    batchComplete_ = false;
    catalogNeedsReview_ = false;
    batchPage_.clear();
    savePreparation();
    ++*localEpoch_;
    localChecked_.clear();
    localPool_.start([cache = localCache_] { invalidateWaveViews(cache); });
    entries_.clear();
    visible_.clear();
    // Keep the user's selected track prioritized; the next worker rechecks the real source fingerprint.
}
void WaveQueue::cancel() {
    ++sourceRepairGeneration_;
    const bool repairing = repairingSource_;
    repairingSource_ = false;
    if (process_ && active_.id && !cancelled_) {
        cancelled_ = true;
        if (auto i = entries_.find(key(active_)); i != entries_.end() && preserveActiveView_) {
            i->stored = true;
            i->verificationDue = false;
            i->message = "已保留原有波形 · 本次分析已暂停";
        } else if (i != entries_.end() && !i->complete && !i->failed)
            entries_.erase(i);
        else if (i != entries_.end() && i->complete && !i->stored)
            i->verificationDue = true;
        process_->kill(); // next job waits for finished; never destroy a running process here
    }
    if (repairing && active_.id) {
        // The decoder is already idle while the parent repairs the source. There may
        // be no process exit left to release this identity, so release it explicitly.
        cancelled_ = true;
        finishJob(false);
        if (!process_)
            cancelled_ = false;
    }
}
void WaveQueue::setVisible(const QList<Track> &tracks) {
    visible_ = tracks;
    QSet<QString> wanted;
    for (const auto &t : visible_) {
        const auto k = key(t);
        wanted.insert(k);
        if (entries_.contains(k))
            entries_[k].touched = ++clock_;
    }
    if (active_.id && !batchActive_ && !retryActive_ && key(active_) != key(selected_) &&
        !wanted.contains(key(active_)))
        cancel();
    trim();
    readLocal();
    schedule();
}
void WaveQueue::select(const Track &track, bool retry) {
    suspendedSourceRepairKey_.clear();
    stopParallel(); // Audition gets the available I/O immediately.
    selected_ = track;
    const auto k = key(track);
    if (retry && entries_.contains(k) && entries_[k].failed)
        entries_.remove(k);
    if (entries_.contains(k))
        entries_[k].touched = ++clock_;
    if (active_.id && key(active_) != k && (!entries_.contains(k) || !background_))
        cancel();
    else if (active_.id && key(active_) == k && background_ && !cancelled_ && !entries_[k].complete)
        cancel(); // Restart at foreground speed instead of leaving a clicked track rate-limited.
    readLocal();
    schedule();
}
void WaveQueue::trim() {
    QSet<QString> pinned{key(selected_), key(active_)};
    for (const auto &slot : parallel_)
        if (slot.track.id)
            pinned.insert(key(slot.track));
    for (const auto &t : visible_)
        pinned.insert(key(t));
    auto bytes = [this] {
        qint64 total = 0;
        for (const auto &entry : entries_)
            total += entry.data.buckets.capacity() *
                         (sizeof(QVector<Peak>) + entry.data.channels * sizeof(Peak) + 32) +
                     512;
        return total;
    };
    while (entries_.size() > 512 || bytes() > 64 * 1024 * 1024) {
        auto oldest = entries_.end();
        for (auto i = entries_.begin(); i != entries_.end(); ++i)
            if (!pinned.contains(i.key()) && (oldest == entries_.end() || i->touched < oldest->touched))
                oldest = i;
        if (oldest == entries_.end())
            break;
        localChecked_.remove(oldest.key());
        entries_.erase(oldest);
    }
    if (localChecked_.size() > 1024) {
        localChecked_.intersect(pinned);
        for (auto i = entries_.cbegin(); i != entries_.cend(); ++i)
            localChecked_.insert(i.key());
    }
}
void WaveQueue::configurePreparation(const QString &database, const QString &stateFile) {
    preparationDatabase_ = database;
    preparationFile_ = stateFile;
    verificationCache_ = QFileInfo(stateFile).dir().filePath("media-verification.sqlite");
    QFile file(stateFile);
    if (file.open(QIODevice::ReadOnly) && file.size() < 2 * 1024 * 1024) {
        const auto state = QJsonDocument::fromJson(file.readAll()).object();
        if (state["schema"].toInt() != 2) {
            emit preparationChanged();
            return;
        }
        batchCursor_ = qMax(qint64(0), state["cursor"].toInteger());
        batchDone_ = qMax(qint64(0), state["done"].toInteger());
        batchFailed_ = qMax(qint64(0), state["failed"].toInteger());
        batchReference_ = qMax(qint64(0), state["reference"].toInteger());
        batchTotal_ = qMax(qint64(0), state["total"].toInteger());
        batchComplete_ = state["complete"].toBool();
        catalogNeedsReview_ = state["catalogNeedsReview"].toBool();
        verifyViewsBefore_ =
            qBound(qint64(0), state["verifyViewsBefore"].toInteger(), QDateTime::currentMSecsSinceEpoch());
        preparationError_ = state["error"].toString();
        retryReferences_ = state["mode"].toString() == "reference";
        retryMode_ = retryReferences_ || state["mode"].toString() == "failed";
        resumeAllAfterRetry_ = state["resumeAllAfterRetry"].toBool() && retryMode_ && !retryReferences_;
        allRetryChecked_ = state["allRetryChecked"].toBool();
        allPrefixReconcilePending_ = state["reconcilingCachedPrefix"].toBool() && !retryMode_;
        retryCursor_ = qMax(qint64(0), state["retryCursor"].toInteger());
        retryUpper_ = qMax(qint64(0), state["retryUpper"].toInteger());
        retryDone_ = qMax(qint64(0), state["retryDone"].toInteger());
        retryFailed_ = qMax(qint64(0), state["retryFailed"].toInteger());
        retryTotal_ = qMax(qint64(0), state["retryTotal"].toInteger());
        retryComplete_ = state["retryComplete"].toBool();
        retryConverted_ = qMax(qint64(0), state["retryConverted"].toInteger());
        retryRemaining_ = qMax(qint64(0), state["retryRemaining"].toInteger());
        batchRate_ = preparationRate(state["rate"].toInteger(1048576));
        if (catalogNeedsReview_)
            batchComplete_ = false;
        recentFailures_ = state["failures"].toArray();
        while (recentFailures_.size() > 100)
            recentFailures_.removeFirst();
    }
    failureFile_ = QFileInfo(stateFile).dir().filePath("waveform-failures.sqlite");
    initializeFailures();
    emit preparationChanged(); // Reopening always stays paused until the user resumes.
}
QJsonObject WaveQueue::preparation() const {
    QJsonObject result{{"schema", 2},
            {"concurrency", preparationConcurrency_},
            {"activeJobs", parallelActiveCount() + (active_.id && !cancelled_ ? 1 : 0)},
            {"reference", batchReference_},
            {"running", batchRunning_},
            {"deferred", !deferReason_.isEmpty() && hasBackgroundWork()},
            {"deferReason", deferReason_},
            {"backgroundPending", hasBackgroundWork()},
            {"waiting", waiting_},
            {"repairingSource", repairingSource_},
            {"sourceRepairPaused", sourceRepairPaused_},
            {"publishing", publishing_},
            {"mode", retryMode_ ? (retryReferences_ ? "reference" : "failed") : "all"},
            {"resumeAllAfterRetry", resumeAllAfterRetry_},
            {"allRetryChecked", allRetryChecked_},
            {"reconcilingCachedPrefix", allPrefixReconciling_ || allPrefixReconcilePending_},
            {"retryCursor", retryCursor_},
            {"retryUpper", retryUpper_},
            {"retryDone", retryDone_},
            {"retryFailed", retryFailed_},
            {"retryTotal", retryTotal_},
            {"retryComplete", retryComplete_},
            {"retryConverted", retryConverted_},
            {"retryRemaining", retryRemaining_},
            {"failuresReady", failuresReady_},
            {"knownFailures", failureSummary_["knownFailures"]},
            {"knownReferences", failureSummary_["knownReferences"]},
            {"legacyUntracked", failureSummary_["legacyUntracked"]},
            {"inactiveRecords", failureSummary_["inactiveRecords"]},
            {"inactiveReferences", failureSummary_["inactiveReferences"]},
            {"failureCategories", failureSummary_["categories"]},
            {"referenceCategories", failureSummary_["referenceCategories"]},
            {"complete", batchComplete_},
            {"catalogNeedsReview", catalogNeedsReview_},
            {"verifyViewsBefore", verifyViewsBefore_},
            {"cursor", batchCursor_},
            {"done", batchDone_},
            {"failed", batchFailed_},
            {"total", batchTotal_},
            {"current", active_.name},
            {"currentPath", active_.relativePath},
            {"readBytes", readBytes_},
            {"decodedMs", decodedMs_},
            {"estimatedMs", entries_.value(key(active_)).estimatedDuration},
            {"elapsedMs", active_.id ? jobClock_.elapsed() : 0},
            {"idleMs", active_.id ? activityClock_.elapsed() : 0},
            {"canSkip", batchRunning_ && (batchActive_ || retryActive_) && active_.id && !cancelled_},
            {"foreground", active_.id && !background_},
            {"failures", recentFailures_},
            {"error", preparationError_},
            {"rate", batchRate_}};
    if (!active_.id) {
        for (const auto &slot : parallel_) {
            if (!slot.track.id || slot.done || !slot.worker)
                continue;
            const auto progress = slot.worker->preparation();
            for (const auto *field : {"current", "currentPath", "readBytes", "decodedMs", "estimatedMs",
                                      "elapsedMs", "idleMs", "waiting", "publishing", "repairingSource"})
                result[field] = progress[field];
            result["canSkip"] = batchRunning_;
            result["foreground"] = false;
            break;
        }
    }
    return result;
}
void WaveQueue::savePreparation() {
    // Cached rows are consumed in small chunks; keep only the latest requested snapshot.
    checkpointDirty_ = true;
    if (!failureFlush_.isActive())
        failureFlush_.start(0);
    emit preparationChanged();
}
void WaveQueue::prepareAll(qint64 rate, bool restart) {
    if (preparationDatabase_.isEmpty())
        return;
    suspendedSourceRepairKey_.clear();
    sourceRepairPaused_ = sourceRepairResumeRunning_ = false;
    if (resumeAllAfterRetry_ && retryMode_ && !restart) {
        batchRate_ = preparationRate(rate);
        batchRunning_ = true;
        preparationError_.clear();
        savePreparation();
        schedule();
        return;
    }
    if (retryMode_) {
        stopParallel(true);
        cancel();
        retryMode_ = false;
        retryPage_.clear();
        ++batchEpoch_;
        resumeAllAfterRetry_ = allRetryChecked_ = false;
    }
    if (batchComplete_ || restart || catalogNeedsReview_) {
        stopParallel(true);
        if (batchActive_ || repairingSource_)
            cancel();
        ++batchEpoch_;
        batchCursor_ = batchDone_ = batchFailed_ = batchTotal_ = 0;
        batchReference_ = 0;
        recentFailures_ = {};
        batchPage_.clear();
        batchComplete_ = false;
        catalogNeedsReview_ = false;
        allRetryChecked_ = false;
        allPrefixReconcilePending_ = false;
    }
    const auto newRate = preparationRate(rate);
    if (newRate != batchRate_)
        stopParallel();
    batchRate_ = newRate;
    batchRunning_ = true;
    if (backgroundJobRate() != activeRate_ && active_.id && background_)
        cancel();
    preparationError_.clear();
    allRetryCheckPending_ = bool(sourceRepairHandler_) && !allRetryChecked_;
    savePreparation();
    if (allPrefixReconcilePending_) {
        continueAllAfterRetry();
        return;
    }
    if (maybeAutoRetryAll())
        return;
    if (batchPage_.isEmpty())
        fetchBatch(); // Publish the total even while visible tracks have decoding priority.
    schedule();
}
void WaveQueue::pausePreparation() {
    if (repairingSource_)
        suspendedSourceRepairKey_ = key(active_);
    sourceRepairPaused_ = sourceRepairResumeRunning_ = false;
    batchRunning_ = false;
    stopParallel();
    if (repairingSource_ || batchActive_ || retryActive_ ||
        (active_.id && background_ && (activeRate_ == 0 || activeRate_ > 1048576)))
        cancel();
    savePreparation();
}
void WaveQueue::initializeFailures() {
    failuresReady_ = false;
    auto *watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher] {
        auto result = watcher->result();
        watcher->deleteLater();
        if (result.contains("error")) {
            preparationError_ = "异常记录未能打开：" + result["error"].toString();
            batchRunning_ = false;
        } else {
            failureSummary_ = result;
            failuresReady_ = true;
            if (retryMode_ && batchRunning_)
                fetchRetries(retryUpper_ == 0);
            else if (allRetryCheckPending_)
                maybeAutoRetryAll();
        }
        emit preparationChanged();
        schedule();
    });
    watcher->setFuture(QtConcurrent::run(&failurePool_, [path = failureFile_, database = preparationDatabase_,
                                                         recent = recentFailures_, failed = batchFailed_] {
        FailureStore store(path, database);
        QString error;
        if (!store.open(&error) || !store.importLegacy(recent, failed, &error))
            return QJsonObject{{"error", error}};
        auto result = store.summary(&error);
        if (!error.isEmpty())
            result["error"] = error;
        return result;
    }));
}
void WaveQueue::persistOutcome(const Track &track, bool success, bool reference, qint64 retryId) {
    if (failureFile_.isEmpty())
        return;
    const auto entry = entries_.value(key(track));
    const auto message = reference && entry.message.isEmpty()
                             ? QString("解码或完整性校验存在异常，已保留本机参考波形，未作为完整波形共享。")
                             : entry.message;
    pendingOutcomes_.append({track, success, reference, entry.failureKind, message, retryId});
    if (!failureFlush_.isActive())
        failureFlush_.start(0);
}
void WaveQueue::flushOutcomes() {
    if (failureWriting_ || failureFile_.isEmpty() ||
        (pendingOutcomes_.isEmpty() && !failureWrites_->retryNeeded.load() && !checkpointDirty_))
        return;
    auto outcomes = std::move(pendingOutcomes_);
    pendingOutcomes_.clear();
    const bool saveCheckpoint = checkpointDirty_;
    const auto bytes =
        saveCheckpoint ? QJsonDocument(preparation()).toJson(QJsonDocument::Compact) : QByteArray();
    checkpointDirty_ = false;
    failureWriting_ = true;
    auto *watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this, [this, watcher] {
        auto result = watcher->result();
        watcher->deleteLater();
        failureWriting_ = false;
        if (result.contains("error")) {
            checkpointDirty_ = true;
            preparationError_ = result["checkpointError"].toBool()
                                    ? "续跑进度未能保存，请检查本机缓存盘的空间和写入权限。"
                                    : "异常记录未能保存，已暂停：" + result["error"].toString();
            batchRunning_ = false;
            if (batchActive_ || retryActive_)
                cancel();
        } else {
            if (result.contains("knownFailures"))
                failureSummary_ = result;
            flushOutcomes();
            schedule();
        }
        emit preparationChanged();
    });
    watcher->setFuture(QtConcurrent::run(
        &failurePool_, [path = failureFile_, database = preparationDatabase_, outcomes,
                        writes = failureWrites_, statePath = preparationFile_, bytes, saveCheckpoint] {
            writes->pending.append(outcomes);
            QString error;
            QJsonObject result;
            if (!writes->pending.isEmpty()) {
                FailureStore store(path, database);
                if (!store.open(&error) || !store.apply(writes->pending, &error)) {
                    writes->retryNeeded.store(true);
                    return QJsonObject{{"error", error}};
                }
                writes->pending.clear();
                writes->retryNeeded.store(false);
                result = store.summary(&error);
                if (!error.isEmpty())
                    return QJsonObject{{"error", error}};
            }
            // The chunk must commit before its cursor can become durable. A failed chunk
            // remains in the serial backlog and leaves the previous disk cursor untouched.
            if (saveCheckpoint && !statePath.isEmpty()) {
                QSaveFile file(statePath);
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
                    return QJsonObject{{"error", file.errorString()}, {"checkpointError", true}};
            }
            return result;
        }));
}
quint64 WaveQueue::requestFailurePage(int offset, int limit, const QString &category, bool references) {
    const auto request = ++failureQueryRevision_;
    if (failureFile_.isEmpty()) {
        QTimer::singleShot(0, this, [this, request] {
            emit failurePageReady(QJsonObject{{"error", "异常记录尚未配置"}, {"requestId", qint64(request)}});
        });
        return request;
    }
    flushOutcomes();
    auto *watcher = new QFutureWatcher<QJsonObject>(this);
    connect(watcher, &QFutureWatcher<QJsonObject>::finished, this,
            [this, watcher, request, offset, category, references] {
                auto page = watcher->result();
                page["requestId"] = qint64(request);
                page["offset"] = offset;
                page["category"] = category;
                page["references"] = references;
                if (!page.contains("error") && page.contains("knownFailures")) {
                    failureSummary_ = page;
                    emit preparationChanged();
                }
                emit failurePageReady(page);
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run(&failurePool_, [path = failureFile_, database = preparationDatabase_,
                                                         offset, limit, category, references] {
        FailureStore store(path, database);
        QString error;
        if (!store.open(&error))
            return QJsonObject{{"error", error}};
        auto page = store.page(offset, limit, category, references, &error);
        if (!error.isEmpty())
            page["error"] = error;
        return page;
    }));
    return request;
}
void WaveQueue::retryFailed(qint64 rate) {
    startRetry(false, rate);
}
void WaveQueue::retryReferences(qint64 rate) {
    startRetry(true, rate);
}
bool WaveQueue::maybeAutoRetryAll() {
    if (!allRetryCheckPending_ || !batchRunning_ || retryMode_)
        return false;
    if (!failuresReady_)
        return true;
    allRetryCheckPending_ = false;
    allRetryChecked_ = true;
    if (failureSummary_["knownFailures"].toInteger() <= 0)
        return false;
    // A bounded ledger snapshot is retried once before returning to the unchanged
    // all-mode cursor. In particular, old failures before that cursor remain reachable.
    startRetry(false, batchRate_);
    resumeAllAfterRetry_ = true;
    savePreparation();
    return true;
}
void WaveQueue::continueAllAfterRetry() {
    allPrefixReconcilePending_ = false;
    resumeAllAfterRetry_ = false;
    retryMode_ = false;
    retryPage_.clear();
    batchRunning_ = true;
    allPrefixReconciling_ = true;
    const auto generation = batchEpoch_;
    const auto cursor = batchCursor_;
    struct Prefix {
        qint64 done = 0, references = 0;
        QString error;
    };
    auto *watcher = new QFutureWatcher<Prefix>(this);
    connect(watcher, &QFutureWatcher<Prefix>::finished, this, [this, watcher, generation, cursor] {
        const auto prefix = watcher->result();
        watcher->deleteLater();
        allPrefixReconciling_ = false;
        if (generation == batchEpoch_ && cursor == batchCursor_ && !retryMode_) {
            if (prefix.error.isEmpty()) {
                batchDone_ = prefix.done;
                batchReference_ = prefix.references;
            } else {
                batchRunning_ = false;
                preparationError_ = "本机缓存进度未能核对：" + prefix.error;
            }
            savePreparation();
        }
        schedule();
    });
    watcher->setFuture(QtConcurrent::run(
        &localPool_, [database = preparationDatabase_, cache = localCache_, cursor] {
            Prefix prefix;
            Index index(database);
            if (!index.open(&prefix.error))
                return prefix;
            qint64 after = 0;
            while (after < cursor) {
                const auto page = index.tracksAfter(after, 200);
                prefix.error = index.lastError();
                if (!prefix.error.isEmpty() || page.isEmpty())
                    break;
                for (const auto &track : page) {
                    if (track.id > cursor)
                        return prefix;
                    after = track.id;
                    WaveData wave;
                    bool reference = false;
                    if (loadWaveView(cache, key(track), &wave, nullptr, &reference)) {
                        ++prefix.done;
                        prefix.references += reference;
                    }
                }
            }
            return prefix;
        }));
}
void WaveQueue::startRetry(bool references, qint64 rate) {
    suspendedSourceRepairKey_.clear();
    resumeAllAfterRetry_ = allRetryCheckPending_ = false;
    allPrefixReconcilePending_ = false;
    sourceRepairPaused_ = sourceRepairResumeRunning_ = false;
    stopParallel(true);
    const bool resume = retryMode_ && retryReferences_ == references && !retryComplete_ && retryUpper_ > 0;
    if (repairingSource_ || batchActive_ || retryActive_)
        cancel();
    ++batchEpoch_;
    batchPage_.clear();
    retryPage_.clear();
    retryMode_ = true;
    retryFallbackKey_.clear();
    retryReferences_ = references;
    retryComplete_ = false;
    batchRunning_ = true;
    preparationError_.clear();
    if (rate >= 0)
        batchRate_ = preparationRate(rate);
    if (active_.id && background_ && activeRate_ != backgroundJobRate())
        cancel();
    if (!resume) {
        retryCursor_ = retryUpper_ = retryDone_ = retryFailed_ = retryTotal_ = 0;
        retryConverted_ = retryRemaining_ = 0;
    }
    flushOutcomes();
    savePreparation();
    if (failuresReady_)
        fetchRetries(!resume);
}
void WaveQueue::fetchRetries(bool start) {
    if (!batchRunning_ || !retryMode_ || retryLoading_ || !failuresReady_ || failureWriting_ ||
        !pendingOutcomes_.isEmpty())
        return;
    retryLoading_ = true;
    struct Page {
        QList<FailureRecord> records;
        QJsonObject summary;
        qint64 upper = 0;
        QString error;
    };
    const auto generation = batchEpoch_;
    auto *watcher = new QFutureWatcher<Page>(this);
    connect(watcher, &QFutureWatcher<Page>::finished, this, [this, watcher, generation, start] {
        auto page = watcher->result();
        watcher->deleteLater();
        retryLoading_ = false;
        if (generation != batchEpoch_) {
            schedule();
            return;
        }
        if (!page.error.isEmpty()) {
            preparationError_ = "异常记录未能读取：" + page.error;
            batchRunning_ = false;
        } else {
            if (start) {
                retryUpper_ = page.upper;
                retryTotal_ =
                    page.summary[retryReferences_ ? "knownReferences" : "knownFailures"].toInteger();
            }
            failureSummary_ = page.summary;
            retryPage_ = page.records;
            if (retryPage_.isEmpty()) {
                batchRunning_ = false;
                retryComplete_ = true;
                if (resumeAllAfterRetry_)
                    continueAllAfterRetry();
            }
        }
        savePreparation();
        schedule();
    });
    watcher->setFuture(QtConcurrent::run(&failurePool_, [path = failureFile_, database = preparationDatabase_,
                                                         cursor = retryCursor_, upper = retryUpper_, start,
                                                         references = retryReferences_] {
        Page page;
        FailureStore store(path, database);
        if (!store.open(&page.error))
            return page;
        page.summary = store.summary(&page.error);
        page.upper = start ? store.retryUpper(&page.error, references) : upper;
        if (page.error.isEmpty())
            page.records = store.retryPage(cursor, page.upper, 100, &page.error, references);
        return page;
    }));
}
void WaveQueue::advanceRetry(const Track &track, bool success, bool reference) {
    if (retryPage_.isEmpty() || retryPage_.first().track.id != track.id)
        return;
    const auto record = retryPage_.takeFirst();
    const auto id = record.id;
    retryCursor_ = id;
    ++retryDone_;
    if (retryReferences_) {
        const auto entry = entries_.value(key(track));
        if (entry.complete && entry.stored && !reference) {
            ++retryConverted_;
            if (record.reference)
                batchReference_ = qMax(qint64(0), batchReference_ - 1);
        } else if (reference)
            ++retryRemaining_;
    }
    if (!success) {
        ++retryFailed_;
        const auto entry = entries_.value(key(track));
        // A failed original is recorded and the remaining retry range still runs.
        // Exhausted local cache storage is a batch-wide condition that must stop writes.
        if (entry.failureKind == "storage_full") {
            batchRunning_ = false;
            preparationError_ = entry.message;
        }
    }
    persistOutcome(track, success, reference, id);
    savePreparation();
}
void WaveQueue::loadReferenceFallback(const Track &track) {
    if (retryLocalLoading_)
        return;
    retryLocalLoading_ = true;
    const auto epoch = batchEpoch_;
    auto *watcher = new QFutureWatcher<RowWave>(this);
    connect(watcher, &QFutureWatcher<RowWave>::finished, this, [this, watcher, track, epoch] {
        const auto entry = watcher->result();
        watcher->deleteLater();
        retryLocalLoading_ = false;
        if (epoch == batchEpoch_) {
            const auto k = key(track);
            retryFallbackKey_ = k;
            localChecked_.insert(k);
            if (entry.complete && (!entries_.contains(k) || !entries_[k].complete)) {
                entries_[k] = entry;
                entries_[k].touched = ++clock_;
                emit changed(track);
            }
        }
        schedule();
    });
    watcher->setFuture(QtConcurrent::run(&localPool_, [cache = localCache_, track] {
        RowWave entry;
        if (loadWaveView(cache, key(track), &entry.data, nullptr, &entry.approximate)) {
            entry.complete = entry.stored = true;
            entry.message = entry.approximate ? "保留原参考波形 · 等待重新分析" : "保留已有完整波形";
        }
        return entry;
    }));
}
void WaveQueue::skipCurrent() {
    if (batchRunning_ && !active_.id) {
        for (auto &slot : parallel_) {
            if (!slot.track.id || slot.done)
                continue;
            auto &entry = entries_[key(slot.track)];
            entry.failed = true;
            entry.failureKind = "manual_skip";
            entry.message = "已手动跳过；全库结束后可重试";
            slot.worker->selected_ = {};
            slot.worker->cancel();
            slot.done = true;
            slot.success = false;
            emit changed(slot.track);
            QTimer::singleShot(0, this, &WaveQueue::schedule);
            return;
        }
    }
    if (!batchRunning_ || (!batchActive_ && !retryActive_) || !active_.id || cancelled_)
        return;
    auto &entry = entries_[key(active_)];
    entry.failed = true;
    entry.failureKind = "manual_skip";
    entry.message = "已手动跳过；全库结束后可重试";
    emit changed(active_);
    if (retryActive_)
        advanceRetry(active_, false, entry.approximate);
    else
        advanceBatch(active_, false);
    cancel(); // Its exit must not count the same file twice or cancel the remaining library.
}
void WaveQueue::fetchBatch() {
    if (batchLoading_ || !batchRunning_ || retryMode_)
        return;
    if (failureWriting_ || !pendingOutcomes_.isEmpty()) {
        flushOutcomes();
        return;
    }
    batchLoading_ = true;
    flushOutcomes();
    struct Page {
        QList<QPair<Track, int>> tracks;
        qint64 total = 0;
        QString error;
    };
    const auto generation = batchEpoch_;
    auto *watcher = new QFutureWatcher<Page>(this);
    connect(watcher, &QFutureWatcher<Page>::finished, this, [this, watcher, generation] {
        const auto page = watcher->result();
        watcher->deleteLater();
        batchLoading_ = false;
        if (generation != batchEpoch_) {
            schedule();
            return;
        }
        batchTotal_ = page.total;
        batchPage_ = page.tracks;
        if (!page.error.isEmpty()) {
            preparationError_ = page.error;
            batchRunning_ = false;
        } else if (batchPage_.isEmpty()) {
            batchRunning_ = false;
            batchComplete_ = true;
            if (!failureFile_.isEmpty()) {
                flushOutcomes();
                failurePool_.start(
                    [this, path = failureFile_, database = preparationDatabase_, writes = failureWrites_] {
                        if (!writes->pending.isEmpty())
                            return;
                        FailureStore store(path, database);
                        QString error;
                        if (store.open(&error) && store.clearLegacyUnknown(&error)) {
                            const auto summary = store.summary(&error);
                            if (error.isEmpty())
                                QMetaObject::invokeMethod(
                                    this,
                                    [this, summary] {
                                        failureSummary_ = summary;
                                        emit preparationChanged();
                                    },
                                    Qt::QueuedConnection);
                        }
                    });
            }
        }
        savePreparation();
        schedule();
    });
    watcher->setFuture(QtConcurrent::run(
        &localPool_, [database = preparationDatabase_, cache = localCache_, cursor = batchCursor_] {
            Page page;
            Index db(database);
            if (!db.open(&page.error))
                return page;
            page.total = db.count();
            const auto tracks = db.tracksAfter(cursor);
            page.error = db.lastError();
            for (const auto &track : tracks) {
                WaveData wave;
                bool reference = false;
                // Catalog reconciliation trusts the versioned local copy. Visible rows keep
                // their separate age/refresh verification policy; bulk checking must not read
                // every NAS original merely because cached views are more than a day old.
                const bool cached = loadWaveView(cache, key(track), &wave, nullptr, &reference);
                page.tracks.append({track, cached ? (reference ? 2 : 1) : 0});
            }
            return page;
        }));
}
void WaveQueue::advanceBatch(const Track &track, bool success, bool reference) {
    if (batchPage_.isEmpty() || batchPage_.first().first.id != track.id)
        return;
    batchPage_.removeFirst();
    batchCursor_ = track.id;
    persistOutcome(track, success, reference);
    if (success) {
        ++batchDone_;
        if (reference)
            ++batchReference_;
    } else {
        ++batchFailed_;
        const auto entry = entries_.value(key(track));
        const auto kind = entry.failureKind;
        recentFailures_.append(QJsonObject{{"id", track.id},
                                           {"name", track.name},
                                           {"path", track.relativePath},
                                           {"library", track.libraryId},
                                           {"kind", kind},
                                           {"message", entry.message},
                                           {"time", QDateTime::currentDateTime().toString(Qt::ISODate)}});
        while (recentFailures_.size() > 100)
            recentFailures_.removeFirst();
        // Source access/read failures and other per-file failures must not prevent
        // later originals from being prepared, even when many fail consecutively.
        if (kind == "storage_full") {
            preparationError_ = entry.message;
            batchRunning_ = false;
        }
    }
    savePreparation();
}
void WaveQueue::setSourceRepairHandler(SourceRepairHandler handler) {
    sourceRepairHandler_ = std::move(handler);
    for (auto &slot : parallel_)
        if (slot.worker)
            slot.worker->setSourceRepairHandler(sourceRepairHandler_);
}
void WaveQueue::refreshMappedTrack(const Track &mapped) {
    const auto mappedKey = key(mapped);
    auto update = [&](Track &track) {
        if (track.id == mapped.id && key(track) == mappedKey)
            track = mapped;
    };
    update(selected_);
    update(active_);
    for (auto &track : visible_)
        update(track);
    for (auto &item : batchPage_)
        update(item.first);
    for (auto &record : retryPage_)
        update(record.track);
    for (auto &slot : parallel_) {
        update(slot.track);
        if (slot.worker)
            slot.worker->refreshMappedTrack(mapped);
    }
}
void WaveQueue::pauseForSourceRepair(const Track &track, const SourceRepairReply &reply) {
    if (sourceRepairPaused_)
        return;
    sourceRepairResumeRunning_ = batchRunning_;
    sourceRepairPaused_ = true;
    batchRunning_ = false;
    preparationError_ = reply.error;
    stopParallel();
    if (active_.id)
        cancel();
    savePreparation();
    if (reply.needsConnection)
        emit sourceRepairNeedsConnection(track, reply.error);
}
void WaveQueue::resumeAfterSourceRepair() {
    if (!sourceRepairPaused_)
        return;
    suspendedSourceRepairKey_.clear();
    sourceRepairPaused_ = false;
    batchRunning_ = sourceRepairResumeRunning_;
    sourceRepairResumeRunning_ = false;
    preparationError_.clear();
    savePreparation();
    schedule();
}
bool WaveQueue::trySourceRepair() {
    const auto track = active_;
    const auto k = key(track);
    const auto kind = entries_.value(k).failureKind;
    if (!sourceRepairHandler_ || sourceRepairAttempted_ ||
        (!batchRunning_ && !parallelWorker_) ||
        (kind != "source_unavailable" && kind != "compatible_unavailable"))
        return false;
    sourceRepairAttempted_ = true;
    repairingSource_ = true;
    workerWatchdog_.stop();
    waiting_ = false;
    auto &entry = entries_[k];
    entry.failed = false;
    entry.message = "正在自动修复音频访问路径…";
    const auto generation = sourceRepairGeneration_;
    const QPointer<WaveQueue> guard(this);
    const auto handler = sourceRepairHandler_;
    emit changed(track);
    emit preparationChanged();
    if (!guard || generation != sourceRepairGeneration_ || !repairingSource_)
        return true;
    handler(track, [guard, track, k, kind, generation](SourceRepairReply reply) {
        if (!guard || guard->sourceRepairGeneration_ != generation || !guard->repairingSource_ ||
            guard->cancelled_ || !guard->active_.id || key(guard->active_) != k)
            return;
        auto *queue = guard.data();
        auto *owner = queue->sourceRepairOwner_ ? queue->sourceRepairOwner_.data() : queue;
        if (reply.paused || reply.needsConnection) {
            owner->pauseForSourceRepair(track, reply);
            return;
        }
        queue->repairingSource_ = false;
        auto &current = queue->entries_[k];
        if (reply.mapped.id == track.id && reply.mapped.id > 0 && key(reply.mapped) == k) {
            owner->refreshMappedTrack(reply.mapped);
            current.failed = false;
            current.failureKind.clear();
            current.message = "路径已修复 · 正在重新准备波形…";
            queue->readBytes_ = queue->decodedMs_ = 0;
            emit owner->sourceRepaired(reply.mapped);
            if (guard && guard->sourceRepairGeneration_ == generation && !guard->cancelled_ &&
                key(guard->active_) == k)
                guard->submitActiveJob();
        } else {
            current.failed = true;
            current.failureKind = kind;
            current.message = reply.error.isEmpty() ? QString("自动修复未完成，原件仍不可访问") : reply.error;
            emit queue->changed(track);
            queue->finishJob(false);
        }
    });
    return true;
}
void WaveQueue::finishJob(bool success) {
    if (!active_.id)
        return;
    if (!cancelled_ && (repairingSource_ || (!success && trySourceRepair())))
        return;
    const auto track = active_;
    const auto k = key(track);
    const bool report = !cancelled_;
    bool outcomeSuccess = false;
    if (!cancelled_) {
        auto &entry = entries_[k];
        if (!success && !entry.failed) {
            entry.failed = true;
            entry.failureKind = publishing_ ? "shared_cache_write" : "worker_failed";
            entry.message =
                publishing_ ? "共享同步未确认完成，已保留本机波形，可重试" : "波形任务意外结束，点击可重试";
            emit changed(track);
        }
        if (!entry.complete && !entry.failed) {
            entry.failed = true;
            entry.failureKind = "worker_failed";
            entry.message = "波形未完成，点击可重试";
            emit changed(track);
        }
        if (!success && entry.complete && !entry.stored) {
            entry.failed = true;
            entry.message = "原件核验失败 · 显示上次缓存，播放时检查原件";
            emit changed(track);
        }
        outcomeSuccess = success && entry.complete && entry.stored;
        if (retryActive_)
            advanceRetry(track, outcomeSuccess, entry.approximate);
        else if (batchActive_)
            advanceBatch(track, outcomeSuccess, entry.approximate);
        else
            persistOutcome(track, outcomeSuccess, entry.approximate);
        if (preserveActiveView_)
            entry.stored = true; // The failed/unfinished attempt never replaced the old local view.
    }
    active_ = {};
    readBytes_ = decodedMs_ = 0;
    batchActive_ = false;
    retryActive_ = false;
    waiting_ = false;
    publishing_ = false;
    preserveActiveView_ = false;
    sourceRepairAttempted_ = false;
    workerWatchdog_.stop();
    if (report)
        emit jobFinished(track, outcomeSuccess);
    trim();
    emit preparationChanged();
    QTimer::singleShot(0, this, &WaveQueue::schedule);
}
void WaveQueue::startProcess() {
    auto *p = new QProcess(this);
    process_ = p;
    pending_.clear();
    connect(p, &QProcess::readyReadStandardError, p, [p] { p->readAllStandardError(); });
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p] {
        pending_.append(p->readAllStandardOutput());
        if (cancelled_ || process_ != p) {
            pending_.clear();
            return;
        }
        if (pending_.size() > 4 * 1024 * 1024) {
            p->kill();
            return;
        }
        qsizetype pos;
        while ((pos = pending_.indexOf('\n')) >= 0) {
            const auto m = QJsonDocument::fromJson(pending_.left(pos)).object();
            pending_.remove(0, pos + 1);
            if (!active_.id)
                continue;
            const auto track = active_;
            const auto k = key(track);
            auto &entry = entries_[k];
            activityClock_.restart();
            if (m["event"] == "waiting") {
                if (!waiting_) {
                    const qint64 maximumWait = background_ ? 65000 : 4500;
                    waitDeadline_ =
                        jobClock_.elapsed() + qBound(qint64(0), m["limitMs"].toInteger(), maximumWait) + 2000;
                }
                waiting_ = true;
                entry.message = m["message"].toString("其他电脑正在准备这首波形");
                emit changed(track);
                emit preparationChanged();
                continue;
            }
            waiting_ = false;
            if (m["event"] == "wave") {
                WaveData wave;
                if (!decodeWave(QByteArray::fromBase64(m["data"].toString().toLatin1()), &wave))
                    continue;
                if (preserveActiveView_ && (!m["complete"].toBool() || !m["stored"].toBool())) {
                    decodedMs_ = wave.sampleRate > 0 ? wave.frames * 1000 / wave.sampleRate : 0;
                    entry.failureKind = m["failureKind"].toString();
                    entry.message = m["complete"].toBool() ? "新波形未能保存 · 继续显示原有波形"
                                                           : "保留原有波形 · 正在重新分析";
                    emit preparationChanged();
                    continue;
                }
                preserveActiveView_ = false;
                entry.data = std::move(wave);
                entry.complete = m["complete"].toBool();
                entry.approximate = m["approximate"].toBool();
                entry.stored = m["stored"].toBool();
                entry.failureKind = m["failureKind"].toString();
                entry.message = m["source"].toString();
                if (entry.failureKind == "source_changed")
                    entry.message = "原文件版本已变化，请刷新素材库后重试";
                else if (entry.failureKind == "cache_write")
                    entry.message = "本机波形缓存未能保存，请检查缓存盘空间和写入权限";
                decodedMs_ = entry.data.sampleRate > 0 ? entry.data.frames * 1000 / entry.data.sampleRate : 0;
                emit changed(track);
            } else if (m["event"] == "duration") {
                entry.estimatedDuration = m["milliseconds"].toInteger();
            } else if (m["event"] == "reading") {
                readBytes_ = m["bytes"].toInteger();
                if (entry.data.buckets.isEmpty()) {
                    entry.message =
                        QString("正在读取 %1 MiB · 可先点击试听").arg(m["bytes"].toInteger() / 1048576);
                    emit changed(track);
                }
            } else if (m["event"] == "error") {
                entry.failed = true;
                entry.message = m["message"].toString();
                entry.failureKind = m["kind"].toString();
                emit changed(track);
            } else if (m["event"] == "warning") {
                if (!entry.stored)
                    entry.message = m["message"].toString();
                emit warning(track, m["message"].toString());
            } else if (m["event"] == "job-finished") {
                finishJob(m["success"].toBool());
            }
        }
    });
    auto exited = [this, p] {
        if (process_ != p)
            return;
        // Cached pixels are not a completion receipt for a new verification/publication job.
        // Persistent workers must acknowledge each job before exiting.
        process_ = nullptr;
        finishJob(false);
        cancelled_ = false;
        p->deleteLater();
        emit preparationChanged(); // A parent pool may be waiting to reuse a cancelled slot.
        QTimer::singleShot(0, this, &WaveQueue::schedule);
    };
    connect(p, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, exited);
    connect(p, &QProcess::errorOccurred, this, [exited](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            exited();
    });
    connect(&workerWatchdog_, &QTimer::timeout, p, [this, p] {
        const auto rate = activeRate_ > 0 ? activeRate_ : 10 * 1048576;
        const auto maximum = qMax(qint64(3600000), active_.size / qMax(qint64(1), rate) * 3000 + 60000);
        if (process_ == p && active_.id && !cancelled_ &&
            ((waiting_ ? jobClock_.elapsed() > waitDeadline_ : activityClock_.elapsed() > 15000) ||
             jobClock_.elapsed() > maximum)) {
            auto &entry = entries_[key(active_)];
            entry.failed = true;
            entry.failureKind = publishing_ ? "shared_publish_timeout"
                                : waiting_  ? "shared_timeout"
                                            : "timeout";
            entry.message = publishing_ ? "共享同步超时，已保留本机波形，可重试"
                            : waiting_  ? "等待共享波形超时，点击可重试"
                                        : "服务器读取无进展，已跳过；点击可重试";
            emit changed(active_);
            p->kill();
        }
        // Report time/read progress even when the completed count is unchanged.
        if (process_ == p && active_.id)
            emit preparationChanged();
    });
    p->start(workerProgram_, {"--worker", "waves"});
}
void WaveQueue::schedule() {
    if (sourceRepairPaused_ || allPrefixReconciling_ || maybeAutoRetryAll())
        return;
    if (!parallelWorker_ && (!batchRunning_ || !deferReason_.isEmpty()))
        stopParallel();
    if (!parallelWorker_ && batchRunning_ && !retryMode_ && !failureWriting_)
        drainParallel();
    // Local retry metadata can load while audition/system load defers audio work. A mode
    // change may wait behind a checkpoint write; load its own total when that write finishes.
    if (batchRunning_ && retryMode_ && retryPage_.isEmpty())
        fetchRetries(retryUpper_ == 0);
    const bool allowBackground = deferReason_.isEmpty() && !failureWrites_->retryNeeded.load() &&
                                 pendingOutcomes_.size() < OutcomeChunkSize;
    if (active_.id && (batchActive_ || retryActive_) && !cancelled_) {
        bool needed =
            selected_.id && !entries_.contains(key(selected_)) && localChecked_.contains(key(selected_));
        if (allowBackground)
            for (const auto &t : visible_)
                needed |= !entries_.contains(key(t)) && localChecked_.contains(key(t));
        if (needed)
            cancel();
    }
    if (active_.id || cancelled_) {
        if (active_.id && background_ && !cancelled_)
            scheduleParallel();
        return;
    }
    Track next;
    const auto sourceSuspended = [this](const Track &track) {
        return !batchRunning_ && !suspendedSourceRepairKey_.isEmpty() && key(track) == suspendedSourceRepairKey_;
    };
    bool publishExisting = false;
    auto sharedConfigured = [this](const Track &track) {
        for (const auto &library : libraries_)
            if (library.id == track.libraryId)
                return !library.sharedCache.isEmpty();
        return false;
    };
    if (selected_.id && !sourceSuspended(selected_) && !entries_.contains(key(selected_)) && localChecked_.contains(key(selected_)))
        next = selected_;
    else if (selected_.id && !sourceSuspended(selected_) && entries_.contains(key(selected_)) && entries_[key(selected_)].verificationDue &&
             !entries_[key(selected_)].failed)
        next = selected_;
    else if (allowBackground)
        for (const auto &t : visible_)
            if (!sourceSuspended(t) && !parallelOwns(t) && !entries_.contains(key(t)) && localChecked_.contains(key(t))) {
                next = t;
                break;
            }
    if (!next.id && allowBackground) {
        for (const auto &t : visible_) {
            const auto it = entries_.constFind(key(t));
            if (!sourceSuspended(t) && !parallelOwns(t) && it != entries_.cend() && it->verificationDue && !it->failed) {
                next = t;
                break;
            }
        }
    }
    if (!next.id && allowBackground && !failureWriting_ && batchRunning_ && retryMode_) {
        if (!retryPage_.isEmpty()) {
            next = retryPage_.first().track;
            if (retryReferences_ && (!entries_.contains(key(next)) || !entries_[key(next)].complete) &&
                retryFallbackKey_ != key(next)) {
                loadReferenceFallback(next);
                return;
            }
            retryActive_ = true;
            const auto kind = retryPage_.first().kind;
            publishExisting = !retryReferences_ &&
                              (kind == "shared_cache_write" || kind == "shared_publish_timeout") &&
                              sharedConfigured(next);
        } else {
            flushOutcomes();
            fetchRetries(retryUpper_ == 0);
        }
    }
    if (!next.id && allowBackground && !failureWriting_ && batchRunning_ && !retryMode_) {
        int skipped = 0;
        while (!batchPage_.isEmpty() && skipped++ < OutcomeChunkSize && batchRunning_ &&
               pendingOutcomes_.size() < OutcomeChunkSize) {
            const auto item = batchPage_.first();
            if (parallelOwns(item.first))
                break;
            const auto cached = entries_.constFind(key(item.first));
            const bool accurateCached = item.second == 1 || (cached != entries_.cend() && cached->stored &&
                                                             cached->complete && !cached->approximate);
            if (accurateCached && sharedConfigured(item.first)) {
                if (preparationConcurrency_ > 1)
                    break;
                next = item.first;
                batchActive_ = true;
                publishExisting = true;
                break;
            }
            if (item.second || (cached != entries_.cend() && cached->stored && !cached->verificationDue)) {
                advanceBatch(item.first, true,
                             item.second == 2 || (cached != entries_.cend() && cached->approximate));
                continue;
            }
            if (preparationConcurrency_ > 1)
                break;
            next = item.first;
            batchActive_ = true;
            break;
        }
        if (!next.id) {
            if (batchPage_.isEmpty())
                fetchBatch();
            else if (preparationConcurrency_ == 1 && pendingOutcomes_.size() < OutcomeChunkSize)
                QTimer::singleShot(0, this, &WaveQueue::schedule);
        }
    }
    if (!next.id) {
        scheduleParallel();
        return;
    }
    if (!parallelWorker_) {
        int physicalJobs = 0;
        for (const auto &slot : parallel_)
            physicalJobs += slot.worker && slot.worker->active_.id;
        // A newly visible or selected track takes an existing slot, never an extra
        // decoder. Cancellation is asynchronous; resume when those workers exit.
        if (physicalJobs >= preparationConcurrency_ ||
            (physicalJobs > 0 && key(next) == key(selected_))) {
            stopParallel();
            return;
        }
    }
    if (parallelWorker_)
        publishExisting = parallelPublish_;
    active_ = next;
    sourceRepairAttempted_ = false;
    ++sourceRepairGeneration_;
    readBytes_ = decodedMs_ = 0;
    publishing_ = publishExisting;
    background_ = parallelWorker_ || publishExisting || (retryActive_ && retryReferences_) ||
                  key(next) != key(selected_);
    const auto k = key(next);
    RowWave entry = entries_.value(k);
    preserveActiveView_ = retryActive_ && retryReferences_ && entry.complete && entry.stored;
    entry.failed = false;
    entry.failureKind.clear();
    entry.message = publishExisting  ? "正在同步已有波形到共享缓存"
                    : entry.complete ? "本机缓存已显示 · 正在后台核验原件"
                                     : "正在读取原音乐…";
    entry.verificationDue = false;
    if (!publishExisting)
        entry.stored = false;
    entry.touched = ++clock_;
    entries_[k] = entry;
    cancelled_ = false;
    waiting_ = false;
    submitActiveJob();
    scheduleParallel();
}
void WaveQueue::submitActiveJob() {
    if (!active_.id || cancelled_ || repairingSource_)
        return;
    const auto next = active_;
    QString shared, sharedLibrary;
    for (const auto &l : libraries_)
        if (l.id == next.libraryId) {
            shared = l.sharedCache;
            sharedLibrary = l.sharedId;
        }
    const QJsonObject config{
        {"path", next.sourcePath()},
        {"compatiblePath", next.compatiblePath},
        {"compatibleSha256", QString::fromLatin1(next.compatibleSha256)},
        {"compatibleSize", next.compatibleSize},
        {"compatibleModified", next.compatibleModified},
        {"verificationCache", verificationCache_},
        {"relative", next.relativePath},
        {"library", next.libraryId},
        {"local", localCache_},
        {"shared", shared},
        {"sharedLibrary", sharedLibrary},
        {"operation", publishing_ ? QString("publish_existing") : QString("wave")},
        {"indexedSize", next.size},
        {"indexedModified", next.modified},
        {"background", background_},
        {"readBytesPerSecond", parallelWorker_ ? batchRate_ : background_ ? backgroundJobRate() : 0}};
    activeRate_ = config["readBytesPerSecond"].toInteger();
    if (!process_)
        startProcess();
    if (!process_)
        return; // FailedToStart may be emitted synchronously on Windows.
    // QProcess buffers writes made while it is starting.
    process_->write(QJsonDocument(config).toJson(QJsonDocument::Compact) + "\n");
    jobClock_.restart();
    activityClock_.restart();
    workerWatchdog_.start(500);
    emit changed(next);
    emit preparationChanged();
}
