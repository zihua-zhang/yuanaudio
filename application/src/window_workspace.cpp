#include "window.h"
#include "source_file_removal.h"
#include "workspace_panel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDesktopServices>
#include <QFileInfo>
#include <QUrl>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSignalBlocker>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>

namespace {
struct CatalogRemovalResult {
    bool success = false;
    bool movedOriginal = false;
    QString error;
};
} // namespace

void Window::confirmCatalogRemoval(RemovalKind kind, const QString &libraryId,
                                   const QString &relativePath, bool moveOriginal) {
    if (catalogBusy_)
        return;
    const auto found = std::find_if(libraries_.cbegin(), libraries_.cend(),
                                    [&libraryId](const Library &library) { return library.id == libraryId; });
    if (found == libraries_.cend()) {
        setStatus("素材库已变化，请刷新后重试。");
        return;
    }
    const Library library = *found;
    const QString target = kind == RemovalKind::Library ? library.name : relativePath;
    if (moveOriginal && kind == RemovalKind::Library) {
        QInputDialog confirmation(this);
        confirmation.setWindowTitle("确认移动整个原目录");
        confirmation.setInputMode(QInputDialog::TextInput);
        confirmation.setLabelText(
            QString("将“%1”的整个原目录移入系统回收站，会影响所有使用服务器音乐的同事；共享目录可能不支持回收站。\n"
                    "请输入素材库名称“%1”以继续：").arg(library.name));
        confirmation.setOkButtonText("继续移入回收站");
        confirmation.setCancelButtonText("取消");
        if (confirmation.exec() != QDialog::Accepted)
            return;
        if (confirmation.textValue() != library.name) {
            setStatus("名称不一致，原目录和素材库列表均未改变。");
            return;
        }
        const auto current = std::find_if(libraries_.cbegin(), libraries_.cend(),
                                          [&libraryId](const Library &entry) { return entry.id == libraryId; });
        if (current == libraries_.cend() ||
            QDir::cleanPath(current->root) != QDir::cleanPath(library.root)) {
            setStatus("素材库目录已变化，请刷新后重试。");
            return;
        }
        requestCatalogRemoval(kind, libraryId, relativePath, true);
        return;
    }
    QMessageBox confirmation(this);
    confirmation.setIcon(moveOriginal ? QMessageBox::Warning : QMessageBox::Question);
    confirmation.setWindowTitle(moveOriginal ? "移动原文件到回收站" : "仅从列表排除");
    confirmation.setText(QString("%1“%2”？")
                             .arg(moveOriginal ? "要移动原文件并从列表移除" : "要仅从这台电脑的列表排除", target));
    confirmation.setInformativeText(
        moveOriginal
            ? "这会影响所有使用该音乐共享目录的同事。软件只尝试移入系统回收站；若共享目录不支持，"
              "原文件和列表都不会改变。"
            : "音乐原文件及共享波形缓存不会删除。排除规则会在下次收录时继续生效；"
              "之后可在“设置 → 素材库”恢复到列表。");
    auto *proceed = confirmation.addButton(moveOriginal ? "移入回收站并移除" : "仅从列表排除",
                                           QMessageBox::AcceptRole);
    auto *cancel = confirmation.addButton("取消", QMessageBox::RejectRole);
    confirmation.setDefaultButton(moveOriginal ? qobject_cast<QPushButton *>(cancel)
                                               : qobject_cast<QPushButton *>(proceed));
    confirmation.setEscapeButton(cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != proceed)
        return;
    const auto current = std::find_if(libraries_.cbegin(), libraries_.cend(),
                                      [&libraryId](const Library &entry) { return entry.id == libraryId; });
    if (current == libraries_.cend() || QDir::cleanPath(current->root) != QDir::cleanPath(library.root)) {
        setStatus("素材库目录已变化，请刷新后重试。");
        return;
    }
    requestCatalogRemoval(kind, libraryId, relativePath, moveOriginal);
}

void Window::clearSelectedTrack() {
    ++selectionGeneration_;
    ++workspaceRequest_;
    if (player_ && player_->state() != QProcess::NotRunning)
        player_->kill();
    playerPending_.clear();
    playerLoading_ = false;
    playerWatchdog_.stop();
    selected_ = {};
    isPlaying_ = waveNeedsRetry_ = false;
    position_ = duration_ = 0;
    {
        QSignalBlocker blocker(seek_);
        seek_->setSliderDown(false);
        seek_->setValue(0);
        seek_->setEnabled(false);
    }
    rowWave_->playingKey.clear();
    rowWave_->position = rowWave_->duration = 0;
    title_->setText("点击任一波形，定位试听");
    title_->setToolTip({});
    detail_->setText("拖动波形或名称，使用整首原文件");
    detail_->setToolTip("选曲后可右键复制原文件路径");
    tags_->clear();
    tags_->setEnabled(false);
    favorite_->setEnabled(false);
    favorite_->setText("☆ 收藏");
    play_->setEnabled(false);
    play_->setText("▶ 播放");
    time_->setText("0:00 / 0:00");
    waveStatus_->setText("波形按需准备");
    updateWorkPolicy();
}

bool Window::trackMatchesRemoval(const Track &track, RemovalKind kind, const QString &libraryId,
                                 const QString &relativePath) const {
    return track.id && track.libraryId == libraryId &&
           (kind == RemovalKind::Library ||
            (kind == RemovalKind::Track && track.relativePath == relativePath) ||
            (kind == RemovalKind::Folder && track.relativePath.startsWith(relativePath + '/')));
}

bool Window::isUnavailableOriginal(const Track &track) const {
    for (const auto &moved : movedOriginalsPending_)
        if (trackMatchesRemoval(track, moved.kind, moved.libraryId, moved.relativePath))
            return true;
    return false;
}

void Window::setCatalogBusy(bool busy) {
    catalogBusy_ = busy;
    table_->setEnabled(!busy);
    tree_->setEnabled(!busy);
    search_->setEnabled(!busy);
    folderSearch_->setEnabled(!busy);
    workspace_->setEnabled(!busy);
    workspaceToggle_->setEnabled(!busy);
    folderMoreButton_->setEnabled(!busy);
    play_->setEnabled(!busy && selected_.id);
    seek_->setEnabled(!busy && selected_.id && duration_ > 0);
    add_->setEnabled(!busy && indexReady_);
    refresh_->setEnabled(!busy && indexReady_ && !scan_);
    subfolders_->setEnabled(!busy && !scopeLibrary_.isEmpty());
    refreshSettingsLibraries();
}

void Window::requestCatalogRemoval(RemovalKind kind, const QString &libraryId,
                                   const QString &relativePath, bool moveOriginal) {
    if (catalogBusy_ || !indexReady_)
        return;
    if (registration_) {
        setStatus("正在检查目录或共享缓存，请完成后再管理素材库。");
        return;
    }
    if (scan_) {
        auto *process = scan_;
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, kind, libraryId, relativePath, moveOriginal] {
                    QTimer::singleShot(0, this, [this, kind, libraryId, relativePath, moveOriginal] {
                        requestCatalogRemoval(kind, libraryId, relativePath, moveOriginal);
                    });
                }, Qt::SingleShotConnection);
        process->kill();
        setStatus("正在停止收录，随后处理素材库变更…");
        return;
    }
    const auto found = std::find_if(libraries_.cbegin(), libraries_.cend(),
                                    [&libraryId](const Library &library) { return library.id == libraryId; });
    if (found == libraries_.cend() || (kind != RemovalKind::Library && relativePath.isEmpty())) {
        setStatus("素材库项目已变化，请刷新后重试。");
        return;
    }
    const bool selectedAffected = trackMatchesRemoval(selected_, kind, libraryId, relativePath);
    if (moveOriginal && selectedAffected && player_ && player_->state() != QProcess::NotRunning) {
        player_->kill();
        QTimer::singleShot(150, this, [this, kind, libraryId, relativePath, moveOriginal] {
            requestCatalogRemoval(kind, libraryId, relativePath, moveOriginal);
        });
        setStatus("正在停止试听，随后移动原文件…");
        return;
    }
    setCatalogBusy(true);
    setStatus(moveOriginal ? "正在移入系统回收站；完成后更新本机列表…" : "正在更新本机素材库列表…");
    if (moveOriginal)
        waveQueue_->refreshIndex(); // Release decode handles before moving an original.
    const auto library = *found;
    const auto allLibraries = libraries_;
    const auto database = databasePath_;
    auto *watcher = new QFutureWatcher<CatalogRemovalResult>(this);
    connect(watcher, &QFutureWatcher<CatalogRemovalResult>::finished, this,
            [this, watcher, kind, libraryId, relativePath, moveOriginal] {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (!result.success) {
                    if (result.movedOriginal) {
                        movedOriginalsPending_.append({kind, libraryId, relativePath});
                        if (trackMatchesRemoval(selected_, kind, libraryId, relativePath))
                            clearSelectedTrack();
                    }
                    setCatalogBusy(false);
                    viewportDebounce_.start();
                    refreshSearch();
                    setStatus(result.movedOriginal
                                  ? "原件已移入回收站；请在列表上右键选择“仅从列表排除”。列表更新失败：" +
                                        result.error
                                  : "未完成移除：" + result.error);
                    return;
                }
                movedOriginalsPending_.removeIf([this, kind, &libraryId, &relativePath](const MovedOriginal &moved) {
                    if (moved.libraryId != libraryId)
                        return false;
                    return kind == RemovalKind::Library ||
                           (kind == RemovalKind::Folder &&
                            (moved.relativePath == relativePath ||
                             moved.relativePath.startsWith(relativePath + '/'))) ||
                           (kind == RemovalKind::Track && moved.kind == RemovalKind::Track &&
                            moved.relativePath == relativePath);
                });
                const bool currentSelectionAffected =
                    trackMatchesRemoval(selected_, kind, libraryId, relativePath);
                if (currentSelectionAffected)
                    clearSelectedTrack();
                const bool scopeGone = scopeLibrary_ == libraryId &&
                                       (kind == RemovalKind::Library ||
                                        (kind == RemovalKind::Folder &&
                                         (scopeFolder_ == relativePath ||
                                          scopeFolder_.startsWith(relativePath + '/'))));
                navigating_ = true;
                if (kind == RemovalKind::Library) {
                    libraries_.removeIf([&libraryId](const Library &lib) { return lib.id == libraryId; });
                    folderModel_->setLibraries(libraries_);
                } else if (kind == RemovalKind::Folder) {
                    folderModel_->removeFolderSubtree(libraryId, relativePath);
                }
                if (scopeGone) {
                    scopeLibrary_.clear();
                    scopeFolder_.clear();
                    favoriteScope_ = false;
                    QSignalBlocker blocker(search_);
                    search_->clear();
                    tree_->setCurrentIndex(folderModel_->special(false));
                    restoreScroll_ = 0;
                    updateScopeLabel();
                }
                navigating_ = false;
                if (kind != RemovalKind::Track) {
                    history_.clear();
                    history_ << browseState();
                    historyAt_ = 0;
                    back_->setEnabled(false);
                    forward_->setEnabled(false);
                }
                *latestLocate_ = ++locateGeneration_;
                waveQueue_->refreshIndex();
                reloadLibraries();
                setCatalogBusy(false);
                refreshSearch();
                if (!folderSearch_->text().trimmed().isEmpty())
                    searchFolders();
                else {
                    folderResults_->clear();
                    folderResults_->hide();
                }
                setStatus(moveOriginal ? "原文件已移入系统回收站，列表已更新。"
                                       : kind == RemovalKind::Library
                                           ? "素材库已从这台电脑断开；原目录未删除。"
                                           : "已从列表排除；原文件保留，可在设置中恢复。");
            });
    watcher->setFuture(QtConcurrent::run(
        &catalogPool_, [database, library, allLibraries, kind, relativePath, moveOriginal] {
            CatalogRemovalResult result;
            Index db(database);
            if (!db.open(&result.error))
                return result;
            const auto catalogLibraries = db.libraries();
            const auto current = std::find_if(catalogLibraries.cbegin(), catalogLibraries.cend(),
                                              [&library](const Library &candidate) {
                                                  return candidate.id == library.id;
                                              });
            if (current == catalogLibraries.cend() ||
                QDir::cleanPath(current->root) != QDir::cleanPath(library.root)) {
                result.error = "素材库目录已变化，原文件和列表均未改变；请刷新后重试。";
                return result;
            }
            if (moveOriginal) {
                if (kind == RemovalKind::Library)
                    result.movedOriginal = moveLibraryRootToTrash(library, allLibraries, &result.error);
                else
                    result.movedOriginal = moveOriginalToTrash(
                        library, relativePath, kind == RemovalKind::Folder, allLibraries, &result.error);
                if (!result.movedOriginal)
                    return result;
            }
            if (kind == RemovalKind::Library)
                result.success = db.removeLibrary(library.id, &result.error);
            else if (kind == RemovalKind::Folder)
                result.success = db.excludeFolder(library.id, relativePath, &result.error);
            else
                result.success = db.excludeTrack(library.id, relativePath, &result.error);
            if (!result.success && result.error.isEmpty())
                result.error = db.lastError();
            return result;
        }));
}

void Window::setupWorkspace(QVBoxLayout *left) {
    workspaceToggle_ = new QPushButton("个人工作区");
    workspaceToggle_->setObjectName("workspaceToggle");
    workspaceToggle_->setCheckable(true);
    workspaceToggle_->setToolTip("最近试听与常用目录 · 只保存在本机");
    left->addWidget(workspaceToggle_);
    workspace_ = new WorkspacePanel(QDir(dataRoot_).filePath("workspace.sqlite"), this);
    workspace_->setCatalogPath(databasePath_);
    workspace_->setMaximumHeight(230);
    left->addWidget(workspace_);
    const auto settingsPath = QDir(dataRoot_).filePath("workspace-ui.ini");
    QSettings settings(settingsPath, QSettings::IniFormat);
    workspace_->setMode(settings.value("mode", 1).toInt() == WorkspacePanel::Folders
                            ? WorkspacePanel::Folders : WorkspacePanel::Recent);
    const bool expanded = settings.value("expanded", false).toBool();
    workspaceToggle_->setChecked(expanded);
    workspace_->setVisible(expanded);
    connect(workspaceToggle_, &QPushButton::toggled, this, [this, settingsPath](bool visible) {
        workspace_->setVisible(visible);
        QSettings preferences(settingsPath, QSettings::IniFormat);
        preferences.setValue("expanded", visible);
    });
    auto *mode = workspace_->findChild<QComboBox *>("workspaceMode");
    connect(mode, &QComboBox::currentIndexChanged, this, [settingsPath, mode] {
        QSettings preferences(settingsPath, QSettings::IniFormat);
        preferences.setValue("mode", mode->currentData());
    });
}

void Window::connectWorkspace() {
    connect(workspace_, &WorkspacePanel::warning, this, &Window::setStatus);
    connect(workspace_, &WorkspacePanel::revealTrackRequested, this, &Window::revealOriginalFile);
    connect(workspace_, &WorkspacePanel::trackActivated, this, &Window::activateWorkspaceTrack);
    connect(workspace_, &WorkspacePanel::folderActivated, this,
            [this](const QString &library, const QString &path) { locateFolder(library, path); });
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto at = table_->indexAt(point);
        if (!at.isValid() || at.row() >= model_->tracks.size())
            return;
        const auto track = model_->tracks[at.row()];
        QMenu menu(this);
#ifdef Q_OS_MACOS
        auto *reveal = menu.addAction("在 Finder 中显示");
#else
        auto *reveal = menu.addAction("在文件管理器中显示");
#endif
        auto *locate = menu.addAction("在左侧定位文件夹");
        menu.addSeparator();
        auto *exclude = menu.addAction("仅从列表排除（保留原文件）…");
        auto *trash = menu.addAction("移入系统回收站并从列表移除…");
        exclude->setEnabled(!catalogBusy_);
        trash->setEnabled(!catalogBusy_);
        menu.setDefaultAction(reveal);
        const auto chosen = menu.exec(table_->viewport()->mapToGlobal(point));
        if (!chosen)
            return;
        if (chosen == reveal) {
            revealOriginalFile(track);
        } else if (chosen == locate) {
            locateFolder(track.libraryId, track.folder == "." ? QString("") : track.folder);
        } else if (chosen == exclude) {
            confirmCatalogRemoval(RemovalKind::Track, track.libraryId, track.relativePath);
        } else if (chosen == trash) {
            confirmCatalogRemoval(RemovalKind::Track, track.libraryId, track.relativePath, true);
        }
    });
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto at = tree_->indexAt(point);
        if (!at.isValid())
            return;
        const auto libraryId = at.data(FolderModel::LibraryRole).toString();
        if (libraryId.isEmpty())
            return; // All-music and favorites are virtual scopes, not pinnable folders.
        const auto path = at.data(FolderModel::PathRole).toString();
        const auto name = at.data(Qt::DisplayRole).toString();
        QString root;
        for (const auto &library : libraries_)
            if (library.id == libraryId)
                root = library.root;
        QMenu menu(this);
        auto *pin = path.isEmpty() ? nullptr : menu.addAction("加入常用目录");
        auto *shared = menu.addAction("设置共享波形缓存…");
        menu.addSeparator();
        auto *exclude = menu.addAction(path.isEmpty() ? "从素材库列表移除…"
                                                    : "仅从列表排除此文件夹（保留原文件）…");
        auto *trash = menu.addAction(path.isEmpty() ? "将整个原目录移入系统回收站并从列表移除…"
                                                     : "移入系统回收站并从列表移除…");
        exclude->setEnabled(!catalogBusy_);
        trash->setEnabled(!catalogBusy_);
        menu.setDefaultAction(exclude);
        const auto chosen = menu.exec(tree_->viewport()->mapToGlobal(point));
        if (!chosen)
            return;
        if (chosen == pin && workspace_->pinFolder(libraryId, path, name, root)) {
            workspace_->setMode(WorkspacePanel::Folders);
            workspaceToggle_->setChecked(true);
            setStatus("已加入常用目录 · 双击可在目录树中定位");
        } else if (chosen == shared) {
            configureSharedCache(libraryId);
        } else if (chosen == exclude) {
            confirmCatalogRemoval(path.isEmpty() ? RemovalKind::Library : RemovalKind::Folder,
                                  libraryId, path);
        } else if (chosen == trash) {
            confirmCatalogRemoval(path.isEmpty() ? RemovalKind::Library : RemovalKind::Folder,
                                  libraryId, path, true);
        }
    });
}

void Window::activateWorkspaceTrack(const Track &snapshot) {
    if (!indexReady_ || catalogBusy_)
        return;
    const auto request = ++workspaceRequest_;
    const auto selection = selectionGeneration_;
    struct Lookup {
        std::optional<Track> track;
        QString error;
    };
    auto *watcher = new QFutureWatcher<Lookup>(this);
    connect(watcher, &QFutureWatcher<Lookup>::finished, this, [this, watcher, request, selection] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (workspaceRequest_ != request || selectionGeneration_ != selection)
            return;
        if (!result.error.isEmpty()) {
            setStatus("个人工作区暂时无法定位音乐：" + result.error);
        } else if (!result.track) {
            setStatus("这首音乐已不在当前索引中；个人引用保留，请刷新对应素材库后再试。");
        } else {
            selectTrackValue(*result.track, 0.0);
        }
    });
    watcher->setFuture(QtConcurrent::run(&searchPool_, [path = databasePath_, snapshot] {
        Lookup result;
        Index db(path);
        if (!db.open(&result.error))
            return result;
        result.track = db.trackByIdentity(snapshot.libraryId, snapshot.relativePath);
        result.error = db.lastError();
        return result;
    }));
}

void Window::revealOriginalFile(const Track &snapshot) {
    // Resolve current catalog identity, never a waveform/local compatibility cache.
    // File-system checks stay off the GUI thread for disconnected network volumes.
    struct Location { QString path, error; };
    auto *watcher = new QFutureWatcher<Location>(this);
    connect(watcher, &QFutureWatcher<Location>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (!result.error.isEmpty()) {
            setStatus(result.error);
            return;
        }
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        auto *process = new QProcess(this);
        connect(process, &QProcess::errorOccurred, this, [this, process] {
            setStatus("无法打开文件管理器：" + process->errorString());
            process->deleteLater();
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, process](int code, QProcess::ExitStatus exit) {
            setStatus(code == 0 && exit == QProcess::NormalExit
                          ? "已请求文件管理器定位原文件"
                          : "无法定位原文件，请检查文件是否存在以及目录访问权限。");
            process->deleteLater();
        });
        QTimer::singleShot(15000, process, [process] {
            if (process->state() != QProcess::NotRunning) process->kill();
        });
#ifdef Q_OS_MACOS
        process->start("/usr/bin/open", {"-R", result.path});
#else
        process->start("explorer.exe", {"/select,", QDir::toNativeSeparators(result.path)});
#endif
#else
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(result.path).absolutePath())))
            setStatus("无法打开原文件所在目录。");
#endif
    });
    watcher->setFuture(QtConcurrent::run(&searchPool_, [database = databasePath_, snapshot] {
        Location result;
        Index db(database);
        if (!db.open(&result.error)) return result;
        const auto track = db.trackByIdentity(snapshot.libraryId, snapshot.relativePath);
        if (!track) {
            result.error = "这首音乐已不在当前索引中，无法定位原文件。";
            return result;
        }
        result.path = track->absolutePath();
        if (!QDir::isAbsolutePath(result.path) || !QFileInfo(result.path).isFile())
            result.error = "原文件不存在或暂时不可访问，请检查文件位置或网络挂载：" + result.path;
        return result;
    }));
}
