#include "wave_queue.h"
#include <QTimer>

qint64 WaveQueue::backgroundJobRate() const {
    if (!batchRunning_)
        return 1048576;
    // A fixed split leaves unused capacity when fewer jobs can run, but never multiplies
    // the user's per-client budget. Retries are deliberately single-job operations.
    return batchRate_ == 0 ? 0 : batchRate_ / (retryMode_ ? 1 : preparationConcurrency_);
}

int WaveQueue::parallelActiveCount() const {
    int count = 0;
    for (const auto &slot : parallel_)
        count += slot.track.id && !slot.done;
    return count;
}

bool WaveQueue::parallelOwns(const Track &track) const {
    for (const auto &slot : parallel_)
        if (slot.track.id && key(slot.track) == key(track))
            return true;
    return false;
}

void WaveQueue::setPreparationConcurrency(int count) {
    const int limit = count == 2 || count == 4 ? count : 1;
    if (preparationConcurrency_ == limit)
        return;
    stopParallel();
    preparationConcurrency_ = limit;
    if (active_.id && background_)
        cancel();
    savePreparation();
    QTimer::singleShot(0, this, &WaveQueue::schedule);
}

void WaveQueue::stopParallel(bool discardCompleted) {
    for (auto &slot : parallel_) {
        if (!slot.track.id || (slot.done && !discardCompleted))
            continue;
        const auto k = key(slot.track);
        if (slot.worker) {
            slot.worker->selected_ = {};
            slot.worker->cancel();
        }
        // Partial pixels are not a completed cache. A later selection must be able to
        // retry; completed caches stay on disk even if their ordered receipt is discarded.
        const auto found = entries_.constFind(k);
        if (found != entries_.cend() && !found->stored)
            entries_.remove(k);
        slot.track = {};
        slot.done = slot.success = false;
    }
    // Reuse helpers while preparing the library, release their Qt/decoder processes
    // when paused, yielding to foreground work, or done. Keep only lightweight slots.
    for (auto &slot : parallel_) {
        auto *child = slot.worker;
        if (child && !child->active_.id && child->process_ && !child->cancelled_) {
            child->cancelled_ = true;
            child->process_->kill();
        }
    }
}

bool WaveQueue::drainParallel() {
    bool advanced = false;
    while (batchRunning_ && !batchPage_.isEmpty() && pendingOutcomes_.size() < OutcomeChunkSize) {
        ParallelSlot *ready = nullptr;
        for (auto &slot : parallel_)
            if (slot.done && slot.track.id && key(slot.track) == key(batchPage_.first().first)) {
                ready = &slot;
                break;
            }
        if (!ready)
            break;
        const auto track = ready->track;
        const bool success = ready->success;
        ready->track = {};
        ready->done = ready->success = false;
        // Only a contiguous prefix advances the durable cursor. Out-of-order completion
        // cannot skip an unfinished file after pause, process exit or a failed ledger write.
        advanceBatch(track, success, entries_.value(key(track)).approximate);
        advanced = true;
    }
    if (!batchRunning_)
        stopParallel();
    return advanced;
}

void WaveQueue::scheduleParallel() {
    if (parallelWorker_ || preparationConcurrency_ <= 1 || !batchRunning_ || retryMode_ ||
        !deferReason_.isEmpty() || failureWriting_ || failureWrites_->retryNeeded.load() ||
        pendingOutcomes_.size() >= OutcomeChunkSize || cancelled_ || (active_.id && !background_))
        return;
    // Bound both running jobs and out-of-order receipts to four. A slow first file can
    // temporarily leave a worker idle, keeping memory and resume bookkeeping predictable.
    const int window = qMin(preparationConcurrency_, int(batchPage_.size()));
    for (int i = 0; i < window; ++i) {
        // Progress signals can synchronously apply a newer foreground/pressure policy.
        if (!batchRunning_ || !deferReason_.isEmpty() || cancelled_)
            break;
        const auto [track, cached] = batchPage_[i];
        if (key(track) == key(active_) || parallelOwns(track))
            continue;
        if (parallelActiveCount() + (active_.id ? 1 : 0) >= preparationConcurrency_)
            break;
        bool shared = false;
        for (const auto &library : libraries_)
            if (library.id == track.libraryId)
                shared = !library.sharedCache.isEmpty();
        const auto entry = entries_.constFind(key(track));
        const bool stored = cached || (entry != entries_.cend() && entry->stored && !entry->verificationDue);
        const bool accurate = cached == 1 ||
                              (entry != entries_.cend() && entry->stored && entry->complete && !entry->approximate);
        const bool publish = shared && accurate;
        if (stored && !publish)
            continue; // The ordered main queue consumes local cache hits without a process.
        ParallelSlot *available = nullptr;
        for (auto &slot : parallel_)
            if (!slot.track.id && (!slot.worker || (!slot.worker->active_.id && !slot.worker->cancelled_))) {
                available = &slot;
                break;
            }
        if (!available)
            break;
        auto &slot = *available;
        if (!slot.worker) {
            slot.worker = new WaveQueue(localCache_, this);
            auto *child = slot.worker;
            child->parallelWorker_ = true;
            child->workerProgram_ = workerProgram_;
            connect(child, &WaveQueue::changed, this, [this, available, child](const Track &changedTrack) {
                if (!available->track.id || available->done || key(available->track) != key(changedTrack))
                    return;
                if (const auto *entry = child->find(changedTrack)) {
                    entries_[key(changedTrack)] = *entry;
                    entries_[key(changedTrack)].touched = ++clock_;
                    emit changed(changedTrack);
                }
            });
            connect(child, &WaveQueue::jobFinished, this,
                    [this, available, child](const Track &track, bool success) {
                        if (!available->track.id || available->done || key(available->track) != key(track))
                            return;
                        if (const auto *entry = child->find(track)) {
                            entries_[key(track)] = *entry;
                            entries_[key(track)].touched = ++clock_;
                        }
                        child->selected_ = {};
                        available->done = true;
                        available->success = success;
                        emit changed(track);
                        emit preparationChanged();
                        QTimer::singleShot(0, this, &WaveQueue::schedule);
                    });
            connect(child, &WaveQueue::preparationChanged, this, [this, child] {
                emit preparationChanged();
                if (!child->active_.id)
                    QTimer::singleShot(0, this, &WaveQueue::schedule);
            });
        }
        slot.track = track;
        slot.done = slot.success = false;
        auto *child = slot.worker;
        child->libraries_ = libraries_;
        child->batchRate_ = backgroundJobRate();
        child->parallelPublish_ = publish;
        child->entries_.clear();
        child->localChecked_.clear();
        child->localChecked_.insert(key(track));
        child->selected_ = track;
        // Start through the same persistent worker protocol, cache validation, timeout,
        // lease and decoder path as the priority job, without a second metadata writer.
        child->schedule();
    }
}
