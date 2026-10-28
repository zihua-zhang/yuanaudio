#include "index.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temporary;
    const auto database = temporary.filePath("benchmark.sqlite");
    QString error;
    if (argc == 2 && !Index::importSnapshot(app.arguments()[1], database, &error))
        return 1;
    Index seed(database);
    QElapsedTimer prepare;
    prepare.start();
    if (!seed.open(&error))
        return 2;
    if (argc != 2) {
        if (!seed.saveLibrary({"benchmark", "合成测试", "Z:/offline", ""}))
            return 3;
        QStringList folders;
        for (int i = 0; i < 20000; ++i)
            folders << QString("分类%1/伤感").arg(i, 5, 10, QChar('0'));
        if (!seed.upsertFolders("benchmark", folders))
            return 4;
        for (int batch = 0; batch < 100; ++batch) {
            QList<Track> tracks;
            for (int i = batch * 1000; i < (batch + 1) * 1000; ++i) {
                Track track;
                track.libraryId = "benchmark";
                track.folder = folders[i % folders.size()];
                track.name = QString("配乐%1.wav").arg(i, 6, 10, QChar('0'));
                track.relativePath = track.folder + "/" + track.name;
                track.size = 1000;
                track.modified = 1000;
                tracks << track;
            }
            if (!seed.upsert(tracks))
                return 5;
        }
    }
    const auto prepareMs = prepare.elapsed();
    QStringList queries{"伤感", "紧张", "音", "PIANO", "不存在的文件夹XYZ", "分类19999", "古风 伤感"};
    qint64 folders = 0;
    for (const auto &lib : seed.libraries()) {
        const auto paths = seed.folders(lib.id);
        folders += paths.size();
        for (int i = 1; i < paths.size() && queries.size() < 27; i += qMax(1, int(paths.size() / 20)))
            queries << QFileInfo(paths[i]).fileName();
    }
    QList<double> timings;
    for (int round = 0; round < 20; ++round)
        for (const auto &query : queries) {
            QElapsedTimer timer;
            timer.start();
            Index db(database);
            if (!db.open())
                return 6;
            const auto page = db.findFolders(query);
            if (!page.error.isEmpty())
                return 7;
            timings << timer.nsecsElapsed() / 1e6;
        }
    std::sort(timings.begin(), timings.end());
    const auto bytes =
        QJsonDocument(QJsonObject{{"tracks", seed.count()},
                                  {"folders_including_roots", folders},
                                  {"prepare_ms", prepareMs},
                                  {"samples", timings.size()},
                                  {"includes_connection_open", true},
                                  {"p50_ms", timings[timings.size() / 2]},
                                  {"p95_ms", timings[int(timings.size() * .95) - 1]},
                                  {"max_ms", timings.last()},
                                  {"source", argc == 2 ? "local-index-snapshot" : "synthetic"}})
            .toJson();
    fwrite(bytes.constData(), 1, bytes.size(), stdout);
    return 0;
}
