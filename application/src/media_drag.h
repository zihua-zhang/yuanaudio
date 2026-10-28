#pragma once
#include "index.h"
#include <QFuture>

class QWidget;
struct MediaDragResult {
    QList<QUrl> urls;
    QString error;
};

// Metadata-only MIME lookup. With a catalog, workspace snapshots are references:
// use the current indexed version and current compatible mapping, never a stale path.
QList<QUrl> mediaFileUrls(const QList<Track> &tracks, const QString &catalogPath = {},
                         QString *error = nullptr);
// Full compatible-copy SHA checks run in a worker, without blocking the UI thread.
QFuture<MediaDragResult> verifiedMediaUrlsAsync(const QList<Track> &tracks,
                                              const QString &catalogPath = {});
// Native drag entry points wait with a responsive, cancellable progress dialog.
QList<QUrl> prepareMediaDragUrls(QWidget *parent, const QList<Track> &tracks,
                                const QString &catalogPath = {}, QString *error = nullptr);
