#include "window.h"
#include "preference_switch.h"
#include "theme.h"
#include "appearance.h"
#include "ui_controls.h"
#include <QAction>
#include <QApplication>
#include <QBoxLayout>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHelpEvent>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QStyleHints>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextLayout>
#include <QToolTip>
#include <QTreeWidget>
#include <QUuid>
#include <QtConcurrent>
#include <cmath>

static QString clockText(qint64 ms) {
    const auto secs = qMax(qint64(0), ms / 1000);
    return QString("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QChar('0'));
}
struct DisplayDuration {
    qint64 milliseconds = 0;
    bool estimated = false;
};
static DisplayDuration displayDuration(const Track &track, const RowWave *entry) {
    if (track.duration > 0)
        return {track.duration, false};
    if (!entry)
        return {};
    // A failed refresh may retain a complete cached reference view.
    const bool usableCachedWave = entry->complete && !entry->data.buckets.isEmpty();
    if (entry->failed && !usableCachedWave)
        return {};
    const qint64 decodedMs =
        entry->data.sampleRate > 0 ? entry->data.frames * 1000 / entry->data.sampleRate : 0;
    if (usableCachedWave && !entry->approximate && decodedMs > 0)
        return {decodedMs, false};
    return {qMax(decodedMs, entry->estimatedDuration), true};
}
// Paint literal filenames; no HTML interpretation and only visible text is laid out.
static void highlightedText(QPainter *p, const QRect &rect, const QString &text, const QFont &font,
                            const QColor &color, const QString &query, Qt::TextElideMode elide) {
    const auto shown = QFontMetrics(font).elidedText(text, elide, rect.width());
    QTextLayout layout(shown, font);
    QList<QTextLayout::FormatRange> formats;
    const auto terms = query.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    for (const auto &term : terms) {
        int pos = 0;
        while ((pos = shown.indexOf(term, pos, Qt::CaseInsensitive)) >= 0) {
            QTextCharFormat format;
            format.setBackground(theme::current().searchMark);
            format.setForeground(theme::current().searchMarkText);
            formats.append({pos, int(term.size()), format});
            pos += term.size();
        }
    }
    layout.setFormats(formats);
    layout.beginLayout();
    auto line = layout.createLine();
    line.setLineWidth(rect.width());
    layout.endLayout();
    p->setPen(color);
    layout.draw(p, QPointF(rect.left(), rect.top() + (rect.height() - line.height()) / 2));
}
class FolderMatchDelegate : public QStyledItemDelegate {
  public:
    FolderMatchDelegate(QLineEdit *query, QObject *parent) : QStyledItemDelegate(parent), query_(query) {}
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        auto background = option;
        initStyleOption(&background, index);
        background.text.clear();
        const auto *widget = option.widget;
        (widget ? widget->style() : QApplication::style())
            ->drawControl(QStyle::CE_ItemViewItem, &background, p, widget);
        const auto lines = index.data().toString().split('\n');
        auto rect = option.rect.adjusted(8, 4, -8, -4);
        p->save();
        p->setClipRect(rect);
        const int half = rect.height() / 2;
        highlightedText(p, QRect(rect.left(), rect.top(), rect.width(), half), lines.value(0), option.font,
                        theme::current().text, query_->text(), Qt::ElideRight);
        auto small = option.font;
        small.setPixelSize(11);
        highlightedText(p, QRect(rect.left(), rect.top() + half, rect.width(), half), lines.value(1), small,
                        theme::current().muted, query_->text(), Qt::ElideMiddle);
        p->restore();
    }

  private:
    QLineEdit *query_;
};
class ElidedLabel : public QLabel {
  public:
    using QLabel::QLabel;
    QSize sizeHint() const override {
        return {100, QLabel::sizeHint().height()};
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideMiddle, contentsRect().width()));
    }
};
class TrackNameDelegate : public QStyledItemDelegate {
  public:
    TrackNameDelegate(QLineEdit *query, QObject *parent) : QStyledItemDelegate(parent), query_(query) {}
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        auto background = option;
        initStyleOption(&background, index);
        background.text.clear();
        const auto *widget = option.widget;
        (widget ? widget->style() : QApplication::style())
            ->drawControl(QStyle::CE_ItemViewItem, &background, p, widget);
        const auto *model = qobject_cast<const TrackModel *>(index.model());
        if (!model || index.row() >= model->tracks.size())
            return;
        const auto &track = model->tracks[index.row()];
        const auto r = option.rect.adjusted(9, 5, -9, -5);
        p->save();
        p->setClipRect(r);
        p->setFont(option.font);
        p->setPen(theme::current().text);
        highlightedText(p, r.adjusted(0, 0, 0, -r.height() / 2), track.name, option.font, theme::current().text,
                        query_->text(), Qt::ElideRight);
        QFont smaller = option.font;
        smaller.setPixelSize(12);
        p->setFont(smaller);
        p->setPen(theme::current().muted);
        highlightedText(p, r.adjusted(0, r.height() / 2, 0, 0), track.folder == "." ? "根目录" : track.folder,
                        smaller, theme::current().muted, query_->text(), Qt::ElideMiddle);
        p->restore();
    }

  private:
    QLineEdit *query_;
};
class FavoriteDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        auto background = option;
        initStyleOption(&background, index);
        background.text.clear();
        const auto *widget = option.widget;
        (widget ? widget->style() : QApplication::style())
            ->drawControl(QStyle::CE_ItemViewItem, &background, p, widget);
        const auto *model = qobject_cast<const TrackModel *>(index.model());
        if (!model || index.row() < 0 || index.row() >= model->tracks.size())
            return;
        const bool favorite = model->tracks[index.row()].favorite;
        const auto &colors = theme::current();
        const auto glyph = ui::icon(favorite ? ui::Icon::StarFilled : ui::Icon::Star,
                                    favorite ? colors.accent : colors.faint, 18);
        glyph.paint(p, QRect(option.rect.center() - QPoint(9, 9), QSize(18, 18)));
    }
};
int TrackModel::rowCount(const QModelIndex &p) const {
    return p.isValid() ? 0 : tracks.size();
}
int TrackModel::columnCount(const QModelIndex &p) const {
    return p.isValid() ? 0 : ColumnCount;
}
QVariant TrackModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() >= tracks.size())
        return {};
    const auto &t = tracks[i.row()];
    if (role == Qt::ToolTipRole && i.column() == Wave)
        return "单击波形定位试听 · 按住拖动整首原文件到达芬奇";
    if (role == Qt::ToolTipRole)
        return t.absolutePath();
    if (role == Qt::TextAlignmentRole && i.column() == Duration)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (role == Qt::ForegroundRole && i.column() == 0)
        return t.favorite ? theme::current().warningText : theme::current().faint;
    if (role == Qt::DisplayRole)
        switch (i.column()) {
        case 0:
            return t.favorite ? "★" : "☆";
        case 1:
            return t.name;
        case Wave:
            return {};
        case Folder:
            return t.folder == "." ? "根目录" : t.folder;
        case Duration: {
            const auto duration = displayDuration(t, waveQueue_ ? waveQueue_->find(t) : nullptr);
            return duration.milliseconds > 0
                       ? (duration.estimated ? "约 " : "") + clockText(duration.milliseconds)
                       : QString("—");
        }
        case Format:
            return QFileInfo(t.name).suffix().toUpper();
        case Tags:
            return t.tags;
        }
    return {};
}
QVariant TrackModel::headerData(int s, Qt::Orientation o, int r) const {
    if (r == Qt::TextAlignmentRole && o == Qt::Horizontal && s == Duration)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    if (r == Qt::DisplayRole && o == Qt::Horizontal)
        return QStringList{"", "音乐名称", "波形", "所属文件夹", "时长", "格式", "个人标签"}.value(s);
    return {};
}
Qt::ItemFlags TrackModel::flags(const QModelIndex &i) const {
    return QAbstractTableModel::flags(i) | Qt::ItemIsDragEnabled;
}
QStringList TrackModel::mimeTypes() const {
    return {"text/uri-list"};
}
Qt::DropActions TrackModel::supportedDragActions() const {
    return Qt::CopyAction;
}
QMimeData *TrackModel::mimeData(const QModelIndexList &indexes) const {
    QList<Track> values;
    QSet<int> seen;
    for (const auto &i : indexes)
        if (i.isValid() && i.row() < tracks.size() && !seen.contains(i.row())) {
            seen.insert(i.row());
            values << tracks[i.row()];
        }
    auto *mime = new QMimeData;
    mime->setUrls(originalFileUrls(values));
    return mime;
}
void TrackModel::replace(QList<Track> values) {
    beginResetModel();
    tracks = std::move(values);
    rowsById_.clear();
    rowsById_.reserve(tracks.size());
    for (int row = 0; row < tracks.size(); ++row)
        rowsById_.insert(tracks[row].id, row);
    endResetModel();
}
static bool sameTrackVersion(const Track &a, const Track &b) {
    return a.id == b.id && a.libraryId == b.libraryId && a.relativePath == b.relativePath &&
           a.size == b.size && a.modified == b.modified;
}
int TrackModel::rowForId(qint64 id) const {
    return rowsById_.value(id, -1);
}
int TrackModel::rowForTrack(const Track &track) const {
    const int row = rowForId(track.id);
    return row >= 0 && sameTrackVersion(tracks[row], track) ? row : -1;
}
void TrackModel::append(QList<Track> values) {
    if (values.isEmpty())
        return;
    const int first = tracks.size();
    beginInsertRows({}, first, first + values.size() - 1);
    tracks.append(std::move(values));
    for (int row = first; row < tracks.size(); ++row)
        rowsById_.insert(tracks[row].id, row);
    endInsertRows();
}
void TrackTable::startDrag(Qt::DropActions) {
    if (!selectionModel() || selectionModel()->selectedRows().isEmpty())
        return;
    const auto rows = selectionModel()->selectedRows();
    executeFileDrag(model()->mimeData(rows),
                    model()->index(rows.first().row(), TrackModel::Name).data().toString());
}
void TrackTable::executeFileDrag(QMimeData *mime, const QString &label) {
    if (!mime || !mime->hasUrls()) {
        delete mime;
        return;
    }
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    const qreal scale = devicePixelRatioF();
    QPixmap preview(QSize(280, 54) * scale);
    preview.setDevicePixelRatio(scale);
    preview.fill(Qt::transparent);
    QPainter painter(&preview);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(theme::current().surface);
    painter.setPen(theme::current().strongBorder);
    painter.drawRoundedRect(QRectF(1, 1, 278, 52), 10, 10);
    painter.setFont(font());
    painter.setPen(theme::current().text);
    painter.drawText(QRect(14, 6, 252, 22), Qt::AlignVCenter,
                     fontMetrics().elidedText(label, Qt::ElideMiddle, 252));
    painter.setPen(theme::current().muted);
    painter.drawText(QRect(14, 29, 252, 18), "整首原文件");
    painter.end();
    drag->setPixmap(preview);
    drag->setHotSpot(QPoint(18, 20));
    drag->exec(Qt::CopyAction, Qt::CopyAction);
    drag->deleteLater();
}
void TrackTable::mousePressEvent(QMouseEvent *e) {
    const auto i = indexAt(e->position().toPoint());
    waveGesture_ = e->button() == Qt::LeftButton && i.isValid() && i.column() == TrackModel::Wave;
    if (waveGesture_) {
        pressedWave_ = i;
        pressedPoint_ = e->position().toPoint();
        setFocus(Qt::MouseFocusReason);
        e->accept();
        return;
    }
    QTableView::mousePressEvent(e);
}
void TrackTable::mouseMoveEvent(QMouseEvent *e) {
    if (waveGesture_) {
        if (pressedWave_.isValid() && (e->buttons() & Qt::LeftButton) &&
            (e->position().toPoint() - pressedPoint_).manhattanLength() >=
                QApplication::startDragDistance()) {
            const QModelIndex pressed = pressedWave_;
            pressedWave_ = QModelIndex(); // Consume before the native drag's nested event loop.
            executeFileDrag(model()->mimeData({pressed}),
                            model()->index(pressed.row(), TrackModel::Name).data().toString());
        }
        e->accept();
        return;
    }
    QTableView::mouseMoveEvent(e);
}
void TrackTable::mouseReleaseEvent(QMouseEvent *e) {
    if (waveGesture_) {
        waveGesture_ = false;
        const auto i = indexAt(e->position().toPoint());
        if (e->button() == Qt::LeftButton && pressedWave_.isValid() && i == pressedWave_) {
            const auto r = visualRect(i).adjusted(8, 0, -8, 0);
            emit waveClicked(i.row(), qBound(0.0, (e->position().x() - r.left()) / qMax(1, r.width()), 1.0));
        }
        pressedWave_ = QModelIndex();
        e->accept();
        return;
    }
    QTableView::mouseReleaseEvent(e);
}
void TrackTable::mouseDoubleClickEvent(QMouseEvent *e) {
    const auto i = indexAt(e->position().toPoint());
    if (i.isValid() && i.column() == TrackModel::Wave && e->button() == Qt::LeftButton) {
        mousePressEvent(e);
        return;
    }
    QTableView::mouseDoubleClickEvent(e);
}
void TrackTable::resizeEvent(QResizeEvent *e) {
    QTableView::resizeEvent(e);
    emit viewportChanged();
}
void TrackTable::scrollContentsBy(int dx, int dy) {
    QTableView::scrollContentsBy(dx, dy);
    emit viewportChanged();
}
RowWaveDelegate::RowWaveDelegate(WaveQueue *queue, QObject *parent)
    : QStyledItemDelegate(parent), queue_(queue) {
    connect(queue_, &WaveQueue::changed, this,
            [this](const Track &track) { renderer_.invalidate(WaveQueue::key(track)); });
}
bool RowWaveDelegate::helpEvent(QHelpEvent *event, QAbstractItemView *view,
                                const QStyleOptionViewItem &option, const QModelIndex &index) {
    const auto *model = qobject_cast<const TrackModel *>(index.model());
    if (!event || !view || !model || !index.isValid() || index.row() >= model->tracks.size())
        return false;
    const auto &track = model->tracks[index.row()];
    const auto *entry = queue_->find(track);
    const auto duration = displayDuration(track, entry);
    const qint64 length = duration.milliseconds;
    const auto r = option.rect.adjusted(8, 0, -8, 0);
    const double ratio = qBound(0.0, double(event->pos().x() - r.left()) / qMax(1, r.width()), 1.0);
    const auto position = length > 0 ? clockText(qint64(ratio * length)) + " / " +
                                           (duration.estimated ? "约 " : "") + clockText(length)
                                     : QString("时长准备中");
    QToolTip::showText(event->globalPos(),
                       position + (entry && entry->approximate ? " · 参考波形：位置仅供参考" : "") +
                           (entry && entry->approximate ? "\n" + entry->message : QString()) +
                           "\n单击从这里试听 · 拖动整首原文件",
                       view->viewport(), option.rect);
    return true;
}
void RowWaveDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    QStyledItemDelegate::paint(p, option, index);
    const auto *model = qobject_cast<const TrackModel *>(index.model());
    if (!model || index.row() >= model->tracks.size())
        return;
    const auto &track = model->tracks[index.row()];
    const auto *entry = queue_->find(track);
    const auto estimatedDuration = entry ? qMax(track.duration, entry->estimatedDuration) : track.duration;
    const QRect r = option.rect.adjusted(8, 7, -8, -7);
    p->save();
    p->setClipRect(r);
    p->setPen(theme::current().waveBaseline);
    p->drawLine(r.left(), r.center().y(), r.right(), r.center().y());
    if (!entry || entry->data.buckets.isEmpty() || (!entry->complete && estimatedDuration <= 0)) {
        p->setPen(entry && entry->failed ? theme::current().warningText : theme::current().muted);
        const auto text =
            entry && entry->failed ? "波形不可用 · 点击重试试听"
            : entry && entry->data.sampleRate > 0
                ? QString("已分析 %1 秒 · 整首轮廓生成中").arg(entry->data.frames / entry->data.sampleRate)
            : entry ? entry->message
                    : QString("等待准备 · 可先点击试听");
        p->drawText(r, Qt::AlignCenter, text);
        p->restore();
        return;
    }
    const auto &wave = entry->data;
    const auto key = WaveQueue::key(track);
    const auto selected = playingKey == key;
    const double fraction = !entry->complete && estimatedDuration > 0
                                ? qMin(1.0, double(wave.frames) * 1000 / wave.sampleRate / estimatedDuration)
                                : 1;
    const int playX = selected && duration > 0
                          ? qBound(0, int(double(position) / duration * r.width()), r.width() - 1)
                          : -1;
    renderer_.paint(p, r, key, wave, fraction, entry->approximate, playX);
    p->restore();
}
Window::Window(const QString &root, const QString &legacyDatabase, const QString &peakRoot)
    : dataRoot_(root), databasePath_(QDir(root).filePath("library.sqlite")), legacyDatabase_(legacyDatabase) {
    waveQueue_ = new WaveQueue(peakRoot.isEmpty() ? QDir(dataRoot_).filePath("peaks") : peakRoot, this);
    metadataPool_.setMaxThreadCount(1);
    folderPool_.setMaxThreadCount(1);
    catalogPool_.setMaxThreadCount(1);
    buildUi();
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this] { if (appearanceModeValue_ == "system") applyAppearance(); });
    waveQueue_->configurePreparation(databasePath_, QDir(dataRoot_).filePath("wave-preparation.json"));
    connect(waveQueue_, &WaveQueue::changed, this, &Window::updateWave);
    connect(waveQueue_, &WaveQueue::warning, this, [this](const Track &t, const QString &message) {
        if (WaveQueue::key(t) == WaveQueue::key(selected_))
            setStatus(message);
    });
    viewportDebounce_.setSingleShot(true);
    viewportDebounce_.setInterval(160);
    connect(&viewportDebounce_, &QTimer::timeout, this, &Window::updateVisibleWaves);
    durationWriteRetry_.setSingleShot(true);
    connect(&durationWriteRetry_, &QTimer::timeout, this, &Window::flushDurationWrite);
    connect(table_, &TrackTable::viewportChanged, this, [this] { viewportDebounce_.start(); });
    setWindowTitle("源音效管理工具 · 0.12.2");
    resize(1320, 850);
    setMinimumSize(980, 640);
    debounce_.setSingleShot(true);
    debounce_.setInterval(120);
    connect(&debounce_, &QTimer::timeout, this, [this] { refreshSearch(); });
    searchPool_.setMaxThreadCount(1);
    connect(search_, &QLineEdit::textChanged, this, [this] {
        inputClock_.restart();
        inputPending_ = true;
        *latestQuery_ = ++queryGeneration_;
        viewportDebounce_.stop();
        waveQueue_->setVisible({});
        if (!musicComposing_)
            debounce_.start();
    });
    playerWatchdog_.setInterval(1000);
    connect(&playerWatchdog_, &QTimer::timeout, this, [this] {
        if (player_ && player_->state() == QProcess::Running && playerResponse_.isValid() &&
            playerResponse_.elapsed() > 8000) {
            player_->kill();
            setStatus("试听读取超时，点击播放可重试；搜索仍可使用。");
        }
    });
    loadTimer_.setInterval(2000);
    connect(&loadTimer_, &QTimer::timeout, this, [this] {
        pressureGate_.observe(loadSampler_.sample());
        updateWorkPolicy();
    });
    connect(waveQueue_, &WaveQueue::preparationChanged, this, &Window::updateWorkPolicy);
    connect(editingPriority_, &QCheckBox::toggled, this, &Window::updateWorkPolicy);
    QSettings settings(QDir(dataRoot_).filePath("browser.ini"), QSettings::IniFormat);
    editingPriority_->setChecked(settings.value("editingPriority", true).toBool());
    volume_->setValue(qBound(0, settings.value("volume", 65).toInt(), 100));
    autoPlay_->setChecked(settings.value("autoPlay", false).toBool());
    if (settings.contains("geometry"))
        restoreGeometry(settings.value("geometry").toByteArray());
    if (settings.contains("splitter"))
        splitter_->restoreState(settings.value("splitter").toByteArray());
    scopeLibrary_ = settings.value("library").toString();
    scopeFolder_ = settings.value("folder").toString();
    favoriteScope_ = settings.value("favorite", false).toBool();
    subfolders_->setChecked(settings.value("recursive", true).toBool());
    search_->setText(settings.value("query").toString());
    restoreScroll_ = settings.value("scroll", 0).toInt();
    folderDebounce_.setSingleShot(true);
    folderDebounce_.setInterval(90);
    connect(&folderDebounce_, &QTimer::timeout, this, [this] { searchFolders(); });
    connect(folderSearch_, &QLineEdit::textChanged, this, [this] {
        *latestFolderQuery_ = ++foldersGeneration_;
        folderClock_.restart();
        if (!folderComposing_)
            folderDebounce_.start();
        folderResults_->clear();
        folderMatches_.clear();
        folderMoreButton_->hide();
        folderResults_->setVisible(!folderSearch_->text().trimmed().isEmpty());
        folderHint_->setVisible(!folderSearch_->text().isEmpty());
        folderHint_->setText(folderSearch_->text().isEmpty() ? "按名称查找，在目录树中定位"
                                                             : "正在查找文件夹…");
    });
    reloadLibraries();
    setStatus("正在打开本机索引，首次升级将保留旧版数据并建立独立副本…");
}
Window::~Window() {
    waveQueue_->pausePreparation();
    *latestFolderQuery_ = ++foldersGeneration_;
    *latestLocate_ = ++locateGeneration_;
    ++librariesGeneration_;
    ++*latestQuery_;
    QSettings preferences(QDir(dataRoot_).filePath("browser.ini"), QSettings::IniFormat);
    preferences.setValue("volume", volume_->value());
    preferences.setValue("autoPlay", autoPlay_->isChecked());
    preferences.setValue("editingPriority", editingPriority_->isChecked());
    preferences.setValue("geometry", saveGeometry());
    preferences.setValue("splitter", splitter_->saveState());
    if (indexReady_) {
        QSettings settings(QDir(dataRoot_).filePath("browser.ini"), QSettings::IniFormat);
        settings.setValue("library", scopeLibrary_);
        settings.setValue("folder", scopeFolder_);
        settings.setValue("favorite", favoriteScope_);
        settings.setValue("query", search_->text());
        settings.setValue("recursive", subfolders_->isChecked());
        settings.setValue("scroll", table_->verticalScrollBar()->value());
        QJsonArray expanded;
        std::function<void(const QModelIndex &)> remember = [&](const QModelIndex &parent) {
            for (int i = 0; i < folderModel_->rowCount(parent) && expanded.size() < 256; ++i) {
                const auto child = folderModel_->index(i, 0, parent);
                if (tree_->isExpanded(child)) {
                    expanded.append(QJsonObject{{"library", child.data(FolderModel::LibraryRole).toString()},
                                                {"path", child.data(FolderModel::PathRole).toString()}});
                    remember(child);
                }
            }
        };
        remember({});
        settings.setValue("expanded", QJsonDocument(expanded).toJson(QJsonDocument::Compact));
        QSaveFile metrics(QDir(dataRoot_).filePath("performance-last-session.json"));
        if (metrics.open(QIODevice::WriteOnly)) {
            metrics.write(QJsonDocument(measurements_).toJson());
            metrics.commit();
        }
    }
    for (auto *p : findChildren<QProcess *>()) {
        p->disconnect();
        if (p->state() != QProcess::NotRunning) {
            p->kill();
            p->waitForFinished(1000);
        }
    }
    if (!scanControlPath_.isEmpty())
        QFile::remove(scanControlPath_);
    folderPool_.clear();
    folderPool_.waitForDone();
    metadataPool_.waitForDone();
    catalogPool_.waitForDone();
    searchPool_.clear();
    searchPool_.waitForDone();
}
void Window::closeEvent(QCloseEvent *event) {
    if (catalogBusy_) {
        event->ignore();
        const QString message = "正在处理素材库或移入系统回收站，请等待完成后关闭窗口。";
        setStatus(message);
        QToolTip::showText(QCursor::pos(), message, this);
        return;
    }
    QMainWindow::closeEvent(event);
}
void Window::applyAppearance() {
    const bool dark = appearanceModeValue_ == "dark" ||
                      (appearanceModeValue_ == "system" &&
                       QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    const auto &colors = dark ? theme::dark() : theme::light();
    qApp->setProperty("sourceAudioDarkMode", dark);
    QPalette palette;
    palette.setColor(QPalette::Window, colors.window);
    palette.setColor(QPalette::WindowText, colors.text);
    palette.setColor(QPalette::Base, colors.surface);
    palette.setColor(QPalette::AlternateBase, colors.alternate);
    palette.setColor(QPalette::Text, colors.text);
    palette.setColor(QPalette::Button, colors.surface);
    palette.setColor(QPalette::ButtonText, colors.text);
    palette.setColor(QPalette::Highlight, colors.selected);
    palette.setColor(QPalette::HighlightedText, colors.selectedText);
    palette.setColor(QPalette::PlaceholderText, colors.faint);
    palette.setColor(QPalette::ToolTipBase, colors.surface);
    palette.setColor(QPalette::ToolTipText, colors.text);
    palette.setColor(QPalette::Disabled, QPalette::Text, colors.disabledText);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, colors.disabledText);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, colors.disabledText);
    qApp->setPalette(palette);
    qApp->setStyleSheet(appearanceStyle(colors));
    for (auto *button : findChildren<QPushButton *>()) {
        if (button->property("sourceIcon").isValid())
            button->setIcon(ui::icon(static_cast<ui::Icon>(button->property("sourceIcon").toInt()), colors.muted));
    }
    for (auto *action : findChildren<QAction *>()) {
        if (action->property("sourceIcon").isValid())
            action->setIcon(ui::icon(static_cast<ui::Icon>(action->property("sourceIcon").toInt()), colors.muted));
    }
    if (auto *speaker = findChild<QLabel *>("volumeIcon"))
        speaker->setPixmap(ui::icon(ui::Icon::Volume, colors.muted).pixmap(QSize(20, 20), devicePixelRatioF()));
    if (tree_)
        tree_->viewport()->update();
    if (rowWave_)
        rowWave_->clearAppearanceCache();
    if (table_)
        table_->viewport()->update();
}

void Window::buildUi() {
    qApp->setStyle("Fusion");
    QSettings preferences(QDir(dataRoot_).filePath("browser.ini"), QSettings::IniFormat);
    appearanceModeValue_ = preferences.value("appearanceMode", "system").toString();
    if (appearanceModeValue_ != "system" && appearanceModeValue_ != "light" && appearanceModeValue_ != "dark")
        appearanceModeValue_ = "system";
    applyAppearance();
    auto *central = new QWidget;
    central->setObjectName("shell");
    setCentralWidget(central);
    auto *outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *topBar = new QWidget;
    topBar->setObjectName("topBar");
    topBar->setFixedHeight(64);
    auto *top = new QHBoxLayout(topBar);
    top->setContentsMargins(18, 10, 18, 10);
    top->setSpacing(12);
    auto *brandIcon = new QLabel;
    const QPixmap brandPixmap(QStringLiteral(":/brand/icon_32.png"));
    if (!brandPixmap.isNull()) {
        brandIcon->setPixmap(brandPixmap.scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        brandIcon->setFixedSize(28, 28);
        brandIcon->setAccessibleName("源音效管理工具图标");
        top->addWidget(brandIcon);
        top->addSpacing(7);
    } else {
        delete brandIcon;
    }
    auto *heading = new QLabel("源音效管理工具");
    heading->setObjectName("brandTitle");
    heading->setFixedWidth(166);
    top->addWidget(heading);
    heading->setToolTip("源音效管理工具 0.12.2 · 原文件工作流");
    top->addSpacing(4);
    search_ = new QLineEdit;
    search_->setObjectName("musicSearch");
    search_->installEventFilter(this);
    search_->setPlaceholderText("搜索音乐名称、文件夹…");
    search_->setFixedHeight(38);
    auto *searchGlyph = search_->addAction(ui::icon(ui::Icon::Search, theme::current().muted), QLineEdit::LeadingPosition);
    searchGlyph->setProperty("sourceIcon", int(ui::Icon::Search));
    search_->setToolTip(QString("搜索名称与路径；多个关键词用空格分隔。%1 聚焦搜索。")
                            .arg(QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText)));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName("搜索音乐");
    top->addWidget(search_, 1);
    add_ = new QPushButton("添加音乐库");
    add_->setObjectName("libraryAction");
    add_->setProperty("sourceIcon", int(ui::Icon::Plus));
    add_->setAccessibleName("添加音乐库");
    add_->setFixedHeight(36);
    add_->setEnabled(false);
    top->addWidget(add_);
    refresh_ = new GlyphButton("刷新", ui::Icon::Refresh);
    refresh_->setObjectName("iconButton");
    refresh_->setFixedSize(36, 36);
    refresh_->setToolTip("刷新所选素材库");
    top->addWidget(refresh_);
    cancel_ = new QPushButton("停止收录");
    cancel_->hide();
    top->addWidget(cancel_);
    settingsButton_ = new GlyphButton("设置", ui::Icon::Settings);
    settingsButton_->setFixedSize(36, 36);
    settingsButton_->setObjectName("openSettings");
    settingsButton_->setToolTip("播放、波形缓存、性能与显示设置");
    settingsButton_->setAccessibleName("设置");
    top->addWidget(settingsButton_);
    outer->addWidget(topBar);
    auto *splitter = new QSplitter;
    splitter_ = splitter;
    splitter_->setObjectName("librarySplitter");
    splitter_->setChildrenCollapsible(false);
    splitter_->setHandleWidth(5);
    outer->addWidget(splitter, 1);
    auto *left = new QWidget;
    left->setObjectName("sidebar");
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(14, 16, 14, 12);
    leftLayout->setSpacing(9);
    auto *navTitle = new QLabel("素材文件夹");
    navTitle->setObjectName("muted");
    auto *navigation = new QHBoxLayout;
    back_ = new GlyphButton("后退", ui::Icon::ChevronLeft);
    forward_ = new GlyphButton("前进", ui::Icon::ChevronRight);
    back_->setObjectName("navigation");
    forward_->setObjectName("navigation");
    back_->setToolTip("上一个浏览位置");
    forward_->setToolTip("下一个浏览位置");
    back_->setEnabled(false);
    forward_->setEnabled(false);
    back_->setFixedSize(26, 26);
    forward_->setFixedSize(26, 26);
    folderSearch_ = new QLineEdit;
    folderSearch_->setObjectName("folderSearch");
    folderSearch_->setAccessibleName("定位文件夹");
    folderSearch_->setPlaceholderText("定位文件夹");
    folderSearch_->setClearButtonEnabled(true);
    folderSearch_->setFixedHeight(36);
    auto *folderGlyph = folderSearch_->addAction(ui::icon(ui::Icon::Search, theme::current().muted), QLineEdit::LeadingPosition);
    folderGlyph->setProperty("sourceIcon", int(ui::Icon::Search));
    folderSearch_->installEventFilter(this);
    navigation->addWidget(navTitle, 1);
    navigation->addWidget(back_);
    navigation->addWidget(forward_);
    leftLayout->addLayout(navigation);
    leftLayout->addWidget(folderSearch_);
    folderHint_ = new QLabel("按名称查找，在目录树中定位");
    folderHint_->setObjectName("muted");
    folderHint_->setWordWrap(true);
    folderHint_->setVisible(false);
    leftLayout->addWidget(folderHint_);
    folderResults_ = new QListWidget;
    folderResults_->setObjectName("folderMatches");
    folderResults_->setItemDelegate(new FolderMatchDelegate(folderSearch_, folderResults_));
    folderResults_->setMaximumHeight(180);
    folderResults_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    folderResults_->installEventFilter(this);
    folderResults_->hide();
    leftLayout->addWidget(folderResults_);
    folderMoreButton_ = new QPushButton("更多匹配文件夹");
    folderMoreButton_->hide();
    leftLayout->addWidget(folderMoreButton_);
    setupWorkspace(leftLayout);
    workspaceToggle_->setProperty("sourceIcon", int(ui::Icon::Workspace));
    tree_ = new QTreeView;
    tree_->setObjectName("folderTree");
    folderModel_ = new FolderModel(databasePath_, this);
    tree_->setModel(folderModel_);
    tree_->setHeaderHidden(true);
    tree_->setUniformRowHeights(true);
    tree_->setIconSize(QSize(18, 18));
    tree_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    tree_->setExpandsOnDoubleClick(false);
    tree_->setAnimated(false);
    tree_->setIndentation(12);
    tree_->setTextElideMode(Qt::ElideRight);
    tree_->viewport()->installEventFilter(this);
    tree_->installEventFilter(this);
    tree_->setMinimumWidth(180);
    leftLayout->addWidget(tree_, 1);
    count_ = new QLabel("尚未添加音乐目录");
    count_->setObjectName("muted");
    leftLayout->addWidget(count_);
    splitter->addWidget(left);
    auto *content = new QWidget;
    content->setObjectName("content");
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(22, 18, 18, 0);
    layout->setSpacing(8);
    auto *playerBar = new QWidget;
    playerBar->setObjectName("playerBar");
    playerBar->setFixedHeight(68);
    auto *controls = new QHBoxLayout(playerBar);
    controls->setContentsMargins(20, 10, 20, 10);
    controls->setSpacing(10);
    play_ = new GlyphButton("▶ 播放", ui::Icon::Play, GlyphButton::Playback);
    play_->setObjectName("transportButton");
    play_->setFixedSize(40, 40);
    play_->setToolTip("播放 / 暂停 · 空格");
    play_->setEnabled(false);
    controls->addWidget(play_);
    auto *nowPlaying = new QVBoxLayout;
    nowPlaying->setSpacing(2);
    title_ = new ElidedLabel("点击任一波形，定位试听");
    title_->setMinimumWidth(0);
    title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    title_->setObjectName("playerTitle");
    title_->setTextFormat(Qt::PlainText);
    nowPlaying->addWidget(title_);
    detail_ = new ElidedLabel("拖动波形或名称，使用整首原文件");
    detail_->setObjectName("muted");
    detail_->setMinimumWidth(0);
    detail_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    detail_->setTextFormat(Qt::PlainText);
    detail_->setContextMenuPolicy(Qt::CustomContextMenu);
    detail_->setToolTip("选曲后可右键复制原文件路径");
    connect(detail_, &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        if (!selected_.id)
            return;
        QMenu menu(this);
        auto *copy = menu.addAction("复制原文件路径");
        if (menu.exec(detail_->mapToGlobal(point)) == copy)
            QApplication::clipboard()->setText(selected_.absolutePath());
    });
    nowPlaying->addWidget(detail_);
    controls->addLayout(nowPlaying, 3);
    controls->addSpacing(18);
    seek_ = new SeekSlider(Qt::Horizontal);
    seek_->setObjectName("playbackSeek");
    seek_->setAccessibleName("试听进度");
    seek_->setToolTip("定位当前音乐的试听位置");
    seek_->setRange(0, 10000);
    seek_->setTracking(false);
    seek_->setMinimumWidth(100);
    seek_->setEnabled(false);
    controls->addWidget(seek_, 4);
    controls->addSpacing(14);
    time_ = new QLabel("0:00 / 0:00");
    time_->setMinimumWidth(104);
    time_->setObjectName("playerClock");
    time_->setAlignment(Qt::AlignCenter);
    controls->addWidget(time_);
    favorite_ = new GlyphButton("☆ 收藏", ui::Icon::Star, GlyphButton::Favorite);
    favorite_->setObjectName("iconButton");
    favorite_->setFixedSize(34, 34);
    favorite_->setToolTip("收藏 / 取消收藏");
    favorite_->setEnabled(false);
    controls->addWidget(favorite_);
    auto *tagToggle = new QPushButton("标签");
    tagToggle->setObjectName("playerTags");
    tagToggle->setFixedHeight(32);
    tagToggle->setAccessibleName("编辑个人标签");
    tagToggle->setCheckable(true);
    controls->addWidget(tagToggle);
    auto *speaker = new QLabel;
    speaker->setObjectName("volumeIcon");
    speaker->setFixedSize(20, 20);
    speaker->setAccessibleName("音量");
    controls->addWidget(speaker);
    volume_ = new QSlider(Qt::Horizontal);
    volume_->setObjectName("previewVolume");
    volume_->setRange(0, 100);
    volume_->setValue(65);
    volume_->setFixedWidth(80);
    volume_->setAccessibleName("试听音量");
    controls->addWidget(volume_);
    auto *tagPanel = new QWidget;
    tagPanel->setObjectName("tagPanel");
    auto *personal = new QHBoxLayout(tagPanel);
    personal->setContentsMargins(20, 8, 20, 8);
    personal->addWidget(new QLabel("个人标签"));
    tags_ = new QLineEdit;
    tags_->setPlaceholderText("例如：女主 回忆");
    tags_->setEnabled(false);
    personal->addWidget(tags_, 1);
    auto *save = new QPushButton("保存标签");
    personal->addWidget(save);
    tagPanel->hide();
    connect(tagToggle, &QPushButton::toggled, tagPanel, &QWidget::setVisible);
    auto *line = new QHBoxLayout;
    scopeLabel_ = new ElidedLabel("全部音乐");
    scopeLabel_->setObjectName("scopeTitle");
    scopeLabel_->setTextFormat(Qt::PlainText);
    scopeLabel_->setMinimumWidth(0);
    scopeLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    scopeLabel_->setMinimumHeight(32);
    line->addWidget(scopeLabel_, 1);
    subfolders_ = new QCheckBox("包含子文件夹");
    subfolders_->setChecked(true);
    line->addWidget(subfolders_);
    auto *allScope = new QPushButton("全部素材");
    allScope->setObjectName("scopeAction");
    line->addWidget(allScope);
    layout->addLayout(line);
    line = new QHBoxLayout;
    results_ = new QLabel("全部音乐");
    results_->setObjectName("resultCount");
    line->addWidget(results_);
    scopePath_ = new ElidedLabel("所有素材库");
    scopePath_->setObjectName("muted");
    scopePath_->setMinimumWidth(0);
    scopePath_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    scopePath_->setTextFormat(Qt::PlainText);
    line->addSpacing(12);
    line->addWidget(scopePath_, 1);
    autoPlay_ = new PreferenceSwitch;
    autoPlay_->setObjectName("autoPreview");
    autoPlay_->setChecked(false);
    layout->addLayout(line);
    model_ = new TrackModel(this);
    model_->setWaveQueue(waveQueue_);
    table_ = new TrackTable;
    table_->setObjectName("musicTable");
    table_->setMouseTracking(true);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    table_->setModel(model_);
    rowWave_ = new RowWaveDelegate(waveQueue_, table_);
    table_->setItemDelegateForColumn(TrackModel::Wave, rowWave_);
    table_->setItemDelegateForColumn(TrackModel::Name, new TrackNameDelegate(search_, table_));
    table_->setItemDelegateForColumn(TrackModel::Favorite, new FavoriteDelegate(table_));
    table_->setWordWrap(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setAlternatingRowColors(false);
    table_->setShowGrid(false);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(52);
    table_->setDragEnabled(true);
    table_->setDragDropMode(QAbstractItemView::DragOnly);
    table_->setDefaultDropAction(Qt::CopyAction);
    table_->setSortingEnabled(false);
    table_->horizontalHeader()->setStretchLastSection(false);
    table_->horizontalHeader()->setMinimumSectionSize(32);
    table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->setColumnWidth(TrackModel::Favorite, 36);
    table_->horizontalHeader()->setSectionResizeMode(TrackModel::Favorite, QHeaderView::Fixed);
    table_->setColumnWidth(TrackModel::Name, 264);
    table_->setColumnWidth(TrackModel::Duration, 72);
    table_->horizontalHeader()->setSectionResizeMode(TrackModel::Duration, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(TrackModel::Wave, QHeaderView::Stretch);
    table_->setColumnHidden(TrackModel::Folder, true); // shown beneath the filename
    table_->setColumnHidden(TrackModel::Format, true); // visible in the filename extension
    table_->setColumnHidden(TrackModel::Tags, true);   // edited in the compact tag panel
    searchFeedback_ = new QWidget;
    searchFeedback_->setObjectName("searchFeedback");
    auto *feedbackLayout = new QHBoxLayout(searchFeedback_);
    feedbackLayout->setContentsMargins(10, 4, 6, 4);
    searchNotice_ = new ElidedLabel;
    searchNotice_->setTextFormat(Qt::PlainText);
    searchNotice_->setMinimumWidth(0);
    feedbackLayout->addWidget(searchNotice_, 1);
    retrySearch_ = new QPushButton("重试搜索");
    retrySearch_->setObjectName("retrySearch");
    feedbackLayout->addWidget(retrySearch_);
    connect(retrySearch_, &QPushButton::clicked, this, [this] {
        if (indexReady_)
            refreshSearch();
        else
            reloadLibraries();
    });
    layout->addWidget(searchFeedback_);
    resultStack_ = new QStackedWidget;
    resultStack_->addWidget(table_);
    auto *empty = new QWidget;
    auto *emptyLayout = new QVBoxLayout(empty);
    emptyLayout->setSpacing(12);
    emptyLayout->addStretch();
    emptyTitle_ = new QLabel;
    emptyTitle_->setObjectName("emptyTitle");
    emptyTitle_->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle_);
    emptyDetail_ = new QLabel;
    emptyDetail_->setObjectName("emptyDetail");
    emptyDetail_->setWordWrap(true);
    emptyDetail_->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyDetail_);
    emptyAction_ = new QPushButton;
    emptyAction_->setObjectName("emptyAction");
    emptyLayout->addWidget(emptyAction_, 0, Qt::AlignCenter);
    emptyLayout->addStretch();
    resultStack_->addWidget(empty);
    layout->addWidget(resultStack_, 1);
    connect(emptyAction_, &QPushButton::clicked, this, [this, allScope] {
        if (!lastSearchError_.isEmpty())
            retrySearch_->click();
        else if (libraries_.isEmpty())
            addLibrary();
        else if (!search_->text().isEmpty())
            search_->clear();
        else
            allScope->click();
    });
    updateSearchFeedback();
    // Retained as a hidden compatibility hook; music results are one complete list.
    more_ = new QPushButton(content);
    more_->setObjectName("legacyLoadMore");
    more_->hide();
    splitter->addWidget(content);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({236, 1084});
    outer->addWidget(tagPanel);
    outer->addWidget(playerBar);
    status_ = new ElidedLabel;
    status_->setMinimumWidth(0);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addWidget(status_, 1);
    waveStatus_ = new ElidedLabel("波形按需准备");
    waveStatus_->setObjectName("muted");
    waveStatus_->setMaximumWidth(320);
    statusBar()->addPermanentWidget(waveStatus_, 0);
    cacheStatusButton_ = new QPushButton("波形缓存");
    cacheStatusButton_->setObjectName("waveCacheToggle");
    cacheStatusButton_->setAccessibleName("后台波形状态，打开缓存设置");
    statusBar()->addPermanentWidget(cacheStatusButton_);
    buildSettings();
    applyAppearance();
    connect(settingsButton_, &QPushButton::clicked, this, [this] { showSettings(); });
    connect(cacheStatusButton_, &QPushButton::clicked, this, [this] { showSettings(1); });
    auto *preferencesShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma), this);
    connect(preferencesShortcut, &QShortcut::activated, this, [this] { showSettings(); });
    connect(add_, &QPushButton::clicked, this, &Window::addLibrary);
    connect(folderModel_, &FolderModel::error, this, &Window::setStatus);
    connect(folderModel_, &FolderModel::pageLoaded, this, [this] {
        if (pinnedFolder_.isValid() && tree_->currentIndex() == pinnedFolder_)
            tree_->scrollTo(pinnedFolder_, QAbstractItemView::EnsureVisible);
    });
    connect(folderMoreButton_, &QPushButton::clicked, this, [this] { searchFolders(true); });
    connect(folderResults_, &QListWidget::itemClicked, this, [this] { activateFolderMatch(); });
    connect(folderSearch_, &QLineEdit::returnPressed, this, &Window::activateFolderMatch);
    connect(back_, &QPushButton::clicked, this, [this] { navigateHistory(-1); });
    connect(forward_, &QPushButton::clicked, this, [this] { navigateHistory(1); });
    connect(allScope, &QPushButton::clicked, this,
            [this] { tree_->setCurrentIndex(folderModel_->special(false)); });
    connect(subfolders_, &QCheckBox::toggled, this, [this] {
        updateScopeLabel();
        refreshSearch();
    });
    connect(refresh_, &QPushButton::clicked, this, [this] {
        if (!indexReady_) {
            reloadLibraries();
            return;
        }
        if (scan_) {
            setStatus("收录正在进行，可先停止再刷新。");
            return;
        }
        for (const auto &lib : libraries_)
            if (lib.id == scopeLibrary_ || libraries_.size() == 1) {
                beginScan(lib);
                return;
            }
        setStatus("请先在左侧选择要刷新的素材库。");
    });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        if (scan_) {
            scan_->kill();
            setStatus("已停止收录，已收录条目继续可搜索。");
        }
    });
    connect(tree_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &index) {
                if (!navigating_) {
                    *latestLocate_ = ++locateGeneration_;
                    applyScope(index, true);
                }
            });
    connect(more_, &QPushButton::clicked, this, [this] { refreshSearch(true); });
    connect(table_, &TrackTable::waveClicked, this, [this](int row, double ratio) {
        QSignalBlocker blocker(table_->selectionModel());
        table_->setCurrentIndex(model_->index(row, TrackModel::Wave));
        table_->selectRow(row);
        selectTrack(row, ratio);
    });
    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex &i) {
                if (i.isValid())
                    selectTrack(i.row());
            });
    connect(table_, &QTableView::doubleClicked, this, [this](const QModelIndex &i) {
        if (i.isValid())
            sendPlayer({{"op", "play"}});
    });
    connect(table_, &QTableView::clicked, this, [this](const QModelIndex &i) {
        if (i.isValid())
            selectTrack(i.row());
        if (i.isValid() && i.column() == 0) {
            persistPersonal(true);
        }
    });
    connect(favorite_, &QPushButton::clicked, this, [this] {
        if (selected_.id)
            persistPersonal(true);
    });
    connect(save, &QPushButton::clicked, this, [this] { persistPersonal(); });
    connect(tags_, &QLineEdit::returnPressed, this, [this] { persistPersonal(); });
    connect(play_, &QPushButton::clicked, this, [this] {
        if (selected_.id && waveNeedsRetry_) {
            launchWave(selected_, selectionGeneration_);
        }
        if (selected_.id)
            sendPlayer({{"op", isPlaying_ ? "pause" : "play"}});
    });
    connect(seek_, &QSlider::valueChanged, this, [this](int value) {
        if (selected_.id && duration_ > 0 && seek_->isEnabled())
            sendPlayer({{"op", "seek"}, {"position", qMin(duration_ - 1, duration_ * value / 10000)}});
    });
    connect(volume_, &QSlider::valueChanged, this, [this](int v) {
        if (player_)
            sendPlayer({{"op", "volume"}, {"value", v / 100.0}});
    });
    auto *find = new QShortcut(QKeySequence::Find, this);
    connect(find, &QShortcut::activated, this, [this] {
        search_->setFocus();
        search_->selectAll();
    });
    auto *findFolder = new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F), this);
    connect(findFolder, &QShortcut::activated, this, [this] {
        folderSearch_->setFocus();
        folderSearch_->selectAll();
    });
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), table_);
    space->setContext(Qt::WidgetShortcut);
    connect(space, &QShortcut::activated, play_, &QPushButton::click);
    connectWorkspace();
}
void Window::setStatus(const QString &message) {
    status_->setText(message);
    status_->setToolTip(message);
}
void Window::reloadLibraries() {
    struct Loaded {
        QList<Library> libraries;
        QList<QList<FolderRecord>> expanded;
        qint64 count = 0;
        QString error;
    };
    const auto generation = ++librariesGeneration_;
    const auto path = databasePath_, legacy = legacyDatabase_;
    const auto settingsPath = QDir(dataRoot_).filePath("browser.ini");
    const bool restore = !indexReady_;
    auto *watcher = new QFutureWatcher<Loaded>(this);
    connect(watcher, &QFutureWatcher<Loaded>::finished, this, [this, watcher, generation] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != librariesGeneration_)
            return;
        if (!result.error.isEmpty()) {
            setStatus("本机索引未就绪：" + result.error + " · 点击刷新重试");
            lastSearchError_ = result.error;
            loading_ = false;
            updateSearchFeedback();
            return;
        }
        const bool first = !indexReady_;
        indexReady_ = true;
        lastSearchError_.clear();
        add_->setEnabled(true);
        libraries_ = result.libraries;
        refreshSettingsLibraries();
        waveQueue_->setLibraries(libraries_);
        folderModel_->setLibraries(libraries_);
        for (const auto &chain : result.expanded) {
            const auto index = folderModel_->materialize(chain);
            for (auto parent = index; parent.isValid(); parent = parent.parent())
                tree_->expand(parent);
        }
        if (!first)
            folderModel_->refresh();
        count_->setText(QString("%1 首已收录 · %2 个素材库").arg(result.count).arg(libraries_.size()));
        if (first) {
            setStatus("就绪 · 单击波形试听，拖动波形使用整首原文件");
            if (!scopeLibrary_.isEmpty())
                locateFolder(scopeLibrary_, scopeFolder_, false);
            else {
                navigating_ = true;
                tree_->setCurrentIndex(folderModel_->special(favoriteScope_));
                navigating_ = false;
                applyScope(tree_->currentIndex(), false);
            }
            if (!folderSearch_->text().isEmpty())
                searchFolders();
        }
        if (!pendingLibrary_.isEmpty()) {
            const auto pending = pendingLibrary_;
            pendingLibrary_.clear();
            if (pendingLibraryNavigation_ == locateGeneration_)
                locateFolder(pending, "");
        }
        if (!first)
            viewportDebounce_.start(); // Rehydrate visible views after shared-cache reconfiguration.
    });
    watcher->setFuture(QtConcurrent::run(&folderPool_, [path, legacy, settingsPath, restore] {
        Loaded result;
        if (!legacy.isEmpty() && !Index::importSnapshot(legacy, path, &result.error))
            return result;
        Index db(path);
        if (!db.open(&result.error))
            return result;
        result.libraries = db.libraries();
        result.count = db.count();
        if (restore) {
            QSettings settings(settingsPath, QSettings::IniFormat);
            const auto expanded = QJsonDocument::fromJson(settings.value("expanded").toByteArray()).array();
            for (int i = 0; i < qMin(256, int(expanded.size())); ++i) {
                const auto entry = expanded[i].toObject();
                const auto page = db.folderAncestors(entry["library"].toString(), entry["path"].toString());
                if (page.error.isEmpty())
                    result.expanded << page.folders;
            }
        }
        return result;
    }));
}
void Window::recordMeasurement(const QString &kind, qint64 milliseconds) {
    if (measurements_.size() >= 256)
        measurements_.removeAt(0);
    measurements_.append(QJsonObject{{"operation", kind}, {"readyMs", milliseconds}});
}
void Window::searchFolders(bool append) {
    if (!indexReady_ || folderComposing_)
        return;
    const auto query = folderSearch_->text().trimmed();
    const auto generation = ++foldersGeneration_;
    *latestFolderQuery_ = generation;
    if (query.isEmpty()) {
        folderResults_->hide();
        folderMoreButton_->hide();
        return;
    }
    if (!append) {
        folderMatches_.clear();
        folderResults_->clear();
    }
    if (append)
        folderClock_.restart();
    const int offset = folderMatches_.size();
    const auto path = databasePath_;
    folderMoreButton_->setEnabled(false);
    auto *watcher = new QFutureWatcher<FolderPage>(this);
    connect(watcher, &QFutureWatcher<FolderPage>::finished, this, [this, watcher, generation, append] {
        const auto page = watcher->result();
        watcher->deleteLater();
        if (generation != foldersGeneration_)
            return;
        if (!page.error.isEmpty()) {
            folderHint_->setText("目录查询未完成，可重新输入重试");
            setStatus(page.error);
            return;
        }
        for (const auto &folder : page.folders) {
            folderMatches_ << folder;
            auto *item = new QListWidgetItem(folder.name + "\n" + folder.libraryName + " / " + folder.path,
                                             folderResults_);
            item->setToolTip(folder.libraryName + " / " + folder.path);
        }
        folderMore_ = page.more;
        folderMoreButton_->setVisible(page.more);
        folderMoreButton_->setEnabled(page.more);
        folderResults_->setVisible(!folderMatches_.isEmpty());
        if (!append && !folderMatches_.isEmpty())
            folderResults_->setCurrentRow(0);
        const auto elapsed = folderClock_.isValid() ? folderClock_.elapsed() : page.elapsedMs;
        recordMeasurement("folder-candidates", elapsed);
        folderHint_->setText(folderMatches_.isEmpty() ? "未找到文件夹 · 仅查本机已收录目录"
                                                      : QString("%1 个匹配%2 · Enter 定位")
                                                            .arg(folderMatches_.size())
                                                            .arg(page.more ? "，还有更多" : ""));
        folderHint_->setToolTip(
            QString("本机查询 %1 ms · 输入至结果就绪 %2 ms").arg(page.elapsedMs).arg(elapsed));
    });
    watcher->setFuture(
        QtConcurrent::run(&folderPool_, [path, query, offset, generation, latest = latestFolderQuery_] {
            if (latest->load() != generation)
                return FolderPage{};
            Index db(path);
            QString error;
            if (!db.open(&error)) {
                FolderPage p;
                p.error = error;
                return p;
            }
            if (latest->load() != generation)
                return FolderPage{};
            return db.findFolders(query, offset);
        }));
}
void Window::activateFolderMatch() {
    if (folderComposing_)
        return;
    const int row = folderResults_->currentRow();
    if (row < 0 || row >= folderMatches_.size() || !folderResults_->isVisible())
        return;
    const auto match = folderMatches_[row];
    *latestFolderQuery_ = ++foldersGeneration_;
    folderDebounce_.stop();
    folderResults_->hide();
    folderMoreButton_->hide();
    folderHint_->setText("正在定位…");
    locateFolder(match.libraryId, match.path);
}
void Window::locateFolder(const QString &library, const QString &folder, bool clearQuery) {
    if (!indexReady_)
        return;
    const auto generation = ++locateGeneration_;
    *latestLocate_ = generation;
    const auto database = databasePath_;
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    auto *watcher = new QFutureWatcher<FolderPage>(this);
    connect(watcher, &QFutureWatcher<FolderPage>::finished, this,
            [this, watcher, generation, clearQuery, clock] {
                const auto page = watcher->result();
                watcher->deleteLater();
                if (generation != locateGeneration_)
                    return;
                if (!page.error.isEmpty()) {
                    folderHint_->setText("目录暂不可定位");
                    setStatus(page.error);
                    refreshSearch();
                    return;
                }
                const auto index = folderModel_->materialize(page.folders);
                navigating_ = true;
                for (auto parent = index.parent(); parent.isValid(); parent = parent.parent())
                    tree_->expand(parent);
                tree_->setCurrentIndex(index);
                pinnedFolder_ = index;
                tree_->scrollTo(index, QAbstractItemView::PositionAtCenter);
                navigating_ = false;
                applyScope(index, clearQuery);
                folderHint_->setText("已在目录树定位 · 可继续展开浏览");
                recordMeasurement("folder-locate", clock->elapsed());
            });
    watcher->setFuture(
        QtConcurrent::run(&folderPool_, [database, library, folder, generation, latest = latestLocate_] {
            if (latest->load() != generation)
                return FolderPage{};
            Index db(database);
            QString error;
            if (!db.open(&error)) {
                FolderPage p;
                p.error = error;
                return p;
            }
            if (latest->load() != generation)
                return FolderPage{};
            return db.folderAncestors(library, folder);
        }));
}
Window::BrowseState Window::browseState() const {
    return {scopeLibrary_,
            scopeFolder_,
            search_->text(),
            favoriteScope_,
            subfolders_->isChecked(),
            table_->verticalScrollBar()->value()};
}
void Window::saveHistoryPosition() {
    if (historyRestoring_)
        return;
    if (historyAt_ >= 0 && historyAt_ < history_.size()) {
        auto state = browseState();
        if (loading_)
            state.scroll = 0;
        history_[historyAt_] = state;
    }
}
void Window::applyScope(const QModelIndex &index, bool clearQuery) {
    if (!indexReady_ || !index.isValid())
        return;
    const auto library = index.data(FolderModel::LibraryRole).toString();
    const auto folder = index.data(FolderModel::PathRole).toString();
    const auto favorite = index.data(FolderModel::FavoriteRole).toBool();
    const bool changed = library != scopeLibrary_ || folder != scopeFolder_ || favorite != favoriteScope_;
    if (!navigating_ && changed)
        saveHistoryPosition();
    if (changed)
        historyRestoring_ = false;
    scopeLibrary_ = library;
    scopeFolder_ = folder;
    favoriteScope_ = favorite;
    if (clearQuery && changed) {
        QSignalBlocker blocker(search_);
        search_->clear();
        debounce_.stop();
        restoreScroll_ = 0;
    }
    if (!navigating_ && (changed || history_.isEmpty())) {
        while (history_.size() > historyAt_ + 1)
            history_.removeLast();
        history_ << browseState();
        if (history_.size() > 100)
            history_.removeFirst();
        historyAt_ = history_.size() - 1;
    }
    back_->setEnabled(historyAt_ > 0);
    forward_->setEnabled(historyAt_ + 1 < history_.size());
    updateScopeLabel();
    inputClock_.restart();
    refreshSearch();
}
void Window::updateScopeLabel() {
    QString label = favoriteScope_ ? "我的收藏" : "全部音乐";
    QString title = label;
    for (const auto &lib : libraries_)
        if (lib.id == scopeLibrary_) {
            label = lib.name + (scopeFolder_.isEmpty() ? " / 根目录" : " / " + scopeFolder_);
            title = scopeFolder_.isEmpty() ? lib.name : scopeFolder_.section('/', -1);
        }
    if (!scopeLibrary_.isEmpty())
        label += subfolders_->isChecked() ? " · 包含子文件夹" : " · 仅本层";
    scopeLabel_->setText(title);
    scopeLabel_->setToolTip(label);
    scopePath_->setText(label);
    scopePath_->setToolTip(label);
    subfolders_->setEnabled(!scopeLibrary_.isEmpty());
}
void Window::updateSearchFeedback() {
    const bool failed = !lastSearchError_.isEmpty();
    const bool hasRows = model_->rowCount() > 0;
    searchFeedback_->setVisible(failed);
    if (loading_)
        results_->setText(hasRows ? "搜索中 · 暂为上次结果" : "搜索中…");
    searchNotice_->setText(failed ? (hasRows ? "搜索未完成 · 下方保留上次结果" : "搜索未完成")
                                  : "正在搜索 · 下方暂为上次结果");
    searchNotice_->setToolTip(lastSearchError_);
    retrySearch_->setVisible(failed);
    resultStack_->setCurrentIndex(hasRows ? 0 : 1);
    emptyAction_->setVisible(!loading_ && indexReady_);
    if (failed) {
        emptyTitle_->setText("搜索暂时无法完成");
        emptyDetail_->setText("本机索引未能读取，点击重试。错误详情可在上方提示中查看。");
        emptyAction_->setText("重试搜索");
        emptyAction_->show();
    } else if (loading_ || !indexReady_) {
        emptyTitle_->setText(indexReady_ ? "正在搜索音乐" : "正在打开素材库");
        emptyDetail_->setText("从本机索引读取，请稍候…");
    } else if (libraries_.isEmpty()) {
        emptyTitle_->setText("从一个音乐文件夹开始");
        emptyDetail_->setText("添加服务器或本地目录，即可搜索音乐、点击波形试听。");
        emptyAction_->setText("添加音乐目录");
    } else {
        emptyTitle_->setText(favoriteScope_ ? "当前收藏中没有匹配的音乐" : "当前范围没有匹配的音乐");
        emptyDetail_->setText(search_->text().isEmpty() ? "可切换左侧文件夹，或查看全部素材。"
                                                        : "试试更短的名称，或清除关键词后重新查找。");
        emptyAction_->setText(search_->text().isEmpty() ? "查看全部素材" : "清除关键词");
    }
}
void Window::navigateHistory(int direction) {
    const int next = historyAt_ + direction;
    if (next < 0 || next >= history_.size())
        return;
    saveHistoryPosition();
    historyRestoring_ = true;
    debounce_.stop();
    inputPending_ = false;
    *latestQuery_ = ++queryGeneration_;
    loading_ = true;
    const auto state = history_[next];
    historyAt_ = next;
    // Set the saved scope first: the resulting selection is a restore, not a new history entry.
    scopeLibrary_ = state.library;
    scopeFolder_ = state.folder;
    favoriteScope_ = state.favorite;
    QSignalBlocker queryBlock(search_), scopeBlock(subfolders_);
    search_->setText(state.query);
    subfolders_->setChecked(state.recursive);
    restoreScroll_ = state.scroll;
    if (state.library.isEmpty()) {
        *latestLocate_ = ++locateGeneration_;
        navigating_ = true;
        tree_->setCurrentIndex(folderModel_->special(state.favorite));
        applyScope(tree_->currentIndex(), false);
        navigating_ = false;
    } else
        locateFolder(state.library, state.folder, false);
}
bool Window::eventFilter(QObject *object, QEvent *event) {
    if (object == search_ && event->type() == QEvent::InputMethod) {
        const bool composing = !static_cast<QInputMethodEvent *>(event)->preeditString().isEmpty();
        const bool wasComposing = musicComposing_;
        musicComposing_ = composing;
        if (composing) {
            debounce_.stop();
            if (!wasComposing)
                *latestQuery_ = ++queryGeneration_;
        } else if (wasComposing) {
            // A cancelled composition need not emit textChanged; still restore the committed query.
            debounce_.start();
        }
    }
    if (object == folderSearch_ && event->type() == QEvent::InputMethod) {
        const bool wasComposing = folderComposing_;
        folderComposing_ = !static_cast<QInputMethodEvent *>(event)->preeditString().isEmpty();
        if (folderComposing_) {
            folderDebounce_.stop();
            if (!wasComposing)
                *latestFolderQuery_ = ++foldersGeneration_;
        } else if (wasComposing) {
            // Cancelling composition can leave committed text without a textChanged signal.
            folderDebounce_.start();
        }
    }
    if (tree_ && (object == tree_ || object == tree_->viewport()) &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::Wheel ||
         event->type() == QEvent::KeyPress))
        pinnedFolder_ = QPersistentModelIndex();
    if ((object == folderSearch_ || object == folderResults_) && event->type() == QEvent::KeyPress) {
        if (folderComposing_)
            return false;
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            *latestFolderQuery_ = ++foldersGeneration_;
            folderDebounce_.stop();
            folderResults_->hide();
            folderMoreButton_->hide();
            tree_->setFocus();
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            activateFolderMatch();
            return true;
        }
        if ((key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) && folderResults_->count()) {
            folderResults_->show();
            folderResults_->setCurrentRow(
                qBound(0, folderResults_->currentRow() + (key->key() == Qt::Key_Down ? 1 : -1),
                       folderResults_->count() - 1));
            return true;
        }
    }
    return QMainWindow::eventFilter(object, event);
}
QProcess *Window::worker(const QString &kind, const QJsonObject &config,
                         std::function<void(const QJsonObject &)> callback) {
    auto *process = new QProcess(this);
    auto pending = std::make_shared<QByteArray>();
    process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(process, &QProcess::readyReadStandardError, process,
            [process] { process->readAllStandardError(); });
    connect(process, &QProcess::readyReadStandardOutput, this, [process, pending, callback] {
        pending->append(process->readAllStandardOutput());
        if (pending->size() > 4 * 1024 * 1024) {
            pending->clear();
            return;
        }
        qsizetype pos;
        while ((pos = pending->indexOf('\n')) >= 0) {
            auto line = pending->left(pos);
            pending->remove(0, pos + 1);
            const auto doc = QJsonDocument::fromJson(line);
            if (doc.isObject())
                callback(doc.object());
        }
    });
    connect(process, &QProcess::started, this, [process, config] {
        process->write(QJsonDocument(config).toJson(QJsonDocument::Compact) + "\n");
        process->closeWriteChannel();
    });
    process->start(QCoreApplication::applicationFilePath(), {"--worker", kind});
    return process;
}
void Window::addLibrary() {
    if (catalogBusy_) {
        setStatus("正在处理素材库，请等待完成后添加目录。");
        return;
    }
    if (!indexReady_) {
        setStatus("本机索引尚未就绪，请先点击刷新重试。");
        return;
    }
    if (scan_ || registration_) {
        setStatus("正在检查或收录目录，请等待完成，或停止当前收录。");
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle("添加音乐目录");
    dialog.setMinimumWidth(660);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel("选择本机或已挂载的服务器目录。只建立索引，不复制音乐。\n共享缓存可留空；使用时"
                             "请选择专用空文件夹，也可以放在音乐目录内；收录将跳过缓存文件夹。");
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto makePath = [&](const QString &placeholder) {
        auto *row = new QHBoxLayout;
        auto *edit = new QLineEdit;
        edit->setPlaceholderText(placeholder);
        auto *browse = new QPushButton("选择…");
        row->addWidget(edit, 1);
        row->addWidget(browse);
        layout->addLayout(row);
        connect(browse, &QPushButton::clicked, &dialog, [edit, &dialog] {
            auto path = QFileDialog::getExistingDirectory(&dialog, "选择文件夹", edit->text());
            if (!path.isEmpty())
                edit->setText(path);
        });
        return edit;
    };
    auto *root = makePath("音乐根目录（必填）");
    auto *cache = makePath("独立共享缓存目录（可选）");
    auto *permission = new QCheckBox("使用共享缓存的人，对这个音乐库拥有相同的访问权限");
    layout->addWidget(permission);
    auto *hint = new QLabel("Mac 同事选择同一缓存目录，即可复用波形；音乐根目录按各自挂载位置选择。");
    hint->setWordWrap(true);
    hint->setObjectName("muted");
    layout->addWidget(hint);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText("添加并收录");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (root->text().trimmed().isEmpty())
            return;
        if (!cache->text().trimmed().isEmpty() && !permission->isChecked()) {
            QMessageBox::information(&dialog, "共享范围",
                                     "请确认访问范围一致，或清空缓存目录先使用本机缓存。");
            return;
        }
        dialog.accept();
    });
    if (dialog.exec() != QDialog::Accepted)
        return;
    setStatus("正在检查音乐目录和缓存配置…");
    registration_ =
        worker("register", {{"root", root->text().trimmed()}, {"cache", cache->text().trimmed()}},
               [this](const QJsonObject &m) {
                   if (m["event"] == "error") {
                       setStatus(m["message"].toString());
                       return;
                   }
                   if (m["event"] == "registered") {
                       if (!indexReady_) {
                           setStatus("本机索引尚未就绪，未写入新素材库。");
                           return;
                       }
                       Library lib{m["id"].toString(), m["name"].toString(), m["root"].toString(),
                                   m["cache"].toString(), m["sharedId"].toString()};
                       Index db(databasePath_);
                       if (!db.open()) {
                           setStatus("无法保存库配置：" + db.lastError());
                           return;
                       }
                       for (const auto &existing : db.libraries()) {
                           if (QDir::cleanPath(existing.root) == QDir::cleanPath(lib.root)) {
                               if (existing.sharedCache != lib.sharedCache) {
                                   setStatus("这个音乐目录已经添加；请在共享设置中接入或更换缓存目录。");
                                   return;
                               }
                               lib.id = existing.id;
                               lib.sharedId = existing.sharedId;
                           }
                       }
                       if (!db.saveLibrary(lib)) {
                           setStatus(db.lastError());
                           return;
                       }
                       pendingLibrary_ = lib.id;
                       pendingLibraryNavigation_ = locateGeneration_;
                       reloadLibraries();
                       registration_ = nullptr;
                       beginScan(lib);
                   }
               });
    auto *registration = registration_;
    auto finishRegistration = [this, registration] {
        if (registration_ == registration)
            registration_ = nullptr;
        registration->deleteLater();
    };
    connect(registration, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            finishRegistration);
    connect(registration, &QProcess::errorOccurred, this,
            [this, finishRegistration](QProcess::ProcessError e) {
                if (e == QProcess::FailedToStart) {
                    setStatus("目录检查进程无法启动，请检查软件文件是否完整。");
                    finishRegistration();
                }
            });
    QTimer::singleShot(15000, registration, [this, registration] {
        if (registration->state() != QProcess::NotRunning) {
            registration->kill();
            setStatus("目录检查超时，请确认共享已挂载后再试。");
        }
    });
}
void Window::beginScan(const Library &lib) {
    if (catalogBusy_) {
        setStatus("正在处理素材库，请等待完成后刷新。");
        return;
    }
    if (registration_) {
        setStatus("共享目录正在检查，完成后即可刷新音乐目录。");
        return;
    }
    if (scan_) {
        setStatus("已有目录正在收录，请等待完成。");
        return;
    }
    scanControlPath_ =
        QDir(dataRoot_).filePath("scan-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".json");
    scanControlState_.clear();
    if (!writeScanControl()) {
        scanControlPath_.clear();
        return;
    }
    cancel_->show();
    refresh_->setEnabled(false);
    setStatus("正在收录：" + lib.name + " · 可继续搜索和试听");
    auto scanCompleted = std::make_shared<bool>(false);
    auto scanChanged = std::make_shared<bool>(false);
    scan_ = worker(
        "scan",
        {{"root", lib.root}, {"library", lib.id}, {"database", databasePath_}, {"control", scanControlPath_}},
        [this, scanCompleted, scanChanged](const QJsonObject &m) {
            if (m["event"] == "deferred")
                setStatus(m["paused"].toBool() ? "收录正在让路，试听结束或电脑空闲后自动继续。"
                                               : "正在继续收录目录…");
            if (m["event"] == "error")
                setStatus("收录未完成：" + m["message"].toString() + "；旧目录保留。");
            if (m["event"] == "progress") {
                if (m["filesChanged"].toBool() && !*scanChanged) {
                    *scanChanged = true;
                    // Earlier committed pages survive a later scan failure/cancellation.
                    // Retain cached views and historical counts, but retire old queued versions.
                    waveQueue_->refreshIndex();
                }
                setStatus(
                    QString("已检查 %1 首 · %2").arg(m["count"].toInteger()).arg(m["folder"].toString()));
                if (!loading_)
                    refreshSearch();
            }
            if (m["event"] == "complete") {
                *scanCompleted = true;
                setStatus(QString("收录完成：%1 首 · 原文件保持原位").arg(m["count"].toInteger()));
            }
        });
    auto *process = scan_;
    updateWorkPolicy();
    auto finishScan = [this, process, scanCompleted, scanChanged] {
        if (scan_ == process) {
            scan_ = nullptr;
            QFile::remove(scanControlPath_);
            scanControlPath_.clear();
            scanControlState_.clear();
            updateWorkPolicy();
            cancel_->hide();
            refresh_->setEnabled(true);
            if (*scanChanged ||
                (*scanCompleted && process->exitStatus() == QProcess::NormalExit && process->exitCode() == 0))
                waveQueue_->refreshIndex();
            reloadLibraries();
            refreshSearch();
        }
        process->deleteLater();
    };
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, finishScan);
    connect(process, &QProcess::errorOccurred, this, [this, finishScan](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            setStatus("收录进程无法启动，请检查软件文件是否完整。");
            finishScan();
        }
    });
}
void Window::selectTrack(int row, std::optional<double> ratio) {
    if (row < 0 || row >= model_->tracks.size())
        return;
    selectTrackValue(model_->tracks[row], ratio);
}
void Window::selectTrackValue(const Track &track, std::optional<double> ratio) {
    if (catalogBusy_)
        return;
    if (isUnavailableOriginal(track)) {
        setStatus("原文件已移入回收站；请在该项目上右键选择“仅从列表排除”。");
        return;
    }
    ++workspaceRequest_;
    if (track.id == selected_.id && track.size == selected_.size && track.modified == selected_.modified &&
        !waveNeedsRetry_) {
        if (ratio) {
            waveQueue_->select(track, true);
            sendPlayer({{"op", "cue"}, {"ratio", *ratio}});
        }
        return;
    }
    selected_ = track;
    const auto generation = ++selectionGeneration_;
    position_ = duration_ = 0;
    {
        QSignalBlocker blocker(seek_);
        seek_->setSliderDown(false);
        seek_->setValue(0);
        seek_->setEnabled(false);
    }
    isPlaying_ = false;
    title_->setText(track.name);
    title_->setToolTip(track.name);
    detail_->setText(track.absolutePath());
    detail_->setToolTip(track.absolutePath());
    const auto personalKey = QString::number(track.id) + ':' + WaveQueue::key(track);
    const bool personalPending = personalWritesInFlight_.contains(personalKey);
    tags_->setText(personalPending ? personalWritesInFlight_.value(personalKey) : track.tags);
    tags_->setEnabled(!personalPending);
    favorite_->setEnabled(!personalPending);
    favorite_->setText(track.favorite ? "★ 已收藏" : "☆ 收藏");
    play_->setEnabled(true);
    play_->setText("▶ 播放");
    time_->setText("0:00 / 0:00");
    waveStatus_->setText("正在准备波形，可先试听");
    QJsonObject load{{"op", "load"},
                     {"path", track.absolutePath()},
                     {"generation", qint64(generation)},
                     {"autoplay", ratio.has_value() || autoPlay_->isChecked()},
                     {"volume", volume_->value() / 100.0}};
    if (ratio)
        load["startRatio"] = *ratio;
    sendPlayer(load);
    launchWave(track, generation);
}
void Window::launchWave(const Track &track, quint64) {
    waveNeedsRetry_ = false;
    rowWave_->playingKey = WaveQueue::key(track);
    rowWave_->position = rowWave_->duration = 0;
    waveQueue_->select(track, true);
    updateWave(track);
    table_->viewport()->update();
}
void Window::updateVisibleWaves() {
    if (catalogBusy_) {
        waveQueue_->setVisible({});
        return;
    }
    if (loading_ || debounce_.isActive())
        return;
    QList<Track> visible;
    const auto view = table_->viewport()->rect();
    // rowAt is constant-time; never enqueue the entire search result list.
    int first = table_->rowAt(0);
    if (first < 0)
        first = 0;
    for (int row = first; row < model_->tracks.size(); ++row) {
        const auto rect = table_->visualRect(model_->index(row, TrackModel::Wave));
        if (rect.top() > view.bottom())
            break;
        if (rect.intersects(view))
            visible << model_->tracks[row];
    }
    waveQueue_->setVisible(visible);
    updateWorkPolicy();
}
void Window::updateWave(const Track &track) {
    const auto *entry = waveQueue_->find(track);
    if (!entry)
        return;
    const auto d = entry->data.sampleRate > 0 ? entry->data.frames * 1000 / entry->data.sampleRate : 0;
    const int row = model_->rowForTrack(track);
    const bool isSelected = sameTrackVersion(track, selected_);
    if (entry->complete && !entry->approximate) {
        // A verified waveform takes priority over a queued player metadata duration.
        // Both writes use the same one-thread pool, so an already in-flight player write
        // finishes before this verified duration is stored.
        const bool pendingPlayerWrite = pendingDurationWrites_.remove(WaveQueue::key(track)) > 0;
        const bool displayedDurationDiffers = row >= 0 && model_->tracks[row].duration != d;
        const bool selectedDurationDiffers = isSelected && selected_.duration > 0 && selected_.duration != d;
        if (track.duration != d || pendingPlayerWrite || displayedDurationDiffers || selectedDurationDiffers)
            metadataPool_.start([path = databasePath_, track, d] {
                Index db(path);
                if (db.open())
                    db.setDuration(track, d);
            });
    }
    if (row >= 0) {
        if (entry->complete && !entry->approximate)
            model_->tracks[row].duration = d;
        emit model_->dataChanged(model_->index(row, TrackModel::Wave),
                                 model_->index(row, TrackModel::Duration));
    }
    if (!isSelected)
        return;
    if (entry->complete && !entry->approximate)
        selected_.duration = d;
    waveNeedsRetry_ = entry->failed;
    if (entry->complete && duration_ == 0 && !entry->approximate)
        duration_ = d;
    waveStatus_->setText(entry->message);
    waveStatus_->setToolTip(entry->message);
}
void Window::queueDurationWrite(const Track &version, qint64 milliseconds) {
    pendingDurationWrites_.insert(WaveQueue::key(version), {version, milliseconds});
    if (!durationWriteRetry_.isActive())
        durationWriteRetry_.start(0);
}
void Window::flushDurationWrite() {
    if (durationWriteInFlight_ || pendingDurationWrites_.isEmpty())
        return;
    const auto key = pendingDurationWrites_.constBegin().key();
    const auto update = pendingDurationWrites_.value(key);
    durationWriteInFlight_ = true;
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, key, update] {
        const bool saved = watcher->result();
        watcher->deleteLater();
        durationWriteInFlight_ = false;
        const auto current = pendingDurationWrites_.constFind(key);
        if (saved && current != pendingDurationWrites_.cend() && current->milliseconds == update.milliseconds)
            pendingDurationWrites_.remove(key);
        if (!pendingDurationWrites_.isEmpty())
            durationWriteRetry_.start(saved ? 0 : 2000);
    });
    watcher->setFuture(QtConcurrent::run(&metadataPool_, [path = databasePath_, update] {
        Index db(path);
        return db.open() && db.setDuration(update.version, update.milliseconds);
    }));
}
void Window::sendPlayer(QJsonObject command) {
    const auto op = command["op"].toString();
    if (op == "load" || op == "cue" || op == "play") {
        playerLoading_ = true;
        playerResponse_.restart();
        playerWatchdog_.start();
        updateWorkPolicy();
    }
    if (!command.contains("generation"))
        command["generation"] = qint64(selectionGeneration_);
    auto bytes = QJsonDocument(command).toJson(QJsonDocument::Compact) + "\n";
    if (player_ && player_->state() == QProcess::Running) {
        player_->write(bytes);
        return;
    }
    if (!player_ && command["op"] != "load" && selected_.id) {
        const QJsonObject reload{{"op", "load"},
                                 {"path", selected_.absolutePath()},
                                 {"generation", qint64(selectionGeneration_)},
                                 {"autoplay", false},
                                 {"volume", volume_->value() / 100.0}};
        playerPending_ = QJsonDocument(reload).toJson(QJsonDocument::Compact) + "\n";
    }
    playerPending_.append(bytes);
    if (player_)
        return;
    player_ = new QProcess(this);
    auto pending = std::make_shared<QByteArray>();
    connect(player_, &QProcess::started, this, [this] {
        playerResponse_.start();
        player_->write(playerPending_);
        playerPending_.clear();
    });
    connect(player_, &QProcess::readyReadStandardError, this, [this] { player_->readAllStandardError(); });
    connect(player_, &QProcess::readyReadStandardOutput, this, [this, pending] {
        playerResponse_.restart();
        pending->append(player_->readAllStandardOutput());
        if (pending->size() > 1024 * 1024)
            pending->clear();
        qsizetype pos;
        while ((pos = pending->indexOf('\n')) >= 0) {
            auto line = pending->left(pos);
            pending->remove(0, pos + 1);
            const auto doc = QJsonDocument::fromJson(line);
            if (doc.isObject())
                handlePlayer(doc.object());
        }
    });
    auto *process = player_;
    auto finishPlayer = [this, process] {
        if (player_ == process) {
            player_ = nullptr;
            isPlaying_ = false;
            playerLoading_ = false;
            playerWatchdog_.stop();
            updateWorkPolicy();
            playerPending_.clear();
            play_->setText("▶ 播放");
            if (selected_.id)
                setStatus("试听进程已退出，点击播放可重试。");
        }
        process->deleteLater();
    };
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, finishPlayer);
    connect(process, &QProcess::errorOccurred, this, [this, finishPlayer](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finishPlayer();
            setStatus("试听进程无法启动，请检查软件文件是否完整。");
        }
    });
    player_->start(QCoreApplication::applicationFilePath(), {"--worker", "player"});
}
void Window::handlePlayer(const QJsonObject &m) {
    if (quint64(m["generation"].toInteger()) != selectionGeneration_)
        return;
    if (m["event"] == "position") {
        playerLoading_ = false;
        position_ = m["position"].toInteger();
        const qint64 playerDuration = m["duration"].toInteger();
        if (playerDuration > 0) {
            duration_ = playerDuration;
            const auto *wave = waveQueue_->find(selected_);
            const bool exactWave = wave && wave->complete && !wave->approximate;
            if (selected_.id && !exactWave && selected_.duration != playerDuration) {
                const Track version = selected_;
                selected_.duration = playerDuration;
                queueDurationWrite(version, playerDuration);
            }
        }
        rowWave_->position = position_;
        rowWave_->duration = duration_;
        const int row = model_->rowForTrack(selected_);
        if (row >= 0) {
            const auto rect = table_->visualRect(model_->index(row, TrackModel::Wave));
            if (rect.intersects(table_->viewport()->rect()))
                table_->viewport()->update(rect);
            const auto *wave = waveQueue_->find(selected_);
            const bool exactWave = wave && wave->complete && !wave->approximate;
            if (playerDuration > 0 && !exactWave && model_->tracks[row].duration != playerDuration) {
                model_->tracks[row].duration = playerDuration;
                emit model_->dataChanged(model_->index(row, TrackModel::Duration),
                                         model_->index(row, TrackModel::Duration));
            }
        }
        seek_->setEnabled(duration_ > 0 && !catalogBusy_);
        if (!seek_->isSliderDown()) {
            QSignalBlocker blocker(seek_);
            seek_->setValue(duration_ > 0 ? int(qBound(0.0, double(position_) / duration_, 1.0) * 10000) : 0);
        }
        const auto timeText = clockText(position_) + " / " + clockText(duration_);
        if (time_->text() != timeText)
            time_->setText(timeText);
    } else if (m["event"] == "state") {
        isPlaying_ = m["playing"].toBool();
        if (isPlaying_) {
            playerLoading_ = false;
            playerWatchdog_.start();
            if (workspace_ && lastAuditionGeneration_ != selectionGeneration_) {
                workspace_->recordAudition(selected_);
                lastAuditionGeneration_ = selectionGeneration_;
            }
        } else if (!playerLoading_) {
            playerWatchdog_.stop();
        }
        play_->setText(isPlaying_ ? "Ⅱ 暂停" : "▶ 播放");
        updateWorkPolicy();
    } else if (m["event"] == "ready") {
        playerLoading_ = false;
        if (!isPlaying_)
            playerWatchdog_.stop();
        updateWorkPolicy();
    } else if (m["event"] == "error") {
        isPlaying_ = false;
        playerLoading_ = false;
        playerWatchdog_.stop();
        updateWorkPolicy();
        play_->setText("▶ 播放");
        setStatus("试听失败：" + m["message"].toString() + " · 请确认原文件可读");
    }
}
bool Window::writeScanControl() {
    if (scanControlPath_.isEmpty())
        return true;
    const bool priority = editingPriority_ && editingPriority_->isChecked();
    const QJsonObject policy{{"paused", priority && (isPlaying_ || playerLoading_ || pressureGate_.busy())},
                             {"entriesPerSecond", priority ? 400 : 0}};
    const auto data = QJsonDocument(policy).toJson(QJsonDocument::Compact);
    if (scanControlState_ == data)
        return true;
    QSaveFile file(scanControlPath_);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        setStatus("后台让路设置未能保存：" + file.errorString());
        return false;
    }
    scanControlState_ = data;
    return true;
}
void Window::updateWorkPolicy() {
    if (!editingPriority_)
        return;
    const bool priority = editingPriority_->isChecked();
    const bool work = scan_ || waveQueue_->hasBackgroundWork();
    if (priority && work) {
        if (!loadTimer_.isActive()) {
            loadSampler_.reset();
            loadSampler_.sample();
            loadTimer_.start();
        }
    } else {
        loadTimer_.stop();
        loadSampler_.reset();
        pressureGate_.reset();
    }
    waveQueue_->setWorkContext(priority && (isPlaying_ || playerLoading_), priority && scan_,
                               priority && pressureGate_.busy());
    writeScanControl();
}
void Window::persistPersonal(bool toggleFavorite) {
    if (!selected_.id || catalogBusy_)
        return;
    const Track version = selected_;
    const auto key = QString::number(version.id) + ':' + WaveQueue::key(version);
    if (personalWritesInFlight_.contains(key))
        return;
    const bool favorite = toggleFavorite ? !version.favorite : version.favorite;
    const QString tags = tags_->text().left(4096);
    const quint64 selection = selectionGeneration_;
    personalWritesInFlight_.insert(key, tags);
    favorite_->setEnabled(false);
    tags_->setEnabled(false);
    setStatus("正在保存个人标记…");
    using Result = QPair<bool, QString>;
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this,
            [this, watcher, key, version, favorite, tags, selection] {
                const auto result = watcher->result();
                watcher->deleteLater();
                personalWritesInFlight_.remove(key);
                const bool stillSelected = sameTrackVersion(selected_, version);
                if (stillSelected) {
                    favorite_->setEnabled(true);
                    tags_->setEnabled(true);
                }
                if (!result.first) {
                    // Retain the saved state and the user's editable tag draft for a retry.
                    if (stillSelected && selectionGeneration_ == selection)
                        setStatus("个人标记保存失败：" + result.second + " · 可再次点击保存");
                    return;
                }
                if (stillSelected) {
                    selected_.favorite = favorite;
                    selected_.tags = tags;
                    tags_->setText(tags);
                    favorite_->setText(favorite ? "★ 已收藏" : "☆ 收藏");
                }
                const int row = model_->rowForTrack(version);
                if (row >= 0) {
                    model_->tracks[row].favorite = favorite;
                    model_->tracks[row].tags = tags;
                    emit model_->dataChanged(model_->index(row, 0), model_->index(row, TrackModel::Tags));
                }
                // Invalidate an in-flight snapshot that may still contain old personal
                // fields, and recompute membership for favorite/tag-filtered results.
                // refreshSearch retains selection without triggering another audition.
                if (loading_ || favoriteScope_ || !search_->text().trimmed().isEmpty())
                    refreshSearch();
                if (stillSelected && selectionGeneration_ == selection &&
                    status_->text() == "正在保存个人标记…")
                    setStatus("个人标记已保存在本机，原音乐没有改动。");
            });
    watcher->setFuture(QtConcurrent::run(&metadataPool_, [path = databasePath_, version, favorite, tags] {
        Index db(path);
        const bool saved = db.open() && db.setPersonal(version, favorite, tags);
        return Result{saved, saved ? QString() : db.lastError()};
    }));
}
