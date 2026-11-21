// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QPixmap>
#include <QRectF>
#include <QSizeF>
#include <QStyledItemDelegate>

/**
 * A stylized "card"-like delegate for the game grid view.
 * Adapted from QML
 */
class GameCard : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit GameCard(QObject* parent = nullptr);

    /// The Switch 2 or Switch 1 frame for a game icon, scaled to `size` logical pixels and
    /// cached per size and device pixel ratio.
    static QPixmap PlatformBorderPixmap(bool is_switch2, int size, qreal dpr);

    /// The cartridge frame's own proportions: width per height, and the label area where the
    /// game art sits, in the frame's 232x324 coordinates.
    static constexpr qreal CartridgeAspect = 232.0 / 324.0;
    static constexpr QSizeF CartridgeBase{232.0, 324.0};
    static constexpr QRectF CartridgeLabel{32.6, 39.5, 166.0, 234.0};
    static constexpr QRectF CartridgeBody{13.0, 8.0, 205.0, 297.0};
    static constexpr qreal CartridgeBodyRadius = 18.0;

    /// The cartridge frame rendered `height_px` logical pixels tall. Rendering the SVG on every
    /// card paint would be far too slow to scroll, so heights are quantized to 32-pixel buckets
    /// and the cached result is drawn scaled to the exact card.
    static QPixmap CartridgePixmap(int height_px, qreal dpr);
    /// The label area of a cartridge frame placed at `cartridge_rect`.
    static QRectF CartridgeLabelRect(const QRectF& cartridge_rect);
    /// The frame's solid body -- no transparent margin, no baked shadow -- placed at
    /// `cartridge_rect`.
    static QRectF CartridgeBodyRect(const QRectF& cartridge_rect);
    /// The pixmap centered and cover-cropped to fill `target`'s aspect.
    static void DrawCoverCropped(QPainter& painter, const QPixmap& pixmap, const QRectF& target);

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;

    QRect getCardRect(const QStyleOptionViewItem &option, const QModelIndex &index) const;

    bool hitTest(const QPoint& point, const QModelIndex& index,
                 const QWidget* widget, const QRect& cellRect) const;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void setSize(const QSize& newSize, const QSize& contentSize, const int padding, const int columns);

private:
    static constexpr int cardMargin = 8;

    QSize m_size;
    QSize m_contentSize;
    int m_padding;
    int m_columns;
};
