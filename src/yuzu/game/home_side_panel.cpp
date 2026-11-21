// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QVBoxLayout>

#include "yuzu/game/home_side_panel.h"

namespace HomeGlyphs {

namespace {
QPixmap MakeCanvas(qreal dpr, int size) {
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    return pixmap;
}
} // namespace

QIcon Close(const QColor& color, qreal dpr, int size) {
    QPixmap pixmap = MakeCanvas(dpr, size);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 20.0, size / 20.0);
    p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(5.5, 5.5), QPointF(14.5, 14.5));
    p.drawLine(QPointF(14.5, 5.5), QPointF(5.5, 14.5));
    return QIcon(pixmap);
}

QIcon Brush(const QColor& color, qreal dpr, int size) {
    QPixmap pixmap = MakeCanvas(dpr, size);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 20.0, size / 20.0);
    p.setPen(Qt::NoPen);
    p.setBrush(color);

    // Drawn upright around the origin, then tilted so the handle points to the top right.
    p.translate(10.5, 9.5);
    p.rotate(45);
    p.drawRoundedRect(QRectF(-1.5, -10.5, 3.0, 8.0), 1.5, 1.5);
    p.drawRoundedRect(QRectF(-2.6, -2.0, 5.2, 3.2), 0.8, 0.8);

    QPainterPath bristles;
    bristles.moveTo(-2.8, 1.9);
    bristles.lineTo(2.8, 1.9);
    bristles.cubicTo(3.2, 5.0, 1.6, 7.6, -0.6, 9.4);
    bristles.cubicTo(-0.2, 7.2, -3.4, 5.6, -2.8, 1.9);
    p.drawPath(bristles);
    return QIcon(pixmap);
}

} // namespace HomeGlyphs

HomeSidePanel::HomeSidePanel(QWidget* parent) : QFrame{parent} {
    setFrameShape(QFrame::NoFrame);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    setMinimumWidth(0);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Window);
    setFocusPolicy(Qt::StrongFocus);

    auto* outer = new QVBoxLayout(this);
    // Leaves room for the border painted in paintEvent.
    outer->setContentsMargins(1, 0, 0, 0);
    outer->setSpacing(0);

    scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);

    content = new QWidget(scroll);
    scroll->setWidget(content);
}

QSize HomeSidePanel::sizeHint() const {
    return {PanelWidth, QFrame::sizeHint().height()};
}

void HomeSidePanel::SetFooter(QWidget* footer_) {
    footer = footer_;
    layout()->addWidget(footer);
}

QLabel* HomeSidePanel::MakeHeading(QWidget* parent, qreal scale) {
    auto* label = new QLabel(parent);
    QFont font = label->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * scale);
    label->setFont(font);
    return label;
}

void HomeSidePanel::paintEvent(QPaintEvent* event) {
    QFrame::paintEvent(event);
    // Mid is black on some dark palettes, so derive the border from the text colour.
    QColor color = palette().color(QPalette::WindowText);
    color.setAlpha(70);
    QPainter p(this);
    p.fillRect(QRect(0, 0, 1, height()), color);
    if (footer && footer->isVisible()) {
        p.fillRect(QRect(1, footer->geometry().top(), width() - 1, 1), color);
    }
}

void HomeSidePanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        emit CloseRequested();
        return;
    }
    QFrame::keyPressEvent(event);
}

void HomeSidePanel::changeEvent(QEvent* event) {
    // Queued so derived panels are fully constructed, and because setting child style sheets
    // while Qt is still applying a style change crashes it.
    const auto queue = [this](bool& flag, void (HomeSidePanel::*hook)()) {
        if (flag) {
            return;
        }
        flag = true;
        QMetaObject::invokeMethod(
            this,
            [this, &flag, hook] {
                flag = false;
                (this->*hook)();
            },
            Qt::QueuedConnection);
    };
    switch (event->type()) {
    case QEvent::LanguageChange:
        queue(language_refresh_queued, &HomeSidePanel::OnLanguageChanged);
        break;
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
        queue(palette_refresh_queued, &HomeSidePanel::OnPaletteChanged);
        update();
        break;
    default:
        break;
    }
    QFrame::changeEvent(event);
}

FloatingActionButton::FloatingActionButton(QWidget* parent) : QAbstractButton{parent} {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setIconSize(QSize(24, 24));
    setFixedSize(sizeHint());
}

QSize FloatingActionButton::sizeHint() const {
    // Extra room below and around the circle for the shadow.
    return {Diameter + 8, Diameter + 10};
}

void FloatingActionButton::enterEvent(QEnterEvent* event) {
    hovered = true;
    update();
    QAbstractButton::enterEvent(event);
}

void FloatingActionButton::leaveEvent(QEvent* event) {
    hovered = false;
    update();
    QAbstractButton::leaveEvent(event);
}

void FloatingActionButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF circle((width() - Diameter) / 2.0, 3, Diameter, Diameter);

    p.setPen(Qt::NoPen);
    for (int i = 4; i >= 1; --i) {
        p.setBrush(QColor(0, 0, 0, 14));
        p.drawEllipse(circle.translated(0, 2).adjusted(-i, -i + 1, i, i + 1));
    }

    const QPalette& pal = palette();
    QColor fill = pal.color(QPalette::Highlight);
    if (isDown()) {
        fill = fill.darker(115);
    } else if (hovered) {
        fill = fill.lighter(112);
    }
    p.setBrush(fill);
    p.drawEllipse(circle);

    if (hasFocus()) {
        p.setPen(QPen(pal.color(QPalette::HighlightedText), 2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(circle.adjusted(3, 3, -3, -3));
    }

    const QRect icon_rect(QPoint{}, iconSize());
    icon().paint(&p, icon_rect.translated(circle.center().toPoint() - icon_rect.center()));
}
