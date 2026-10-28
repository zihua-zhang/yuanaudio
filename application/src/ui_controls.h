#pragma once

#include "theme.h"
#include "ui_icons.h"
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QStyleOptionButton>
#include <QStyleOptionSlider>
#include <QStylePainter>

// Keep descriptive button text for accessibility and existing state handling;
// the compact transport paints the same state as a vector glyph.
class GlyphButton : public QPushButton {
  public:
    enum Mode { Fixed, Playback, Favorite };
    GlyphButton(const QString &text, ui::Icon glyph, Mode mode = Fixed, QWidget *parent = nullptr)
        : QPushButton(text, parent), glyph_(glyph), mode_(mode) {
        setCursor(Qt::PointingHandCursor);
        setIconSize(QSize(20, 20));
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QStylePainter painter(this);
        QStyleOptionButton option;
        initStyleOption(&option);
        const auto &colors = theme::current();
        const bool filledStar = mode_ == Favorite && text().startsWith(QStringLiteral("★"));
        const auto glyph = mode_ == Playback
                               ? (text().startsWith(QStringLiteral("Ⅱ")) ? ui::Icon::Pause : ui::Icon::Play)
                               : filledStar ? ui::Icon::StarFilled : glyph_;
        const QColor color = !isEnabled() ? colors.disabledText
                              : mode_ == Playback ? colors.accentText
                              : filledStar ? colors.accent : colors.muted;
        option.text.clear();
        option.icon = ui::icon(glyph, color);
        painter.drawControl(QStyle::CE_PushButton, option);
    }

  private:
    ui::Icon glyph_;
    Mode mode_;
};

// Qt's default slider pages on groove clicks. A seek bar should go directly to
// the clicked time, while still supporting the standard keyboard interaction.
class SeekSlider : public QSlider {
  public:
    using QSlider::QSlider;

  protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            QSlider::mousePressEvent(event);
            return;
        }
        QStyleOptionSlider option;
        initStyleOption(&option);
        const auto groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const auto handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        const int span = qMax(1, groove.width() - handle.width());
        const int at = qRound(event->position().x()) - groove.left() - handle.width() / 2;
        setSliderDown(true);
        setSliderPosition(QStyle::sliderValueFromPosition(minimum(), maximum(), at, span, option.upsideDown));
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (!isSliderDown()) {
            QSlider::mouseMoveEvent(event);
            return;
        }
        mousePressEventPosition(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && isSliderDown()) {
            mousePressEventPosition(event);
            setSliderDown(false);
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

  private:
    void mousePressEventPosition(QMouseEvent *event) {
        QStyleOptionSlider option;
        initStyleOption(&option);
        const auto groove = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
        const auto handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
        setSliderPosition(QStyle::sliderValueFromPosition(minimum(), maximum(),
            qRound(event->position().x()) - groove.left() - handle.width() / 2,
            qMax(1, groove.width() - handle.width()), option.upsideDown));
        event->accept();
    }
};
