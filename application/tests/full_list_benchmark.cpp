#include "full_list_fixture.h"
#include "window.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QTemporaryDir>
#include <cstdio>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#endif

static QJsonObject memory() {
#ifdef Q_OS_WIN
    using Query = BOOL(WINAPI *)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    const auto query = reinterpret_cast<Query>(QLibrary::resolve("kernel32", "K32GetProcessMemoryInfo"));
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (query &&
        query(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&counters), sizeof(counters)))
        return {{"working_set_bytes", qint64(counters.WorkingSetSize)},
                {"private_bytes", qint64(counters.PrivateUsage)}};
#endif
    return {};
}
class CountDelegate : public QStyledItemDelegate {
  public:
    mutable QSet<int> rows;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        rows.insert(index.row());
        QStyledItemDelegate::paint(p, option, index);
    }
};
class PaintTable : public TrackTable {
  public:
    QElapsedTimer *frameClock = nullptr;
    double firstPaintMs = -1;

  protected:
    void paintEvent(QPaintEvent *event) override {
        TrackTable::paintEvent(event);
        if (frameClock && firstPaintMs < 0)
            firstPaintMs = frameClock->nsecsElapsed() / 1e6;
    }
};
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const int count = app.arguments().size() > 1 ? app.arguments()[1].toInt() : 100000;
    if (count != 60000 && count != 100000)
        return 1;
    QTemporaryDir data;
    Index db(data.filePath("library.sqlite"));
    QString error;
    if (!seedFullList(db, data.filePath("no-audio"), count, &error)) {
        fprintf(stderr, "%s\n", qPrintable(error));
        return 2;
    }
    TrackModel model;
    PaintTable table;
    CountDelegate delegate;
    table.setModel(&model);
    table.setItemDelegate(&delegate);
    table.resize(1120, 760);
    table.verticalHeader()->hide();
    table.verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table.verticalHeader()->setDefaultSectionSize(52);
    table.setColumnWidth(TrackModel::Favorite, 40);
    table.setColumnWidth(TrackModel::Name, 300);
    table.horizontalHeader()->setSectionResizeMode(TrackModel::Wave, QHeaderView::Stretch);
    for (const int column : {TrackModel::Folder, TrackModel::Format, TrackModel::Tags})
        table.setColumnHidden(column, true);
    table.show();
    app.processEvents();
    const auto before = memory();
    QElapsedTimer timer;
    timer.start();
    table.frameClock = &timer;
    auto page = db.searchAll("");
    const double queryMs = timer.nsecsElapsed() / 1e6;
    if (!page.error.isEmpty() || page.tracks.size() != count)
        return 3;
    model.replace(std::move(page.tracks));
    const double modelReadyMs = timer.nsecsElapsed() / 1e6;
    table.doItemsLayout();
    app.processEvents();
    table.grab();
    const double firstPaintMs = table.firstPaintMs;
    const auto after = memory();
    const int firstPaintedRows = delegate.rows.size();
    delegate.rows.clear();
    timer.restart();
    table.firstPaintMs = -1;
    table.scrollToBottom();
    app.processEvents();
    table.grab();
    const double bottomMs = table.firstPaintMs;
    table.frameClock = nullptr;
    if (!delegate.rows.contains(count - 1))
        return 4;
    const auto last = model.tracks.last();
    timer.restart();
    qint64 checksum = 0;
    for (int i = 0; i < 1000000; ++i)
        checksum += model.rowForTrack(last);
    const double lookupMs = timer.nsecsElapsed() / 1e6;
    if (checksum != qint64(count - 1) * 1000000)
        return 5;
    timer.restart();
    const auto chinese = db.searchAll("雨夜 Music");
    const double chineseMs = timer.nsecsElapsed() / 1e6;
    if (chinese.tracks.size() != count / 2)
        return 6;
    const auto json =
        QJsonDocument(QJsonObject{{"scope", "Synthetic local metadata and table render; no audio, waveform "
                                            "I/O, network, playback, or physical display presentation"},
                                  {"rows", count},
                                  {"metadata_query_ms", queryMs},
                                  {"metadata_and_model_ms", modelReadyMs},
                                  {"metadata_to_first_render_ms", firstPaintMs},
                                  {"first_render_rows", firstPaintedRows},
                                  {"jump_to_last_render_ms", bottomMs},
                                  {"last_render_rows", delegate.rows.size()},
                                  {"chinese_query_rows", chinese.tracks.size()},
                                  {"chinese_query_ms", chineseMs},
                                  {"one_million_versioned_row_lookups_ms", lookupMs},
                                  {"memory_before_metadata", before},
                                  {"memory_after_first_render", after}})
            .toJson();
    fwrite(json.constData(), 1, json.size(), stdout);
    return 0;
}
