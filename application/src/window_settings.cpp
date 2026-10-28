#include "preference_switch.h"
#include "window.h"
#include <QBoxLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFrame>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSlider>
#include <QStackedWidget>
#include <QTreeView>
#include <QtConcurrent>
#include <algorithm>

namespace {
QLabel *textLabel(const QString &text, const char *name = nullptr) {
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    if (name)
        label->setObjectName(name);
    return label;
}
QVBoxLayout *group(QVBoxLayout *page, const QString &title) {
    page->addWidget(textLabel(title, "settingsSectionTitle"));
    auto *frame = new QFrame;
    frame->setObjectName("settingsGroup");
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(18, 15, 18, 15);
    layout->setSpacing(14);
    page->addWidget(frame);
    return layout;
}
void setting(QVBoxLayout *layout, const QString &title, const QString &detail, QWidget *control) {
    auto *row = new QHBoxLayout;
    row->setSpacing(24);
    auto *labels = new QVBoxLayout;
    labels->setSpacing(5);
    labels->addWidget(textLabel(title, "settingTitle"));
    if (!detail.isEmpty())
        labels->addWidget(textLabel(detail, "settingDetail"));
    row->addLayout(labels, 1);
    row->addWidget(control, 0, Qt::AlignVCenter);
    layout->addLayout(row);
}
QString elapsedText(qint64 milliseconds) {
    const auto seconds = qMax(qint64(0), milliseconds / 1000);
    return QString("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QChar('0'));
}
} // namespace

void Window::buildSettings() {
    settingsDialog_ = new QDialog(this);
    settingsDialog_->setObjectName("settingsDialog");
    settingsDialog_->setWindowTitle("设置 · 源音效管理工具");
    settingsDialog_->setModal(false);
    settingsDialog_->resize(880, 640);
    settingsDialog_->setMinimumSize(760, 520);
    auto *outer = new QVBoxLayout(settingsDialog_);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto *body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    auto *sidebar = new QWidget;
    sidebar->setObjectName("settingsSidebar");
    sidebar->setFixedWidth(174);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(14, 24, 14, 14);
    side->setSpacing(18);
    side->addWidget(textLabel("设置", "settingsTitle"));
    settingsNavigation_ = new QListWidget;
    settingsNavigation_->setObjectName("settingsNavigation");
    settingsNavigation_->setAccessibleName("设置分类");
    settingsNavigation_->addItems({"播放与浏览", "波形缓存", "性能", "显示", "素材库", "快捷键"});
    settingsNavigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (int row = 0; row < settingsNavigation_->count(); ++row)
        settingsNavigation_->item(row)->setSizeHint(QSize(140, 42));
    side->addWidget(settingsNavigation_, 1);
    side->addWidget(textLabel("源音效管理工具\n0.12.5", "settingDetail"));
    body->addWidget(sidebar);
    settingsPages_ = new QStackedWidget;
    settingsPages_->setObjectName("settingsPages");
    body->addWidget(settingsPages_, 1);
    outer->addLayout(body, 1);
    auto *footer = new QWidget;
    footer->setObjectName("settingsFooter");
    auto *footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(20, 10, 20, 10);
    footerRow->addWidget(textLabel("设置即时生效", "settingDetail"), 1);
    auto *close = new QPushButton("完成");
    close->setObjectName("closeSettings");
    close->setDefault(true);
    footerRow->addWidget(close);
    outer->addWidget(footer);
    connect(close, &QPushButton::clicked, settingsDialog_, &QDialog::close);
    connect(settingsNavigation_, &QListWidget::currentRowChanged, settingsPages_,
            &QStackedWidget::setCurrentIndex);

    auto page = [this](const QString &title, const QString &description, const char *name) {
        auto *scroll = new QScrollArea;
        scroll->setObjectName("settingsScroll");
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *content = new QWidget;
        content->setObjectName(name);
        auto *layout = new QVBoxLayout(content);
        layout->setContentsMargins(28, 26, 28, 24);
        layout->setSpacing(16);
        layout->addWidget(textLabel(title, "settingsTitle"));
        layout->addWidget(textLabel(description, "settingDetail"));
        scroll->setWidget(content);
        settingsPages_->addWidget(scroll);
        return layout;
    };
    const auto preferencesFile = QDir(dataRoot_).filePath("browser.ini");
    QSettings preferences(preferencesFile, QSettings::IniFormat);

    auto *playback = page("播放与浏览", "让每一次试听都顺手。", "playbackSettings");
    auto *audition = group(playback, "试听方式");
    autoPlay_->setText("");
    autoPlay_->setAccessibleName("选中曲目后自动试听");
    setting(audition, "选中曲目后自动试听", "关闭时，点击波形或播放按钮开始试听。", autoPlay_);
    auto *recursive = new PreferenceSwitch;
    recursive->setObjectName("settingsIncludeSubfolders");
    recursive->setAccessibleName("浏览时包含子文件夹");
    recursive->setChecked(subfolders_->isChecked());
    connect(recursive, &QCheckBox::toggled, subfolders_, &QCheckBox::setChecked);
    connect(subfolders_, &QCheckBox::toggled, recursive, &QCheckBox::setChecked);
    setting(audition, "包含子文件夹", "显示当前目录及其下级目录中的音乐。", recursive);
    auto *audio = group(playback, "试听音量");
    auto *volumeControl = new QWidget;
    auto *volumeRow = new QHBoxLayout(volumeControl);
    volumeRow->setContentsMargins(0, 0, 0, 0);
    auto *volumeSlider = new QSlider(Qt::Horizontal);
    volumeSlider->setObjectName("settingsPreviewVolume");
    volumeSlider->setRange(0, 100);
    volumeSlider->setValue(volume_->value());
    volumeSlider->setFixedWidth(120);
    volumeSlider->setAccessibleName("设置试听音量");
    auto *volumeValue = new QLabel(QString::number(volume_->value()) + "%");
    volumeValue->setFixedWidth(36);
    volumeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    volumeRow->addWidget(volumeSlider);
    volumeRow->addWidget(volumeValue);
    connect(volumeSlider, &QSlider::valueChanged, volume_, &QSlider::setValue);
    connect(volume_, &QSlider::valueChanged, volumeSlider, &QSlider::setValue);
    connect(volumeSlider, &QSlider::valueChanged, volumeValue,
            [volumeValue](int value) { volumeValue->setText(QString::number(value) + "%"); });
    setting(audio, "播放音量", "只影响本软件试听，不修改音乐文件。", volumeControl);
    playback->addStretch();

    auto *cache = page("波形缓存", "已有波形优先复用，整首拖出使用原件；路径修复项使用已校验的兼容文件。", "cachePanel");
    auto *progressGroup = group(cache, "全库准备");
    auto *progress = textLabel("尚未准备全库", "wavePreparationStatus");
    progressGroup->addWidget(progress);
    auto *bar = new QProgressBar;
    bar->setObjectName("wavePreparationProgress");
    bar->setRange(0, 100);
    bar->setValue(0);
    bar->setTextVisible(false);
    bar->setFixedHeight(5);
    progressGroup->addWidget(bar);
    auto *current = textLabel("已有波形优先从本机读取。", "wavePreparationCurrent");
    progressGroup->addWidget(current);
    auto *actions = new QHBoxLayout;
    auto *prepare = new QPushButton("准备全部波形");
    prepare->setObjectName("prepareAllWaves");
    auto *pause = new QPushButton("暂停准备");
    pause->setObjectName("pauseWavePreparation");
    pause->setEnabled(false);
    auto *skip = new QPushButton("跳过当前");
    skip->setObjectName("skipWavePreparation");
    skip->setEnabled(false);
    skip->setToolTip("跳过全库准备中的当前文件并记录异常，不删除音乐。");
    actions->addWidget(prepare);
    actions->addWidget(pause);
    actions->addWidget(skip);
    actions->addStretch();
    progressGroup->addLayout(actions);
    auto *records = new QHBoxLayout;
    auto *failures = new QPushButton("异常详情与重试");
    failures->setObjectName("wavePreparationFailures");
    failures->setEnabled(false);
    records->addWidget(failures);
    auto *references = new QPushButton("重新分析参考波形");
    references->setObjectName("reanalyzeReferenceWaves");
    references->setToolTip("只处理已记录的参考波形，保留原有显示；可暂停，关闭后可续跑。不修改音乐文件。");
    references->setEnabled(false);
    records->addWidget(references);
    records->addStretch();
    progressGroup->addLayout(records);
    auto *shared = group(cache, "共享波形");
    sharedCacheSummary_ = textLabel("尚未添加素材库", "sharedCacheSummary");
    sharedCacheSummary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    shared->addWidget(sharedCacheSummary_);
    auto *sharedButton = new QPushButton("设置共享缓存目录…");
    sharedButton->setObjectName("sharedCacheSettings");
    shared->addWidget(sharedButton, 0, Qt::AlignLeft);
    cache->addWidget(
        textLabel("首次准备仍需要读取音乐；参考波形单独记录。关闭此窗口后，后台任务继续。", "settingDetail"));
    cache->addStretch();
    connect(prepare, &QPushButton::clicked, this, [this] {
        if (indexReady_)
            waveQueue_->prepareAll(preparationSpeed_->currentData().toLongLong());
    });
    connect(pause, &QPushButton::clicked, waveQueue_, &WaveQueue::pausePreparation);
    connect(skip, &QPushButton::clicked, waveQueue_, &WaveQueue::skipCurrent);
    connect(failures, &QPushButton::clicked, this, &Window::showWaveFailures);
    connect(references, &QPushButton::clicked, this, [this] {
        if (indexReady_)
            waveQueue_->retryReferences(preparationSpeed_->currentData().toLongLong());
    });
    connect(sharedButton, &QPushButton::clicked, this, [this] { configureSharedCache(); });

    auto *performance = page("性能", "后台任务为当前剪辑和试听留出空间。", "performanceSettings");
    auto *priority = group(performance, "后台工作");
    editingPriority_ = new PreferenceSwitch;
    editingPriority_->setObjectName("editingPriority");
    editingPriority_->setAccessibleName("剪辑优先，自动让路");
    editingPriority_->setChecked(true);
    setting(priority, "剪辑优先", "试听、收录目录或电脑持续繁忙时，让后台波形等待；空闲后继续。",
            editingPriority_);
    preparationSpeed_ = new PreferenceCombo;
    preparationSpeed_->setObjectName("backgroundReadRate");
    preparationSpeed_->setAccessibleName("后台音频读取速度");
    preparationSpeed_->addItem("低占用 · 1 MiB/s", 1048576);
    preparationSpeed_->addItem("较快 · 10 MiB/s", 10 * 1048576);
    preparationSpeed_->addItem("超快 · 不限速", 0);
    const int savedRate =
        preparationSpeed_->findData(preferences.value("backgroundReadRate", 1048576).toInt());
    preparationSpeed_->setCurrentIndex(savedRate < 0 ? 0 : savedRate);
    setting(priority, "后台读取速度", "1 / 10 MiB/s 为本机后台总额度；不限速时由磁盘、网络和解码速度决定。",
            preparationSpeed_);
    auto *concurrency = new PreferenceCombo;
    concurrency->setObjectName("backgroundConcurrency");
    concurrency->setAccessibleName("同时准备的音频数");
    for (const auto count : {1, 2, 4})
        concurrency->addItem(QString("%1 首").arg(count), count);
    const int savedConcurrency = concurrency->findData(preferences.value("backgroundConcurrency", 1).toInt());
    concurrency->setCurrentIndex(savedConcurrency < 0 ? 0 : savedConcurrency);
    waveQueue_->setPreparationConcurrency(concurrency->currentData().toInt());
    setting(priority, "同时准备", "全库准备可同时分析多首音频；失败和参考波形重试仍逐首处理。", concurrency);
    connect(concurrency, &QComboBox::currentIndexChanged, this, [this, concurrency, preferencesFile] {
        QSettings saved(preferencesFile, QSettings::IniFormat);
        saved.setValue("backgroundConcurrency", concurrency->currentData());
        waveQueue_->setPreparationConcurrency(concurrency->currentData().toInt());
    });
    auto *limits = group(performance, "资源使用");
    limits->addWidget(textLabel(
        "列表只绘制可见行；波形和绘图缓存按预算复用。全库进度分批保存，避免任务积压。", "settingDetail"));
    limits->addWidget(
        textLabel("自动让路根据系统 CPU 判断，不检测达芬奇 GPU 渲染。共享缓存读写与试听不计入上方限速。",
                  "settingDetail"));
    performance->addStretch();
    connect(preparationSpeed_, &QComboBox::currentIndexChanged, this, [this, preferencesFile] {
        QSettings saved(preferencesFile, QSettings::IniFormat);
        saved.setValue("backgroundReadRate", preparationSpeed_->currentData());
        const auto state = waveQueue_->preparation();
        if (state["running"].toBool()) {
            const auto rate = preparationSpeed_->currentData().toLongLong();
            if (state["mode"] == "reference")
                waveQueue_->retryReferences(rate);
            else if (state["mode"] == "failed")
                waveQueue_->retryFailed(rate);
            else
                waveQueue_->prepareAll(rate);
        }
    });

    auto *display = page("显示", "选择舒适的外观，并把空间留给音乐名称和波形。", "displaySettings");
    auto *appearance = group(display, "界面外观");
    appearanceMode_ = new PreferenceCombo;
    appearanceMode_->setObjectName("appearanceMode");
    appearanceMode_->setAccessibleName("界面外观");
    appearanceMode_->addItem("跟随系统", "system");
    appearanceMode_->addItem("清透浅色", "light");
    appearanceMode_->addItem("石墨深色", "dark");
    const int savedAppearance = appearanceMode_->findData(appearanceModeValue_);
    appearanceMode_->setCurrentIndex(savedAppearance < 0 ? 0 : savedAppearance);
    setting(appearance, "外观模式", "立即切换；跟随系统时也会随电脑的外观设置变化。", appearanceMode_);
    connect(appearanceMode_, &QComboBox::currentIndexChanged, this, [this, preferencesFile] {
        appearanceModeValue_ = appearanceMode_->currentData().toString();
        QSettings saved(preferencesFile, QSettings::IniFormat);
        saved.setValue("appearanceMode", appearanceModeValue_);
        applyAppearance();
    });
    auto *layout = group(display, "列表与目录");
    folderIndentation_ = new PreferenceCombo;
    folderIndentation_->setObjectName("folderIndentation");
    folderIndentation_->setAccessibleName("文件夹层级缩进");
    folderIndentation_->addItem("更紧凑", 8);
    folderIndentation_->addItem("紧凑", 12);
    folderIndentation_->addItem("适中", 16);
    const int savedIndent = folderIndentation_->findData(preferences.value("folderIndentation", 12).toInt());
    folderIndentation_->setCurrentIndex(savedIndent < 0 ? 1 : savedIndent);
    tree_->setIndentation(folderIndentation_->currentData().toInt());
    setting(layout, "文件夹层级缩进", "缩小每一级的右移距离，深层目录可以显示更长的名称。",
            folderIndentation_);
    listDensity_ = new PreferenceCombo;
    listDensity_->setObjectName("listDensity");
    listDensity_->setAccessibleName("音乐列表行距");
    listDensity_->addItem("紧凑", 44);
    listDensity_->addItem("标准", 52);
    listDensity_->addItem("宽松", 60);
    const int savedHeight = listDensity_->findData(preferences.value("rowHeight", 52).toInt());
    listDensity_->setCurrentIndex(savedHeight < 0 ? 1 : savedHeight);
    table_->verticalHeader()->setDefaultSectionSize(listDensity_->currentData().toInt());
    setting(layout, "音乐列表行距", "按屏幕大小调整，同样保留名称、目录和行内波形。", listDensity_);
    display->addWidget(
        textLabel("长名称在悬停时显示完整路径；拖动左侧分隔线可调整目录栏宽度。", "settingDetail"));
    display->addStretch();
    connect(folderIndentation_, &QComboBox::currentIndexChanged, this, [this, preferencesFile] {
        tree_->setIndentation(folderIndentation_->currentData().toInt());
        QSettings saved(preferencesFile, QSettings::IniFormat);
        saved.setValue("folderIndentation", folderIndentation_->currentData());
    });
    connect(listDensity_, &QComboBox::currentIndexChanged, this, [this, preferencesFile] {
        table_->verticalHeader()->setDefaultSectionSize(listDensity_->currentData().toInt());
        viewportDebounce_.start();
        QSettings saved(preferencesFile, QSettings::IniFormat);
        saved.setValue("rowHeight", listDensity_->currentData());
    });

    auto *libraries = page("素材库", "搜索使用本机索引，音乐保留在原来的位置。", "librarySettings");
    auto *sources = group(libraries, "已连接的目录");
    librarySummary_ = textLabel("正在打开素材库…", "librarySummary");
    sources->addWidget(librarySummary_);
    libraryList_ = new QListWidget;
    libraryList_->setObjectName("connectedLibraries");
    libraryList_->setAccessibleName("已连接的音乐素材库");
    libraryList_->setSelectionMode(QAbstractItemView::SingleSelection);
    libraryList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    libraryList_->setTextElideMode(Qt::ElideMiddle);
    libraryList_->setMinimumHeight(92);
    libraryList_->setMaximumHeight(240);
    sources->addWidget(libraryList_);
    auto *sourceActions = new QHBoxLayout;
    auto *add = new QPushButton("添加音乐目录…");
    add->setObjectName("settingsAddLibrary");
    refreshLibraryButton_ = new QPushButton("刷新选中目录");
    refreshLibraryButton_->setObjectName("settingsRefreshLibrary");
    removeLibraryButton_ = new QPushButton("从列表移除…");
    removeLibraryButton_->setObjectName("settingsRemoveLibrary");
    removeLibraryButton_->setToolTip("仅断开这个素材库的本机索引；不会删除服务器或本机音乐文件");
    libraryMoreButton_ = new QPushButton("更多…");
    libraryMoreButton_->setObjectName("settingsLibraryMore");
    libraryMoreButton_->setToolTip("管理原目录文件；操作共享目录会影响所有使用者");
    sourceActions->addWidget(add);
    sourceActions->addWidget(refreshLibraryButton_);
    sourceActions->addWidget(removeLibraryButton_);
    sourceActions->addWidget(libraryMoreButton_);
    sourceActions->addStretch();
    sources->addLayout(sourceActions);
    libraries->addWidget(textLabel(
        "移除只修改这台电脑的素材库列表，不删除音乐原文件。共享波形位置在“波形缓存”中设置。", "settingDetail"));
    auto *excluded = group(libraries, "已排除的项目");
    excludedSummary_ = textLabel("选择上方素材库查看。", "settingDetail");
    excluded->addWidget(excludedSummary_);
    excludedFilter_ = new QLineEdit;
    excludedFilter_->setObjectName("excludedFilter");
    excludedFilter_->setPlaceholderText("筛选已排除的文件夹或音乐");
    excludedFilter_->setClearButtonEnabled(true);
    excluded->addWidget(excludedFilter_);
    excludedList_ = new QListWidget;
    excludedList_->setObjectName("excludedCatalogItems");
    excludedList_->setAccessibleName("已从列表排除的项目");
    excludedList_->setMinimumHeight(92);
    excludedList_->setMaximumHeight(150);
    excluded->addWidget(excludedList_);
    restoreExcludedButton_ = new QPushButton("恢复到列表并重新收录");
    restoreExcludedButton_->setObjectName("restoreExcludedItem");
    restoreExcludedButton_->setEnabled(false);
    excluded->addWidget(restoreExcludedButton_, 0, Qt::AlignRight);
    libraries->addWidget(textLabel(
        "恢复只取消这台电脑的排除规则，不会从回收站恢复原文件；仍在原位置的音乐会在重新收录后出现。",
        "settingDetail"));
    libraries->addStretch();
    connect(add, &QPushButton::clicked, this, [this] {
        settingsDialog_->hide();
        addLibrary();
    });
    connect(libraryList_, &QListWidget::currentItemChanged, this, [this] {
        const bool selected = libraryList_->currentItem() && !catalogBusy_;
        refreshLibraryButton_->setEnabled(selected);
        removeLibraryButton_->setEnabled(selected);
        libraryMoreButton_->setEnabled(selected);
        loadLibraryDetails();
    });
    connect(excludedFilter_, &QLineEdit::textChanged, this, &Window::filterExcludedItems);
    connect(excludedList_, &QListWidget::currentItemChanged, this, [this] {
        restoreExcludedButton_->setEnabled(excludedList_->currentItem() && !catalogBusy_);
    });
    connect(restoreExcludedButton_, &QPushButton::clicked, this, &Window::restoreExcludedSelection);
    connect(refreshLibraryButton_, &QPushButton::clicked, this, [this] {
        const auto id = libraryList_->currentItem() ? libraryList_->currentItem()->data(Qt::UserRole).toString()
                                                    : QString();
        if (id.isEmpty())
            return;
        for (const auto &lib : libraries_)
            if (lib.id == id) {
                settingsDialog_->hide();
                beginScan(lib);
                return;
            }
    });
    connect(removeLibraryButton_, &QPushButton::clicked, this, [this] {
        const auto id = libraryList_->currentItem() ? libraryList_->currentItem()->data(Qt::UserRole).toString()
                                                    : QString();
        if (!id.isEmpty())
            confirmCatalogRemoval(RemovalKind::Library, id);
    });
    connect(libraryMoreButton_, &QPushButton::clicked, this, [this] {
        const auto id = libraryList_->currentItem() ? libraryList_->currentItem()->data(Qt::UserRole).toString()
                                                    : QString();
        if (id.isEmpty())
            return;
        QMenu menu(settingsDialog_);
        auto *trash = menu.addAction("将整个原目录移入系统回收站并从列表移除…");
        if (menu.exec(libraryMoreButton_->mapToGlobal(QPoint(0, libraryMoreButton_->height()))) == trash)
            confirmCatalogRemoval(RemovalKind::Library, id, {}, true);
    });

    auto *shortcuts = page("快捷键", "键盘浏览与波形手势可以配合使用。", "shortcutSettings");
    auto *keys = group(shortcuts, "常用操作");
    setting(keys, "搜索音乐", "聚焦顶部搜索框", textLabel("Ctrl + F", "shortcutKey"));
    setting(keys, "打开设置", "设置立即应用", textLabel("Ctrl + ,", "shortcutKey"));
    setting(keys, "播放 / 暂停", "输入文字时不触发", textLabel("空格", "shortcutKey"));
    setting(keys, "定位文件夹", "在左侧输入后确认", textLabel("Enter", "shortcutKey"));
    auto *gestures = group(shortcuts, "波形操作");
    gestures->addWidget(textLabel(
        "单击波形：从对应位置试听。\n拖动波形或名称：向达芬奇交付整首原文件。\n悬停波形：查看对应时间。",
        "settingDetail"));
    shortcuts->addStretch();

    auto saveBool = [preferencesFile](const QString &key, bool value) {
        QSettings saved(preferencesFile, QSettings::IniFormat);
        saved.setValue(key, value);
    };
    connect(autoPlay_, &QCheckBox::toggled, this, [saveBool](bool value) { saveBool("autoPlay", value); });
    connect(editingPriority_, &QCheckBox::toggled, this,
            [saveBool](bool value) { saveBool("editingPriority", value); });
    connect(subfolders_, &QCheckBox::toggled, this, [saveBool](bool value) { saveBool("recursive", value); });
    connect(volume_, &QSlider::valueChanged, this, [preferencesFile](int value) {
        QSettings saved(preferencesFile, QSettings::IniFormat);
        saved.setValue("volume", value);
    });

    connect(waveQueue_, &WaveQueue::preparationChanged, this,
            [this, prepare, pause, skip, failures, references, progress, current, bar] {
                const auto state = waveQueue_->preparation();
                const bool running = state["running"].toBool(), complete = state["complete"].toBool();
                const bool review = state["catalogNeedsReview"].toBool();
                const auto done = state["done"].toInteger(), failed = state["failed"].toInteger();
                const auto knownFailures = state["knownFailures"].toInteger();
                const bool retry = state["mode"] != "all", referenceMode = state["mode"] == "reference";
                const auto total = state[retry ? "retryTotal" : "total"].toInteger();
                const auto processed = retry ? state["retryDone"].toInteger() : done;
                const int percent = total > 0 ? int(qMin(qint64(100), processed * 100 / total)) : 0;
                prepare->setEnabled(!running && indexReady_);
                prepare->setText(review                        ? "检查更新后的素材库"
                                 : complete                    ? "重新检查全部"
                                 : state["cursor"].toInteger() ? "继续准备全部波形"
                                                               : "准备全部波形");
                pause->setEnabled(running);
                skip->setEnabled(state["canSkip"].toBool());
                failures->setEnabled(state["failuresReady"].toBool());
                references->setEnabled(indexReady_ && state["failuresReady"].toBool() &&
                                       state["knownReferences"].toInteger() > 0 && !running);
                references->setText(referenceMode && !state["retryComplete"].toBool() &&
                                            state["retryUpper"].toInteger() > 0
                                        ? "继续分析参考波形"
                                        : "重新分析参考波形");
                preparationSpeed_->setEnabled(true);
                const auto stage = state["sourceRepairPaused"].toBool() ? QString("路径处理暂停")
                                   : state["repairingSource"].toBool() ? QString("自动处理路径")
                                   : state["reconcilingCachedPrefix"].toBool() ? QString("核对已有缓存")
                                   : state["deferred"].toBool() ? QString("自动让路")
                                   : review                   ? QString("上次准备")
                                   : running                  ? QString("准备中")
                                   : (retry ? state["retryComplete"].toBool() : complete)
                                       ? QString("本轮结束")
                                       : QString("已暂停");
                progress->setText(retry ? QString("%1 · %2 %3 / %4\n当前参考 %5 · 待处理异常 %6")
                                              .arg(stage, referenceMode ? "参考波形已分析" : "失败项已重试")
                                              .arg(processed)
                                              .arg(total)
                                              .arg(state["knownReferences"].toInteger())
                                              .arg(state["knownFailures"].toInteger())
                                        : QString("%1 · 已缓存 %2 / %3\n参考波形 %4 · 当前待处理异常 %5 · 批次累计失败 %6")
                                              .arg(stage)
                                              .arg(done)
                                              .arg(total)
                                              .arg(state["reference"].toInteger())
                                              .arg(knownFailures)
                                              .arg(failed));
                bar->setValue(percent);
                const auto error = state["error"].toString();
                cacheStatusButton_->setText(state["sourceRepairPaused"].toBool() ? "路径处理已暂停"
                                            : state["repairingSource"].toBool() ? "正在处理路径"
                                            : state["reconcilingCachedPrefix"].toBool() ? "正在核对缓存"
                                            : state["deferred"].toBool()             ? "后台已让路"
                                            : running && referenceMode             ? "正在分析参考波形"
                                            : running && state["mode"] == "failed" ? "正在重试波形"
                                            : running          ? QString("缓存中 %1%").arg(percent)
                                            : !error.isEmpty() ? "缓存需处理"
                                            : review           ? "缓存待检查"
                                            : knownFailures    ? QString("缓存异常 %1").arg(knownFailures)
                                                               : "波形缓存");
                cacheStatusButton_->setToolTip(progress->text() + (error.isEmpty() ? "" : "\n" + error) +
                                               "\n点击打开波形缓存设置");
                QString detail;
                if (state["sourceRepairPaused"].toBool())
                    detail = "当前任务与已有波形已保留，处理连接或缓存问题后可继续。";
                else if (state["repairingSource"].toBool())
                    detail = "正在自动处理当前文件的访问路径，完成后继续准备这首波形。";
                else if (state["reconcilingCachedPrefix"].toBool())
                    detail = "正在核对已有缓存，随后继续全库准备。";
                else if (state["deferred"].toBool())
                    detail = "已保留进度 · " + state["deferReason"].toString();
                else if (referenceMode)
                    detail = QString("本轮已转准确 %1 · 仍为参考 %2 · 分析或同步异常 %3\n%4")
                                 .arg(state["retryConverted"].toInteger())
                                 .arg(state["retryRemaining"].toInteger())
                                 .arg(state["retryFailed"].toInteger())
                                 .arg(running ? state["current"].toString()
                                              : "进度已保留；异常不覆盖原有波形，音乐文件保持原样。");
                else if (state["mode"] == "failed")
                    detail = QString("%1 · 已处理 %2 / %3 · 本次仍失败 %4")
                                 .arg(state["resumeAllAfterRetry"].toBool() ? "先重试已有异常，随后继续全库" : "只重试失败")
                                 .arg(state["retryDone"].toInteger())
                                 .arg(state["retryTotal"].toInteger())
                                 .arg(state["retryFailed"].toInteger());
                else if (state["publishing"].toBool())
                    detail = "正在补充共享波形 · 复用本机缓存，不读取音乐内容";
                else if (review)
                    detail = "目录已更新，已有波形保留。检查后可处理新增或变化的音乐。";
                else if (running && !state["current"].toString().isEmpty()) {
                    detail = QString("%1 · %2\n已读 %3 MiB · 用时 %4 秒")
                                 .arg(state["foreground"].toBool() ? "优先准备当前列表" : "后台准备",
                                      state["current"].toString())
                                 .arg(state["readBytes"].toInteger() / 1048576.0, 0, 'f', 1)
                                 .arg(state["elapsedMs"].toInteger() / 1000);
                    if (state["decodedMs"].toInteger() > 0)
                        detail += " · 已分析 " + elapsedText(state["decodedMs"].toInteger()) +
                                  (state["estimatedMs"].toInteger() > 0
                                       ? " / " + elapsedText(state["estimatedMs"].toInteger())
                                       : "");
                } else
                    detail = running    ? "正在读取本机索引与已有波形…"
                             : complete ? "本轮准备结束，失败文件可在异常详情中重试。"
                                        : "进度已保留，点击准备可继续。";
                if (!error.isEmpty() && !running)
                    detail = error;
                if (running && !state["deferred"].toBool()) {
                    const auto rate = state["rate"].toInteger();
                    detail += QString("\n%1 · 正在处理 %2 首")
                                  .arg(rate == 0 ? QString("不限速")
                                                 : QString("后台总额度 %1 MiB/s").arg(rate / 1048576))
                                  .arg(state["activeJobs"].toInt());
                }
                current->setText(detail);
                current->setToolTip(state["currentPath"].toString());
            });
    settingsNavigation_->setCurrentRow(0);
    refreshSettingsLibraries();
}

void Window::refreshSettingsLibraries() {
    if (!librarySummary_ || !sharedCacheSummary_ || !libraryList_)
        return;
    const auto selectedId = libraryList_->currentItem()
                                ? libraryList_->currentItem()->data(Qt::UserRole).toString()
                                : scopeLibrary_;
    QSignalBlocker blocker(libraryList_);
    libraryList_->clear();
    QStringList caches;
    for (const auto &lib : libraries_) {
        auto *item = new QListWidgetItem(lib.name + "\n" + QDir::toNativeSeparators(lib.root), libraryList_);
        item->setData(Qt::UserRole, lib.id);
        item->setData(Qt::UserRole + 1, lib.name);
        item->setData(Qt::UserRole + 2, QDir::toNativeSeparators(lib.root));
        item->setToolTip(lib.name + "\n" + QDir::toNativeSeparators(lib.root));
        item->setSizeHint(QSize(0, 52));
        caches << lib.name + "\n" +
                      (lib.sharedCache.isEmpty() ? "尚未接入共享缓存"
                                                 : QDir::toNativeSeparators(lib.sharedCache));
    }
    for (int i = 0; i < libraryList_->count(); ++i)
        if (libraryList_->item(i)->data(Qt::UserRole).toString() == selectedId) {
            libraryList_->setCurrentRow(i);
            break;
        }
    if (libraryList_->currentRow() < 0 && libraryList_->count())
        libraryList_->setCurrentRow(0);
    librarySummary_->setText(libraries_.isEmpty()
                                 ? "尚未添加音乐目录。"
                                 : QString("%1 个素材库 · 选择一项管理").arg(libraries_.size()));
    const bool hasSelection = libraryList_->currentItem() && !catalogBusy_;
    refreshLibraryButton_->setEnabled(hasSelection);
    removeLibraryButton_->setEnabled(hasSelection);
    libraryMoreButton_->setEnabled(hasSelection);
    loadLibraryDetails();
    sharedCacheSummary_->setText(caches.isEmpty() ? "先添加音乐目录，再选择专用共享缓存文件夹。"
                                                  : caches.join("\n\n"));
    if (auto *prepare = settingsDialog_->findChild<QPushButton *>("prepareAllWaves"))
        prepare->setEnabled(indexReady_ && !waveQueue_->preparation()["running"].toBool());
    if (auto *references = settingsDialog_->findChild<QPushButton *>("reanalyzeReferenceWaves")) {
        const auto state = waveQueue_->preparation();
        references->setEnabled(indexReady_ && state["failuresReady"].toBool() &&
                               state["knownReferences"].toInteger() > 0 && !state["running"].toBool());
    }
}

void Window::loadLibraryDetails() {
    if (!libraryList_ || !excludedList_)
        return;
    const auto generation = ++libraryDetailsGeneration_;
    const auto selectedId = libraryList_->currentItem()
                                ? libraryList_->currentItem()->data(Qt::UserRole).toString()
                                : QString();
    excludedItems_.clear();
    excludedList_->clear();
    restoreExcludedButton_->setEnabled(false);
    if (selectedId.isEmpty() || catalogBusy_) {
        excludedSummary_->setText(catalogBusy_ ? "正在更新素材库…" : "选择上方素材库查看。");
        return;
    }
    excludedSummary_->setText("正在读取排除记录…");
    struct Details {
        QHash<QString, qint64> counts;
        QList<CatalogExcludedItem> excluded;
        QString error;
    };
    auto *watcher = new QFutureWatcher<Details>(this);
    connect(watcher, &QFutureWatcher<Details>::finished, this,
            [this, watcher, generation, selectedId] {
                const auto details = watcher->result();
                watcher->deleteLater();
                if (generation != libraryDetailsGeneration_ || !libraryList_->currentItem() ||
                    libraryList_->currentItem()->data(Qt::UserRole).toString() != selectedId)
                    return;
                for (int row = 0; row < libraryList_->count(); ++row) {
                    auto *item = libraryList_->item(row);
                    const auto id = item->data(Qt::UserRole).toString();
                    const auto name = item->data(Qt::UserRole + 1).toString();
                    const auto root = item->data(Qt::UserRole + 2).toString();
                    if (details.counts.contains(id))
                        item->setText(QString("%1 · %2 首\n%3").arg(name).arg(details.counts[id]).arg(root));
                }
                if (!details.error.isEmpty()) {
                    excludedSummary_->setText("排除记录暂时无法读取：" + details.error);
                    return;
                }
                excludedItems_ = details.excluded;
                filterExcludedItems();
            });
    watcher->setFuture(QtConcurrent::run(&catalogPool_,
                                         [database = databasePath_, libraries = libraries_, selectedId] {
                                             Details details;
                                             Index db(database);
                                             if (!db.open(&details.error))
                                                 return details;
                                             for (const auto &library : libraries)
                                                 details.counts.insert(library.id, db.countForLibrary(library.id));
                                             details.excluded = db.excludedItems(selectedId, &details.error);
                                             return details;
                                         }));
}

void Window::filterExcludedItems() {
    if (!excludedList_)
        return;
    QSignalBlocker blocker(excludedList_);
    excludedList_->clear();
    const auto query = excludedFilter_->text().trimmed();
    int matches = 0;
    for (const auto &excluded : excludedItems_) {
        if (!query.isEmpty() && !excluded.path.contains(query, Qt::CaseInsensitive))
            continue;
        ++matches;
        if (excludedList_->count() >= 200)
            continue;
        auto *item = new QListWidgetItem(
            QString("%1 · %2").arg(excluded.isFolder ? "文件夹" : "音乐", excluded.path), excludedList_);
        item->setData(Qt::UserRole, excluded.path);
        item->setData(Qt::UserRole + 1, excluded.isFolder);
        item->setToolTip(excluded.path);
    }
    restoreExcludedButton_->setEnabled(false);
    excludedSummary_->setText(excludedItems_.isEmpty()
                                  ? "这个素材库目前没有排除项。"
                                  : QString("%1 项已排除%2；%3")
                                        .arg(excludedItems_.size())
                                        .arg(query.isEmpty() ? QString() : QString(" · 筛选匹配 %1 项").arg(matches))
                                        .arg(matches > 200 ? "只显示前 200 项，可继续缩小关键词" : "选择一项恢复"));
}

void Window::restoreExcludedSelection() {
    if (catalogBusy_ || !excludedList_->currentItem() || !libraryList_->currentItem())
        return;
    if (scan_ || registration_) {
        setStatus("请先停止当前收录或等待目录检查结束，再恢复排除项。");
        return;
    }
    const auto libraryId = libraryList_->currentItem()->data(Qt::UserRole).toString();
    const auto path = excludedList_->currentItem()->data(Qt::UserRole).toString();
    const bool folder = excludedList_->currentItem()->data(Qt::UserRole + 1).toBool();
    const auto found = std::find_if(libraries_.cbegin(), libraries_.cend(),
                                    [&libraryId](const Library &library) { return library.id == libraryId; });
    if (found == libraries_.cend())
        return;
    const Library library = *found;
    QMessageBox confirmation(settingsDialog_);
    confirmation.setWindowTitle("恢复到列表");
    confirmation.setText("恢复“" + path + "”的排除规则，并重新收录该素材库？");
    confirmation.setInformativeText("只会读取原位置的音乐，不会从回收站恢复原文件；"
                                    "如果上一级文件夹仍被排除，这个项目仍不会出现。");
    auto *restore = confirmation.addButton("恢复并重新收录", QMessageBox::AcceptRole);
    auto *cancel = confirmation.addButton("取消", QMessageBox::RejectRole);
    confirmation.setDefaultButton(qobject_cast<QPushButton *>(restore));
    confirmation.setEscapeButton(cancel);
    confirmation.exec();
    if (confirmation.clickedButton() != restore)
        return;
    const auto current = std::find_if(libraries_.cbegin(), libraries_.cend(),
                                      [&libraryId](const Library &entry) { return entry.id == libraryId; });
    if (current == libraries_.cend() || QDir::cleanPath(current->root) != QDir::cleanPath(library.root)) {
        setStatus("素材库目录已变化，请刷新后重试。");
        return;
    }
    setCatalogBusy(true);
    setStatus("正在恢复排除规则…");
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, library] {
        const auto error = watcher->result();
        watcher->deleteLater();
        setCatalogBusy(false);
        if (!error.isEmpty()) {
            setStatus("恢复未完成：" + error);
            return;
        }
        setStatus("排除规则已恢复，正在重新收录原位置的音乐…");
        beginScan(library);
    });
    watcher->setFuture(QtConcurrent::run(&catalogPool_, [database = databasePath_, libraryId, path, folder] {
        QString error;
        Index db(database);
        if (!db.open(&error) || !db.restoreExcludedItem(libraryId, path, folder, &error)) {
            if (error.isEmpty())
                error = db.lastError().isEmpty() ? "请稍后重试" : db.lastError();
        }
        return error;
    }));
}

void Window::showSettings(int page) {
    refreshSettingsLibraries();
    settingsNavigation_->setCurrentRow(qBound(0, page, settingsPages_->count() - 1));
    settingsDialog_->show();
    settingsDialog_->raise();
    settingsDialog_->activateWindow();
}
