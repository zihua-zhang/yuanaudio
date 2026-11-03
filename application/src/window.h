#pragma once
#include "background_policy.h"
#include "cache.h"
#include "folder_model.h"
#include "index.h"
#include "wave_queue.h"
#include "wave_renderer.h"
#include "workspace_panel.h"
#include <QAbstractTableModel>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QThreadPool>
#include <QTimer>
#include <atomic>
#include <memory>
#include <optional>
class QLineEdit;
class QLabel;
class QPushButton;
class QTreeView;
class QListWidget;
class QCheckBox;
class QComboBox;
class QSlider;
class QSplitter;
class QStackedWidget;
class QVBoxLayout;
class QHelpEvent;
class QDialog;
class SourceRepairCoordinator;

class TrackModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    using QAbstractTableModel::QAbstractTableModel;
    enum Column { Favorite, Name, Wave, Folder, Duration, Format, Tags, ColumnCount };
    QList<Track> tracks;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override;
    QStringList mimeTypes() const override;
    Qt::DropActions supportedDragActions() const override;
    void setCatalogPath(const QString &path) { catalogPath_ = path; }
    QString catalogPath() const { return catalogPath_; }
    void replace(QList<Track> values);
    void append(QList<Track> values);
    void setWaveQueue(const WaveQueue *queue) {
        waveQueue_ = queue;
    }
    int rowForId(qint64 id) const;
    int rowForTrack(const Track &track) const;

  private:
    const WaveQueue *waveQueue_ = nullptr; // Display-only duration hints for visible cached waves.
    QHash<qint64, int> rowsById_;
    QString catalogPath_;
};
class TrackTable : public QTableView {
    Q_OBJECT
  public:
    using QTableView::QTableView;
  signals:
    void waveClicked(int row, double ratio);
    void viewportChanged();

  protected:
    virtual void executeFileDrag(QMimeData *mime, const QString &label);
    void startDrag(Qt::DropActions) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

  private:
    bool waveGesture_ = false;
    QPersistentModelIndex pressedWave_;
    QPoint pressedPoint_;
};
class RowWaveDelegate : public QStyledItemDelegate {
  public:
    RowWaveDelegate(WaveQueue *queue, QObject *parent);
    void paint(QPainter *, const QStyleOptionViewItem &, const QModelIndex &) const override;
    bool helpEvent(QHelpEvent *, QAbstractItemView *, const QStyleOptionViewItem &,
                   const QModelIndex &) override;
    void clearAppearanceCache() { renderer_.clear(); }
    QString playingKey;
    qint64 position = 0, duration = 0;

  private:
    WaveQueue *queue_;
    mutable WaveRenderer renderer_;
};
class Window : public QMainWindow {
    Q_OBJECT
    friend class WindowTests;
    friend class QueueRefreshTests;
    friend class WindowUxTests;
    friend class Window08Tests;
    friend class WindowPolicyUiTests;
    friend class FailureUiTests;
    friend class FullListTests;
    friend class SettingsUiTests;

  public:
    explicit Window(const QString &dataRoot, const QString &legacyDatabase = {},
                    const QString &peakRoot = {});
    ~Window() override;

  private:
    QString dataRoot_, databasePath_, scopeLibrary_, scopeFolder_;
    QString lastSearchError_;
    bool favoriteScope_ = false, isPlaying_ = false, hasMore_ = false, loading_ = false;
    bool waveNeedsRetry_ = false;
    bool playerLoading_ = false;
    quint64 lastAuditionGeneration_ = 0, workspaceRequest_ = 0;
    WorkspacePanel *workspace_ = nullptr;
    QPushButton *workspaceToggle_ = nullptr;
    QCheckBox *editingPriority_ = nullptr;
    QTimer loadTimer_;
    SystemLoadSampler loadSampler_;
    PressureGate pressureGate_;
    QString scanControlPath_;
    QByteArray scanControlState_;
    QPointer<QDialog> failureDialog_;
    QDialog *settingsDialog_ = nullptr;
    QListWidget *settingsNavigation_ = nullptr;
    QStackedWidget *settingsPages_ = nullptr;
    QPushButton *settingsButton_ = nullptr, *cacheStatusButton_ = nullptr;
    QComboBox *preparationSpeed_ = nullptr, *folderIndentation_ = nullptr, *listDensity_ = nullptr;
    QComboBox *appearanceMode_ = nullptr;
    QString appearanceModeValue_ = "system";
    QLabel *sharedCacheSummary_ = nullptr, *librarySummary_ = nullptr;
    QListWidget *libraryList_ = nullptr;
    QPushButton *refreshLibraryButton_ = nullptr, *removeLibraryButton_ = nullptr;
    QPushButton *libraryMoreButton_ = nullptr;
    QListWidget *excludedList_ = nullptr;
    QLineEdit *excludedFilter_ = nullptr;
    QPushButton *restoreExcludedButton_ = nullptr;
    QLabel *excludedSummary_ = nullptr;
    QList<CatalogExcludedItem> excludedItems_;
    quint64 libraryDetailsGeneration_ = 0;
    bool catalogBusy_ = false;
    QThreadPool catalogPool_;
    enum class RemovalKind { Library, Folder, Track };
    struct MovedOriginal {
        RemovalKind kind;
        QString libraryId, relativePath;
    };
    QList<MovedOriginal> movedOriginalsPending_;
    bool indexReady_ = false, navigating_ = false, historyRestoring_ = false;
    QString legacyDatabase_;
    QString pendingLibrary_;
    quint64 pendingLibraryNavigation_ = 0;
    QPersistentModelIndex pinnedFolder_;
    std::shared_ptr<std::atomic<quint64>> latestFolderQuery_ = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<std::atomic<quint64>> latestLocate_ = std::make_shared<std::atomic<quint64>>(0);
    struct BrowseState {
        QString library, folder, query;
        bool favorite = false, recursive = true;
        int scroll = 0;
    };
    QList<BrowseState> history_;
    int historyAt_ = -1, restoreScroll_ = -1;
    quint64 foldersGeneration_ = 0, locateGeneration_ = 0, librariesGeneration_ = 0;
    QList<FolderRecord> folderMatches_;
    bool folderMore_ = false;
    QThreadPool folderPool_;
    QElapsedTimer inputClock_, folderClock_;
    QJsonArray measurements_;
    bool inputPending_ = false;
    bool folderComposing_ = false;
    bool musicComposing_ = false;
    int pageOffset_ = 0;
    quint64 queryGeneration_ = 0, selectionGeneration_ = 0;
    Track selected_;
    QList<Library> libraries_;
    QLineEdit *search_, *tags_;
    QLabel *results_, *title_, *detail_, *time_, *status_, *waveStatus_, *count_;
    QTreeView *tree_ = nullptr;
    FolderModel *folderModel_;
    QLineEdit *folderSearch_;
    QListWidget *folderResults_ = nullptr;
    QLabel *folderHint_, *scopeLabel_;
    QLabel *scopePath_, *searchNotice_, *emptyTitle_, *emptyDetail_;
    QWidget *searchFeedback_;
    QPushButton *retrySearch_, *emptyAction_;
    QStackedWidget *resultStack_;
    QSplitter *splitter_;
    QPushButton *folderMoreButton_, *back_, *forward_;
    QPushButton *add_;
    QCheckBox *subfolders_;
    QTimer folderDebounce_;
    TrackTable *table_ = nullptr;
    TrackModel *model_;
    WaveQueue *waveQueue_;
    SourceRepairCoordinator *sourceRepairCoordinator_ = nullptr;
    QString automaticSourceRepairLibrary_;
    bool playerSourceRepairReload_ = false;
    RowWaveDelegate *rowWave_ = nullptr;
    QPushButton *play_, *favorite_, *more_, *refresh_, *cancel_;
    QCheckBox *autoPlay_;
    QSlider *volume_;
    QSlider *seek_ = nullptr;
    QTimer debounce_;
    QTimer viewportDebounce_;
    QTimer playerWatchdog_;
    QElapsedTimer playerResponse_;
    QThreadPool searchPool_;
    QThreadPool metadataPool_;
    std::shared_ptr<std::atomic<quint64>> latestQuery_ = std::make_shared<std::atomic<quint64>>(0);
    QProcess *registration_ = nullptr, *scan_ = nullptr, *player_ = nullptr;
    QByteArray playerPending_;
    qint64 position_ = 0, duration_ = 0, indexGeneration_ = -1;
    struct DurationWrite {
        Track version;
        qint64 milliseconds = 0;
    };
    QHash<QString, DurationWrite> pendingDurationWrites_;
    QTimer durationWriteRetry_;
    bool durationWriteInFlight_ = false;
    QHash<QString, QString> personalWritesInFlight_; // Track version -> tag draft being saved.
    void queueDurationWrite(const Track &version, qint64 milliseconds);
    void flushDurationWrite();
    void buildUi();
    void applyAppearance();
    void buildSettings();
    void showSettings(int page = 0);
    void refreshSettingsLibraries();
    void loadLibraryDetails();
    void filterExcludedItems();
    void restoreExcludedSelection();
    void confirmCatalogRemoval(RemovalKind kind, const QString &libraryId,
                               const QString &relativePath = {}, bool moveOriginal = false);
    void requestCatalogRemoval(RemovalKind kind, const QString &libraryId,
                               const QString &relativePath = {}, bool moveOriginal = false);
    void clearSelectedTrack();
    void setCatalogBusy(bool busy);
    bool trackMatchesRemoval(const Track &track, RemovalKind kind, const QString &libraryId,
                             const QString &relativePath) const;
    bool isUnavailableOriginal(const Track &track) const;
    void setupWorkspace(QVBoxLayout *left);
    void connectWorkspace();
    void activateWorkspaceTrack(const Track &track);
    void revealOriginalFile(const Track &track);
    void selectTrackValue(const Track &track, std::optional<double> ratio = std::nullopt);
    void configureSharedCache(const QString &libraryId = {});
    void showWaveFailures();
    void updateWorkPolicy();
    bool writeScanControl();
    void reloadLibraries();
    void searchFolders(bool append = false);
    void locateFolder(const QString &library, const QString &folder, bool clearQuery = true);
    void activateFolderMatch();
    void applyScope(const QModelIndex &index, bool clearQuery);
    void updateScopeLabel();
    void updateSearchFeedback();
    BrowseState browseState() const;
    void saveHistoryPosition();
    void navigateHistory(int direction);
    void recordMeasurement(const QString &kind, qint64 milliseconds);
    bool eventFilter(QObject *object, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void refreshSearch(bool append = false);
    void showNasPathRepair(bool authorizeOnly = false);
    void setupAutomaticSourceRepair();
    void addLibrary();
    void beginScan(const Library &library);
    void selectTrack(int row, std::optional<double> ratio = std::nullopt);
    void launchWave(const Track &track, quint64 generation);
    void updateWave(const Track &track);
    void updateVisibleWaves();
    void sendPlayer(QJsonObject command);
    void handlePlayer(const QJsonObject &message);
    void persistPersonal(bool toggleFavorite = false);
    void setStatus(const QString &message);
    QProcess *worker(const QString &kind, const QJsonObject &config,
                     std::function<void(const QJsonObject &)> onMessage);
};
