#include "workspace_panel.h"
#include "media_drag.h"
#include <QComboBox>
#include <QCursor>
#include <QDrag>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolTip>
#include <QVBoxLayout>

void WorkspaceTrackList::setTrackRows(const QList<Track> &tracks) {
    clear();
    tracks_ = tracks;
    setDragEnabled(true);
    for (const auto &track : tracks_) {
        auto *item = new QListWidgetItem(track.name + "\n" + track.folder, this);
        item->setToolTip(track.name + "\n" + track.absolutePath() + "\n双击试听 · 拖出整首音频");
        item->setSizeHint(QSize(0, 40));
    }
}
void WorkspaceTrackList::setFolderRows(const QList<PinnedFolder> &folders) {
    clear();
    tracks_.clear();
    setDragEnabled(false);
    for (const auto &folder : folders) {
        auto *item = new QListWidgetItem(
            folder.name + "\n" + (folder.path.isEmpty() ? QString("根目录") : folder.path), this);
        item->setToolTip(folder.name + "\n" + folder.root + (folder.path.isEmpty() ? "" : "/" + folder.path) +
                         "\n双击定位左侧目录树");
        item->setSizeHint(QSize(0, 40));
    }
}
QMimeData *WorkspaceTrackList::originalMimeData() const {
    auto *mime = new QMimeData;
    QList<Track> selected;
    for (const auto *item : selectedItems()) {
        const auto index = row(item);
        if (index >= 0 && index < tracks_.size())
            selected << tracks_[index];
    }
    if (!selected.isEmpty())
        mime->setUrls(mediaFileUrls(selected, catalogPath_));
    return mime;
}
void WorkspaceTrackList::startDrag(Qt::DropActions) {
    QList<Track> selected;
    for (const auto *item : selectedItems()) {
        const auto index = row(item);
        if (index >= 0 && index < tracks_.size())
            selected.append(tracks_[index]);
    }
    QString error;
    const auto urls = prepareMediaDragUrls(this, selected, catalogPath_, &error);
    if (urls.isEmpty()) {
        if (!error.isEmpty())
            QToolTip::showText(QCursor::pos(), error, this);
        return;
    }
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->exec(Qt::CopyAction, Qt::CopyAction);
    drag->deleteLater();
}
void WorkspacePanel::setCatalogPath(const QString &path) {
    rows_->setCatalogPath(path);
}

WorkspacePanel::WorkspacePanel(const QString &databasePath, QWidget *parent)
    : QWidget(parent), store_(databasePath) {
    setObjectName("workspacePanel");
    setMinimumWidth(180);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    mode_ = new QComboBox;
    mode_->setObjectName("workspaceMode");
    mode_->addItem("最近试听", Recent);
    mode_->addItem("常用目录", Folders);
    mode_->setAccessibleName("个人工作区视图");
    layout->addWidget(mode_);
    rows_ = new WorkspaceTrackList;
    rows_->setObjectName("workspaceRows");
    rows_->setMinimumWidth(0);
    rows_->setMinimumHeight(64);
    rows_->setMaximumHeight(220);
    rows_->setUniformItemSizes(true);
    rows_->setTextElideMode(Qt::ElideMiddle);
    rows_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rows_->setSelectionMode(QAbstractItemView::SingleSelection);
    rows_->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(rows_);
    notice_ = new QLabel;
    notice_->setObjectName("workspaceNotice");
    notice_->setTextFormat(Qt::PlainText);
    notice_->setWordWrap(false);
    layout->addWidget(notice_);
    connect(mode_, &QComboBox::currentIndexChanged, this, &WorkspacePanel::refresh);
    connect(rows_, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) { activateRow(rows_->row(item)); });
    connect(rows_, &QWidget::customContextMenuRequested, this, &WorkspacePanel::contextMenu);
    if (!store_.open()) {
        notice_->setWordWrap(true);
        notice_->setText("个人工作区暂不可用");
        notice_->setToolTip(store_.lastError());
        mode_->setEnabled(false);
    } else {
        refresh();
    }
}
WorkspaceStore &WorkspacePanel::store() {
    return store_;
}
bool WorkspacePanel::report(bool success) {
    if (success)
        return true;
    notice_->setWordWrap(true);
    notice_->setText("操作未完成，请重试");
    notice_->setToolTip(store_.lastError());
    emit warning(store_.lastError());
    return false;
}
void WorkspacePanel::setMode(Mode mode) {
    const auto at = mode_->findData(mode);
    mode_->setCurrentIndex(at >= 0 ? at : 0);
}
bool WorkspacePanel::recordAudition(const Track &track) {
    if (!report(store_.recordAudition(track)))
        return false;
    if (mode_->currentData().toInt() == Recent)
        refresh();
    return true;
}
bool WorkspacePanel::pinFolder(const QString &libraryId, const QString &path, const QString &name,
                               const QString &root) {
    if (!report(store_.pinFolder({libraryId, path, name, root})))
        return false;
    if (mode_->currentData().toInt() == Folders)
        refresh();
    return true;
}
void WorkspacePanel::refresh() {
    const int scroll = rows_->verticalScrollBar()->value();
    const auto previous = rows_->currentRow();
    const auto previousTrack = previous >= 0 && previous < tracks_.size() ? tracks_[previous] : Track{};
    const auto previousFolder =
        previous >= 0 && previous < folders_.size() ? folders_[previous] : PinnedFolder{};
    const int mode = mode_->currentData().toInt();
    QList<Track> tracks;
    QList<PinnedFolder> folders;
    if (mode == Folders)
        folders = store_.pinnedFolders();
    else
        tracks = store_.recents();
    if (!store_.lastError().isEmpty()) {
        report(false);
        return;
    }
    tracks_ = tracks;
    folders_ = folders;
    QSignalBlocker block(rows_);
    if (mode == Folders) {
        rows_->setFolderRows(folders_);
        for (int i = 0; i < folders_.size(); ++i)
            if (folders_[i].libraryId == previousFolder.libraryId && folders_[i].path == previousFolder.path)
                rows_->setCurrentRow(i);
    } else {
        rows_->setTrackRows(tracks_);
        for (int i = 0; i < tracks_.size(); ++i)
            if (tracks_[i].libraryId == previousTrack.libraryId &&
                tracks_[i].relativePath == previousTrack.relativePath)
                rows_->setCurrentRow(i);
    }
    rows_->verticalScrollBar()->setValue(scroll);
    notice_->setWordWrap(false);
    if (rows_->count()) {
        notice_->setText(mode == Folders ? QString("%1 个目录 · 双击定位").arg(rows_->count())
                                         : QString("%1 首 · 双击试听 / 拖出").arg(rows_->count()));
        notice_->setToolTip(mode == Folders ? "双击定位左侧文件夹；移除只影响常用目录记录。"
                                            : "双击试听；拖动整首音频到剪辑软件，音乐原件保持原位。");
    } else {
        notice_->setText(mode == Folders ? "右键文件夹加入常用目录" : "最近试听 · 最多 100 首");
        notice_->setToolTip(mode == Folders ? "在左侧目录上右键，加入常用目录。"
                                           : "试听过的音乐会出现在这里，最多 100 首。");
    }
}
void WorkspacePanel::activateRow(int row) {
    if (mode_->currentData().toInt() == Folders) {
        if (row >= 0 && row < folders_.size()) {
            const auto folder = folders_[row];
            emit folderActivated(folder.libraryId, folder.path);
        }
    } else if (row >= 0 && row < tracks_.size()) {
        const auto track = tracks_[row];
        emit trackActivated(track);
    }
}
void WorkspacePanel::contextMenu(const QPoint &point) {
    const auto *item = rows_->itemAt(point);
    if (!item)
        return;
    const int row = rows_->row(item);
    const int mode = mode_->currentData().toInt();
    const auto track = row < tracks_.size() ? tracks_[row] : Track{};
    const auto folder = row < folders_.size() ? folders_[row] : PinnedFolder{};
    QMenu menu(this);
    auto *activate = menu.addAction(mode == Folders ? "定位文件夹" : "试听");
#ifdef Q_OS_MACOS
    auto *reveal = mode == Recent ? menu.addAction("在 Finder 中显示") : nullptr;
#else
    auto *reveal = mode == Recent ? menu.addAction("在文件管理器中显示") : nullptr;
#endif
    auto *remove = menu.addAction(mode == Folders ? "从常用目录移除" : "移除这条试听记录");
    const auto chosen = menu.exec(rows_->viewport()->mapToGlobal(point));
    if (chosen == activate) {
        if (mode == Folders)
            emit folderActivated(folder.libraryId, folder.path);
        else
            emit trackActivated(track);
    } else if (reveal && chosen == reveal) {
        emit revealTrackRequested(track);
    } else if (chosen == remove) {
        bool success = false;
        if (mode == Folders) {
            success = store_.unpinFolder(folder.libraryId, folder.path);
        } else {
            success = store_.removeRecent(track.libraryId, track.relativePath);
        }
        if (report(success))
            refresh();
    }
}
