#pragma once
#include "index.h"

// Synthetic local metadata only: no real audio, NAS paths, or production profiles are touched.
inline bool seedFullList(Index &db, const QString &root, int count, QString *error) {
    if (!db.open(error) || !db.saveLibrary({"first", "前六万首", root, ""}) ||
        !db.saveLibrary({"rest", "其余音乐", root, ""}))
        return false;
    for (int base = 0; base < count; base += 1000) {
        QList<Track> batch;
        for (int i = base; i < qMin(base + 1000, count); ++i) {
            Track track;
            track.libraryId = i < 60000 ? "first" : "rest";
            track.name = QString("Music_%1.wav").arg(i, 6, 10, QChar('0'));
            track.folder = i % 2 ? "短剧/追逐" : i % 10 ? "短剧/雨夜" : "短剧/雨夜/回忆";
            track.relativePath = track.folder + "/" + track.name;
            track.size = 1000 + i;
            track.modified = 1000;
            batch << track;
        }
        if (!db.upsert(batch, error))
            return false;
    }
    return true;
}
