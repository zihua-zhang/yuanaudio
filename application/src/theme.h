#pragma once

#include <QApplication>
#include <QColor>

// One set of semantic colors is shared by Qt style sheets and custom painters.
// Reading a QApplication property keeps the waveform renderer independent of the
// settings dialog and avoids a dependency on the music index or cache identity.
namespace theme {
struct Colors {
    QColor window, surface, alternate, sidebar, soft, player;
    QColor text, muted, faint, border, strongBorder;
    QColor hover, selected, selectedText, accent, accentHover, accentText;
    QColor disabled, disabledText, wave, played, waveBaseline;
    QColor warningBackground, warningText, referenceBackground, referenceOutline;
    QColor scrollThumb, sliderTrack, searchMark, searchMarkText;
};

inline const Colors &light() {
    static const Colors colors{
        "#f5f5f7", "#ffffff", "#fafafb", "#f1f2f4", "#f6f6f8", "#fcfcfd",
        "#25262a", "#6c7078", "#858a93", "#e4e5e9", "#c8ccd2",
        "#eaedf1", "#e5efff", "#185eb5", "#087af0", "#0069d8", "#ffffff",
        "#f1f2f4", "#979ba3", "#9199a5", "#087af0", "#e7e9ed",
        "#fbf5e9", "#815920", "#fbf5e9", "#a97c3e",
        "#c3c6cd", "#dfe2e7", "#fff0bb", "#644d15"};
    return colors;
}

inline const Colors &dark() {
    static const Colors colors{
        "#1c1d20", "#222326", "#252629", "#1d1e21", "#292a2e", "#252629",
        "#edeef1", "#a3a6ae", "#80858f", "#35373c", "#51555d",
        "#303237", "#293b55", "#d5e7ff", "#69a9ff", "#8bbcff", "#152337",
        "#292b30", "#777d87", "#a1a8b2", "#69a9ff", "#3c4047",
        "#3c3326", "#e5c79b", "#3c3326", "#cfa76e",
        "#51555e", "#41454e", "#5c4723", "#ffedc6"};
    return colors;
}

inline bool isDark() {
    return qApp && qApp->property("sourceAudioDarkMode").toBool();
}

inline const Colors &current() {
    return isDark() ? dark() : light();
}
} // namespace theme
