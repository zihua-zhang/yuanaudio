#include "index.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    Index db(dir.filePath("benchmark.sqlite"));
    QString error;
    if (!db.open(&error) || !db.saveLibrary({"bench", "十万条合成目录", "Z:/not-connected", ""}))
        return 1;
    QElapsedTimer build;
    build.start();
    const QStringList folders = {"伤感/雨夜", "紧张/追逐", "温暖/重逢", "轻松/日常", "史诗/战斗",
                                 "悬疑/谜案", "爱情/告白", "回忆/童年", "喜剧/笑场", "悲伤/离别"};
    for (int batch = 0; batch < 100; ++batch) {
        QList<Track> tracks;
        for (int j = 0; j < 1000; ++j) {
            int n = batch * 1000 + j;
            Track t;
            t.libraryId = "bench";
            t.folder = "短剧/" + folders[n % 10];
            t.name = QString("Music_%1_Piano.wav").arg(n, 6, 10, QChar('0'));
            t.relativePath = t.folder + "/" + t.name;
            t.size = 1000 + n;
            t.modified = 1000;
            tracks << t;
        }
        if (!db.upsert(tracks, &error)) {
            fprintf(stderr, "%s\n", qPrintable(error));
            return 2;
        }
    }
    const auto buildMs = build.elapsed();
    QList<double> times;
    const QStringList queries = {"伤感",         "雨夜 Piano", "Music_012345", "紧张 追逐",
                                 "不存在的名称", "短剧",       "Piano",        "雨"};
    for (int i = 0; i < 160; ++i) {
        QElapsedTimer timer;
        timer.start();
        auto result = db.search(queries[i % queries.size()]);
        times << timer.nsecsElapsed() / 1000000.0;
        if (i % queries.size() != 4 && result.tracks.isEmpty())
            return 3;
    }
    std::sort(times.begin(), times.end());
    const auto bytes = QJsonDocument(QJsonObject{{"rows", db.count()},
                                                 {"samples", times.size()},
                                                 {"build_ms", buildMs},
                                                 {"p50_ms", times[times.size() / 2]},
                                                 {"p95_ms", times[int(times.size() * 0.95) - 1]},
                                                 {"p99_ms", times[int(times.size() * 0.99) - 1]},
                                                 {"max_ms", times.last()}})
                           .toJson(QJsonDocument::Compact);
    fwrite(bytes.constData(), 1, bytes.size(), stdout);
    fflush(stdout);
    return 0;
}
