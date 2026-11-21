// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QListView>
#include <QPixmap>
#include <QString>

#include <array>
#include <vector>

#include "common/common_types.h"

class GameListModel;
class QVariantAnimation;

/// The home screen's carousel: one large centered game card with the neighbouring games stacked
/// behind it on both sides, each tilted and faded with its distance from the center. The base
/// stays QListView so the surrounding wiring (selection model, clicked/doubleClicked, context
/// menus, the transparency style sheet) keeps working; the layout, painting and navigation are
/// entirely this class's.
class GameCarousel : public QListView {
    Q_OBJECT

public:
    /// The centered card's share of the available view height, in the order the
    /// carousel_card_size setting stores them (full, three quarters, two thirds, half, a
    /// third). An out-of-range stored index reads as full.
    static constexpr std::array<qreal, 5> CardSizeFractions{1.0, 0.75, 2.0 / 3.0, 0.5, 1.0 / 3.0};

    explicit GameCarousel(QWidget* parent = nullptr);

    void SetModel(GameListModel* model);
    void ApplyFilter(const QString& edit_filter_text, GameListModel* model);
    /// Kept for the surrounding wiring: the card sizes follow the viewport, not this setting.
    void UpdateIconSize();

    /// The program whose game the backdrop should feature: the slide's destination once one is
    /// running, the settled center otherwise.
    u64 CenteredProgramId() const;

    QModelIndex indexAt(const QPoint& point) const override;
    QRect visualRect(const QModelIndex& index) const override;
    bool isIndexHidden(const QModelIndex& index) const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void scrollTo(const QModelIndex& index, ScrollHint hint) override;
    QModelIndex moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers) override;
    void setSelection(const QRect& rect, QItemSelectionModel::SelectionFlags command) override;
    QRegion visualRegionForSelection(const QItemSelection& selection) const override;
    void wheelEvent(QWheelEvent* event) override;
    void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;

private:
    int PositionOf(int row) const;
    void MoveTo(int position);
    /// Re-applies the stored filter once the model has changed shape.
    void ScheduleRelayout();

    QString m_filter_text;
    /// The rows the filter left visible, left to right.
    std::vector<int> m_visible;
    /// The animated position within m_visible of the centered card.
    qreal m_position = 0.0;
    QVariantAnimation* m_position_anim = nullptr;
    /// The slide's destination position, or -1 when no slide is running.
    int m_pending_center = -1;
    bool m_relayout_queued = false;

signals:
    /// The game the backdrop should feature: the slide's destination once one is running, the
    /// settled center otherwise. Emitted on selection changes and relayouts.
    void CenteredGameChanged(u64 program_id);
};
