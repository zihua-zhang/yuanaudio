#include "index.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    // Pass a SQLite backup of the index, never a live production database.
    if (app.arguments().size() != 2 || !QFileInfo::exists(app.arguments()[1]))
        return 1;
    const auto path = app.arguments()[1];
    Index seed(path);
    if (!seed.open() || seed.count() == 0)
        return 2;
    QStringList queries{"", "伤感", "紧张", "温暖", "悬疑", "轻松", "情绪", "Piano", "音", "不存在的配乐XYZ"};
    for (const auto &lib : seed.libraries()) {
        const auto folders = seed.folders(lib.id);
        for (int i = 0; i < folders.size() && queries.size() < 20; i += qMax(1, int(folders.size() / 10)))
            queries << QFileInfo(folders[i]).fileName();
    }
    for (const auto &track : seed.search("").tracks) {
        queries << QFileInfo(track.name).completeBaseName();
        if (queries.size() >= 26)
            break;
    }
    QList<double> times;
    int hits = 0;
    for (int round = 0; round < 20; ++round)
        for (const auto &query : queries) {
            QElapsedTimer timer;
            timer.start();
            Index db(path);
            if (!db.open())
                return 3;
            const auto result = db.search(query);
            if (!db.lastError().isEmpty())
                return 4;
            hits += !result.tracks.isEmpty();
            times << timer.nsecsElapsed() / 1000000.0;
        }
    std::sort(times.begin(), times.end());
    const auto report = QJsonDocument(QJsonObject{{"rows", seed.count()},
                                                  {"queries", queries.size()},
                                                  {"samples", times.size()},
                                                  {"nonempty_samples", hits},
                                                  {"includes_connection_open", true},
                                                  {"p50_ms", times[times.size() / 2]},
                                                  {"p95_ms", times[int(times.size() * 0.95) - 1]},
                                                  {"max_ms", times.last()}})
                            .toJson(QJsonDocument::Compact);
    fwrite(report.constData(), 1, report.size(), stdout);
    return 0;
}
