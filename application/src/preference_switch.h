#pragma once
#include "theme.h"
#include <QCheckBox>
#include <QComboBox>
#include <QPainter>

// A native checkable control with a clear on/off shape; no animation or idle timer.
class PreferenceSwitch : public QCheckBox {
  public:
    explicit PreferenceSwitch(QWidget *parent = nullptr) : QCheckBox(parent) {
        setFixedSize(38, 24);
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::PointingHandCursor);
    }

  protected:
    bool hitButton(const QPoint &point) const override {
        return rect().contains(point);
    }
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        const auto &colors = theme::current();
        painter.setBrush(!isEnabled()  ? colors.disabledText
                         : isChecked() ? colors.accent
                                       : colors.faint);
        painter.drawRoundedRect(QRectF(1, 3, 36, 18), 9, 9);
        painter.setBrush(isChecked() ? colors.accentText : colors.surface);
        painter.drawEllipse(QPointF(isChecked() ? 28 : 10, 12), 7, 7);
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(colors.accent, 1));
            painter.drawRoundedRect(QRectF(.5, .5, 37, 23), 11, 11);
        }
    }
};

class PreferenceCombo : public QComboBox {
  public:
    explicit PreferenceCombo(QWidget *parent = nullptr) : QComboBox(parent) {
        setMinimumWidth(130);
        setStyleSheet("QComboBox::down-arrow { image:none; }");
    }

  protected:
    void paintEvent(QPaintEvent *event) override {
        QComboBox::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(isEnabled() ? theme::current().muted : theme::current().disabledText, 1.4, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        const qreal x = width() - 16, y = height() / 2.0;
        painter.drawPolyline(
            QPolygonF{QPointF(x - 3, y - 1.5), QPointF(x, y + 1.5), QPointF(x + 3, y - 1.5)});
    }
};
