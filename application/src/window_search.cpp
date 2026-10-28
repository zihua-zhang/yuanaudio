#include "window.h"
#include <QCheckBox>
#include <QFutureWatcher>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QtConcurrent>

void Window::refreshSearch(bool append) {
    if (!indexReady_ || musicComposing_ || catalogBusy_)
        return;
    // Kept for compatibility with old callers: there is no second music page to request.
    if (append)
        return;
    if (!inputPending_)
        inputClock_.restart();
    const bool fromTyping = inputPending_;
    inputPending_ = false;
    waveQueue_->setVisible({});
    viewportDebounce_.stop();
    const quint64 generation = ++queryGeneration_;
    *latestQuery_ = generation;
    const auto path = databasePath_, query = search_->text(), lib = scopeLibrary_, folder = scopeFolder_;
    const bool fav = favoriteScope_, recursive = subfolders_->isChecked();
    const int scroll =
        restoreScroll_ >= 0 ? restoreScroll_ : (fromTyping ? 0 : table_->verticalScrollBar()->value());
    more_->hide();
    more_->setEnabled(false);
    loading_ = true;
    lastSearchError_.clear();
    updateSearchFeedback();
    auto *watcher = new QFutureWatcher<SearchPage>(this);
    connect(watcher, &QFutureWatcher<SearchPage>::finished, this, [this, watcher, generation, scroll] {
        auto future = watcher->future();
        auto page = future.takeResult();
        watcher->deleteLater();
        if (generation != queryGeneration_ || page.cancelled)
            return;
        loading_ = false;
        if (!page.error.isEmpty()) {
            lastSearchError_ = page.error;
            results_->setText(model_->rowCount() ? "搜索未完成 · 保留之前的结果" : "搜索未完成");
            results_->setToolTip(page.error);
            setStatus("搜索未完成，请重试；已显示的音乐与试听保持不变。");
            restoreScroll_ = -1;
            historyRestoring_ = false;
            viewportDebounce_.start();
            updateSearchFeedback();
            return;
        }
        indexGeneration_ = page.generation;
        lastSearchError_.clear();
        hasMore_ = false;
        pageOffset_ = 0;
        const int currentRow = table_->currentIndex().row();
        const Track current =
            currentRow >= 0 && currentRow < model_->rowCount() ? model_->tracks[currentRow] : selected_;
        QSignalBlocker selection(table_->selectionModel());
        model_->replace(std::move(page.tracks));
        updateSearchFeedback();
        table_->doItemsLayout();
        const int retainedRow = model_->rowForTrack(current);
        if (retainedRow >= 0)
            table_->selectRow(retainedRow);
        table_->verticalScrollBar()->setValue(scroll);
        restoreScroll_ = -1;
        historyRestoring_ = false;
        const auto readyMs = inputClock_.isValid() ? inputClock_.elapsed() : page.elapsedMs;
        recordMeasurement("music-results", readyMs);
        results_->setToolTip(
            QString("本机查询 %1 ms · 输入至全部结果就绪 %2 ms").arg(page.elapsedMs).arg(readyMs));
        results_->setText(QString("%1 首音乐").arg(model_->rowCount()));
        viewportDebounce_.start();
        updateSearchFeedback();
    });
    watcher->setFuture(QtConcurrent::run(
        &searchPool_, [path, query, lib, folder, fav, recursive, generation, latest = latestQuery_] {
            const auto cancelled = [latest, generation] { return latest->load() != generation; };
            if (cancelled()) {
                SearchPage page;
                page.cancelled = true;
                return page;
            }
            Index db(path);
            QString error;
            if (!db.open(&error)) {
                SearchPage page;
                page.error = error;
                return page;
            }
            // Cancellation is checked before SQL and every 128 result rows. SQLite's current
            // statement/sort is not interrupted; it stays off the UI thread and is discarded.
            return db.searchAll(query, lib, folder, fav, recursive, cancelled);
        }));
}
