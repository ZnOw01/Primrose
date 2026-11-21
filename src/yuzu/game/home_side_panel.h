// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QAbstractButton>
#include <QFrame>
#include <QIcon>

class QLabel;
class QScrollArea;

namespace HomeGlyphs {
QIcon Close(const QColor& color, qreal dpr, int size = 20);
QIcon Brush(const QColor& color, qreal dpr, int size = 20);
} // namespace HomeGlyphs

/// Base for the sidebars that slide in beside the home screen. GameList animates the maximum
/// width between 0 and PanelWidth.
class HomeSidePanel : public QFrame {
    Q_OBJECT

public:
    static constexpr int PanelWidth = 340;

    explicit HomeSidePanel(QWidget* parent = nullptr);

    QSize sizeHint() const override;

signals:
    void CloseRequested();

protected:
    /// The scrolled widget derived panels build their layout on.
    QWidget* Content() const {
        return content;
    }

    /// Pins a widget below the scrolled content, always visible.
    void SetFooter(QWidget* footer);

    static QLabel* MakeHeading(QWidget* parent, qreal scale = 1.15);

    /// Run once after the palette or style changed, outside Qt's style change handling.
    virtual void OnPaletteChanged() {}
    virtual void OnLanguageChanged() {}

    void paintEvent(QPaintEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    QScrollArea* scroll = nullptr;
    QWidget* content = nullptr;
    QWidget* footer = nullptr;
    bool palette_refresh_queued = false;
    bool language_refresh_queued = false;
};

/// Round accent-coloured button that floats over the home screen.
class FloatingActionButton : public QAbstractButton {
    Q_OBJECT

public:
    static constexpr int Diameter = 52;

    explicit FloatingActionButton(QWidget* parent = nullptr);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    bool hovered = false;
};
