#pragma once

#include "theme.h"
#include <QHash>
#include <QString>

// Styling stays separate from layout and indexing. Both appearances share the
// same geometry, so switching themes does not rebuild or reload the music view.
inline QString appearanceStyle(const theme::Colors &colors) {
    QString style = QStringLiteral(R"(
        QWidget { color:@{text}; @{fontRule} font-size:14px; }
        QMainWindow, QWidget#shell, QDialog { background:@{window}; }
        QLabel { background:transparent; border:none; }
        QLabel#heading, QLabel#brandTitle { font-size:17px; font-weight:600; }
        QLabel#muted, QLabel#wavePreparationCurrent { color:@{muted}; font-size:12px; }
        QLabel#scopeTitle { font-size:22px; font-weight:600; }
        QLabel#resultCount, QLabel#workspaceNotice { color:@{muted}; font-size:12px; }
        QLabel#emptyTitle { color:@{text}; font-size:20px; font-weight:600; }
        QLabel#emptyDetail { color:@{muted}; }
        QLabel#playerTitle { font-size:14px; font-weight:600; }
        QLabel#playerClock { color:@{muted}; font-size:12px; }
        QLabel#playerTags { color:@{muted}; font-size:12px; }
        QWidget#topBar { background:@{surface}; border:none; border-bottom:1px solid @{border}; }
        QWidget#sidebar { background:@{sidebar}; border:none; border-radius:0; }
        QWidget#content { background:@{surface}; border:none; border-radius:0; }
        QWidget#playerBar { background:@{player}; border:none; border-top:1px solid @{border}; border-radius:0; }
        QLineEdit, QPlainTextEdit { background:@{surface}; color:@{text}; border:1px solid @{border}; border-radius:8px; padding:7px 11px; selection-background-color:@{selected}; selection-color:@{selectedText}; }
        QLineEdit:focus, QPlainTextEdit:focus { border-color:@{accent}; }
        QLineEdit:disabled, QPlainTextEdit:disabled { background:@{disabled}; color:@{disabledText}; }
        QLineEdit#musicSearch { background:@{soft}; border:1px solid @{border}; border-radius:8px; padding:8px 12px; }
        QLineEdit#musicSearch:focus { background:@{surface}; border-color:@{accent}; }
        QLineEdit#sidebarSearch, QLineEdit#folderSearch { background:@{surface}; border-color:@{border}; border-radius:7px; padding:7px 9px; font-size:12px; }
        QLineEdit#sidebarSearch:focus, QLineEdit#folderSearch:focus { border-color:@{accent}; }
        QPushButton { background:@{surface}; color:@{text}; border:1px solid @{border}; border-radius:8px; padding:7px 12px; }
        QPushButton:hover { background:@{hover}; border-color:@{strongBorder}; }
        QPushButton:checked, QPushButton:pressed { background:@{selected}; color:@{selectedText}; border-color:@{strongBorder}; }
        QPushButton:focus { border-color:@{accent}; }
        QPushButton:disabled { background:@{disabled}; color:@{disabledText}; border-color:@{border}; }
        QPushButton#primary { background:@{accent}; color:@{accentText}; border:1px solid @{accent}; font-weight:600; }
        QPushButton#primary:hover { background:@{accentHover}; border-color:@{accentHover}; color:@{accentText}; }
        QPushButton#primary:disabled { background:@{disabled}; border-color:@{border}; color:@{disabledText}; }
        QPushButton#navigation { background:transparent; color:@{muted}; border:1px solid transparent; font-size:19px; padding:1px 4px; border-radius:6px; }
        QPushButton#navigation:hover { background:@{hover}; color:@{text}; }
        QPushButton#navigation:focus { border-color:@{accent}; }
        QPushButton#navigation:disabled { color:@{disabledText}; }
        QPushButton#iconButton, QPushButton#openSettings { background:transparent; border:1px solid transparent; padding:6px; border-radius:7px; }
        QPushButton#iconButton:hover, QPushButton#openSettings:hover { background:@{hover}; }
        QPushButton#iconButton:pressed, QPushButton#openSettings:pressed { background:@{selected}; }
        QPushButton#iconButton:focus, QPushButton#openSettings:focus { border-color:@{accent}; }
        QPushButton#iconButton:disabled { color:@{disabledText}; background:transparent; }
        QPushButton#libraryAction { background:@{soft}; border:1px solid @{border}; padding:7px 12px; font-size:13px; }
        QPushButton#libraryAction:hover { background:@{hover}; border-color:@{strongBorder}; }
        QPushButton#libraryAction:focus { border-color:@{accent}; }
        QPushButton#libraryAction:disabled { background:@{disabled}; color:@{disabledText}; }
        QPushButton#workspaceToggle { background:transparent; border:1px solid @{border}; padding:7px 10px; font-size:12px; }
        QPushButton#workspaceToggle:hover { background:@{hover}; }
        QPushButton#workspaceToggle:checked { background:@{selected}; color:@{selectedText}; border-color:@{selected}; }
        QPushButton#workspaceToggle:focus { border-color:@{accent}; }
        QPushButton#transportButton { background:@{accent}; color:@{accentText}; border:1px solid @{accent}; border-radius:20px; padding:0; }
        QPushButton#transportButton:hover { background:@{accentHover}; border-color:@{accentHover}; }
        QPushButton#transportButton:pressed { background:@{accentHover}; color:@{accentText}; }
        QPushButton#transportButton:focus { border-color:@{selectedText}; }
        QPushButton#transportButton:disabled { background:@{disabled}; border-color:@{border}; color:@{disabledText}; }
        QPushButton#playerTags { background:transparent; color:@{muted}; border:1px solid transparent; font-size:12px; padding:5px 8px; }
        QPushButton#playerTags:hover { background:@{hover}; color:@{text}; }
        QPushButton#playerTags:focus { border-color:@{accent}; }
        QWidget#settingsSidebar { background:@{sidebar}; border-right:1px solid @{border}; }
        QWidget#settingsFooter { background:@{soft}; border-top:1px solid @{border}; }
        QScrollArea#settingsScroll, QWidget#playbackSettings, QWidget#cachePanel,
        QWidget#performanceSettings, QWidget#displaySettings, QWidget#librarySettings,
        QWidget#shortcutSettings { background:@{window}; border:none; }
        QFrame#settingsGroup { background:@{surface}; border:1px solid @{border}; border-radius:10px; }
        QLabel#settingsTitle { font-size:23px; font-weight:600; }
        QLabel#settingsSectionTitle { font-size:14px; font-weight:600; margin-top:8px; }
        QLabel#settingTitle { font-size:14px; font-weight:500; }
        QLabel#settingDetail, QLabel#shortcutKey { color:@{muted}; font-size:12px; }
        QPushButton#waveCacheToggle { background:transparent; color:@{muted}; border:1px solid transparent; padding:2px 7px; font-size:11px; border-radius:5px; }
        QPushButton#waveCacheToggle:hover { background:@{hover}; color:@{text}; }
        QPushButton#waveCacheToggle:focus { border-color:@{accent}; }
        QProgressBar#wavePreparationProgress { background:@{sliderTrack}; border:none; border-radius:3px; color:@{text}; }
        QProgressBar#wavePreparationProgress::chunk { background:@{accent}; border-radius:3px; }
        QWidget#searchFeedback { background:@{warningBackground}; border:1px solid @{border}; border-radius:8px; }
        QWidget#searchFeedback QLabel { color:@{warningText}; }
        QTreeView { background:transparent; border:none; outline:none; font-size:13px; }
        QTreeView::item { height:32px; padding:0 3px; border-radius:6px; }
        QTreeView::item:hover { background:@{hover}; }
        QTreeView::item:selected { background:@{selected}; color:@{selectedText}; }
        QListWidget { background:@{surface}; border:1px solid @{border}; border-radius:8px; }
        QListWidget::item { padding:7px; }
        QListWidget::item:selected { background:@{selected}; color:@{selectedText}; }
        QListWidget#settingsNavigation { background:transparent; border:none; outline:none; }
        QListWidget#settingsNavigation::item { padding:0 12px; border-radius:7px; margin:2px 0; }
        QListWidget#settingsNavigation::item:selected { background:@{selected}; color:@{selectedText}; }
        QListWidget#settingsNavigation::item:hover { background:@{hover}; }
        QTableView { background:@{surface}; color:@{text}; alternate-background-color:@{alternate}; border:none; selection-background-color:@{selected}; selection-color:@{selectedText}; outline:none; }
        QTableView::item { padding:4px; border-bottom:1px solid @{border}; }
        QTableView::item:hover { background:@{hover}; }
        QHeaderView::section { background:@{surface}; color:@{muted}; font-size:12px; font-weight:400; padding:9px 6px; border:none; border-bottom:1px solid @{border}; }
        QScrollBar:vertical { width:8px; background:transparent; margin:0; }
        QScrollBar::handle:vertical { background:@{scrollThumb}; border-radius:4px; min-height:32px; }
        QScrollBar::handle:vertical:hover { background:@{strongBorder}; }
        QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
        QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical { background:transparent; }
        QScrollBar:horizontal { height:8px; background:transparent; margin:0; }
        QScrollBar::handle:horizontal { background:@{scrollThumb}; border-radius:4px; min-width:32px; }
        QScrollBar::add-line:horizontal,QScrollBar::sub-line:horizontal { width:0; }
        QScrollBar::add-page:horizontal,QScrollBar::sub-page:horizontal { background:transparent; }
        QSplitter::handle { background:@{border}; width:1px; }
        QSplitter::handle:hover { background:@{accent}; }
        QCheckBox { spacing:6px; color:@{muted}; background:transparent; font-size:12px; }
        QComboBox { background:@{surface}; color:@{text}; border:1px solid @{border}; border-radius:8px; padding:7px 12px; }
        QComboBox:focus { border-color:@{accent}; }
        QComboBox:disabled { background:@{disabled}; color:@{disabledText}; }
        QComboBox::drop-down { border:none; width:24px; }
        QComboBox QAbstractItemView { background:@{surface}; color:@{text}; selection-background-color:@{selected}; selection-color:@{selectedText}; }
        QSlider { background:transparent; }
        QSlider::groove:horizontal { height:3px; background:@{sliderTrack}; border-radius:1px; }
        QSlider::sub-page:horizontal { background:@{accent}; border-radius:1px; }
        QSlider::handle:horizontal { background:@{surface}; border:1px solid @{strongBorder}; width:11px; margin:-5px 0; border-radius:6px; }
        QStatusBar { background:@{player}; color:@{muted}; font-size:11px; border:none; }
        QStatusBar::item { border:none; }
        QMenu { background:@{surface}; color:@{text}; border:1px solid @{border}; padding:5px; }
        QMenu::item { padding:7px 18px; border-radius:5px; }
        QMenu::item:selected { background:@{selected}; color:@{selectedText}; }
        QMenu::separator { height:1px; background:@{border}; margin:4px 8px; }
        QToolTip { background:@{surface}; color:@{text}; border:1px solid @{strongBorder}; padding:6px; font-size:12px; }
    )");
#ifdef Q_OS_MACOS
    // Keep the native macOS system font, including its Chinese fallback.
    style.replace("@{fontRule}", "");
#else
    style.replace("@{fontRule}", "font-family:'Microsoft YaHei UI';");
#endif
    const QHash<QString, QColor> values{
        {"window", colors.window}, {"surface", colors.surface}, {"alternate", colors.alternate},
        {"sidebar", colors.sidebar}, {"soft", colors.soft}, {"player", colors.player},
        {"text", colors.text}, {"muted", colors.muted}, {"border", colors.border},
        {"strongBorder", colors.strongBorder}, {"hover", colors.hover},
        {"selected", colors.selected}, {"selectedText", colors.selectedText},
        {"accent", colors.accent}, {"accentHover", colors.accentHover},
        {"accentText", colors.accentText}, {"disabled", colors.disabled},
        {"disabledText", colors.disabledText}, {"warningBackground", colors.warningBackground},
        {"warningText", colors.warningText}, {"scrollThumb", colors.scrollThumb},
        {"sliderTrack", colors.sliderTrack}};
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        style.replace("@{" + it.key() + '}', it.value().name());
    return style;
}
