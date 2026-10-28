#include "media_drag.h"
#include "media_source.h"
#include <QEventLoop>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QPromise>
#include <QtConcurrent>

namespace {
QList<Track> currentTracks(const QList<Track> &snapshots, const QString &catalogPath, QString *error) {
    if (error)
        error->clear();
    if (catalogPath.isEmpty())
        return snapshots;
    if (!QFileInfo::exists(catalogPath)) {
        if (error)
            *error = "当前素材索引不可访问，无法核验拖出文件。";
        return {};
    }
    Index catalog(catalogPath);
    QString lookupError;
    if (!catalog.open(&lookupError)) {
        if (error)
            *error = "无法读取当前素材索引：" + lookupError;
        return {};
    }
    QList<Track> tracks;
    for (const auto &snapshot : snapshots) {
        const auto current = catalog.trackByIdentity(snapshot.libraryId, snapshot.relativePath);
        if (!current) {
            if (error)
                *error = catalog.lastError().isEmpty() ? QString("这首音乐已不在当前索引中，请刷新后再拖出。")
                                                      : catalog.lastError();
            return {};
        }
        tracks.append(*current);
    }
    return tracks;
}
} // namespace

QList<QUrl> mediaFileUrls(const QList<Track> &snapshots, const QString &catalogPath, QString *error) {
    QString detail;
    const auto tracks = currentTracks(snapshots, catalogPath, &detail);
    QList<QUrl> urls;
    if (detail.isEmpty())
        for (const auto &track : tracks) {
            if (!track.sourceMatches(&detail)) {
                urls.clear();
                break;
            }
            urls.append(QUrl::fromLocalFile(track.sourcePath()));
        }
    if (error)
        *error = detail;
    return urls;
}

QFuture<MediaDragResult> verifiedMediaUrlsAsync(const QList<Track> &snapshots, const QString &catalogPath) {
    return QtConcurrent::run([snapshots, catalogPath](QPromise<MediaDragResult> &promise) {
        MediaDragResult result;
        const auto tracks = currentTracks(snapshots, catalogPath, &result.error);
        if (!result.error.isEmpty()) {
            promise.addResult(result);
            return;
        }
        for (const auto &track : tracks) {
            if (promise.isCanceled())
                return;
            const auto path = verifiedMediaPath(track, &result.error);
            if (path.isEmpty()) {
                result.urls.clear();
                if (result.error.isEmpty())
                    result.error = "无法核验拖出文件，请刷新后重试。";
                promise.addResult(result);
                return;
            }
            result.urls.append(QUrl::fromLocalFile(path));
        }
        if (!promise.isCanceled())
            promise.addResult(result);
    });
}

QList<QUrl> prepareMediaDragUrls(QWidget *parent, const QList<Track> &tracks,
                                const QString &catalogPath, QString *error) {
    if (error)
        error->clear();
    if (tracks.isEmpty())
        return {};
    auto future = verifiedMediaUrlsAsync(tracks, catalogPath);
    QFutureWatcher<MediaDragResult> watcher;
    QEventLoop loop;
    QProgressDialog progress("正在核验拖出文件…", "取消", 0, 0, parent);
    progress.setWindowTitle("核验音频");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(250);
    progress.setValue(0);
    bool cancelled = false;
    QObject::connect(&watcher, &QFutureWatcher<MediaDragResult>::finished, &loop, &QEventLoop::quit);
    QObject::connect(&progress, &QProgressDialog::canceled, &loop, [&] {
        cancelled = true;
        future.cancel(); // Finish at most the current read; do not hash the remaining selection.
        loop.quit();
    });
    watcher.setFuture(future);
    if (!future.isFinished())
        loop.exec();
    // close() emits QProgressDialog::canceled even when the worker completed.
    progress.hide();
    if (cancelled) {
        if (error)
            *error = "已取消文件拖出。";
        return {};
    }
    const auto result = future.result();
    if (error)
        *error = result.error;
    return result.urls;
}
