// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include <QFrame>
#include <QHash>
#include <QPixmap>
#include <QPushButton>

#include "common/common_types.h"
#include "common/settings_enums.h"
#include "yuzu/game/home_background.h"
#include "yuzu/game/home_side_panel.h"

class QButtonGroup;
class QCheckBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QSlider;
class QToolButton;

/// 16:9 preview of the current background; click to browse, or drop a file onto it.
class BackgroundDropZone : public QFrame {
    Q_OBJECT

public:
    explicit BackgroundDropZone(HomeBackground* background, QWidget* parent = nullptr);

    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;

signals:
    void BrowseRequested();
    void FileDropped(const QString& path);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    HomeBackground* background;
    bool hovered = false;
    bool drag_active = false;
};

/// A small push button whose body shows one background color; the panel opens the color dialog.
class ColorSwatchButton : public QPushButton {
public:
    explicit ColorSwatchButton(QWidget* parent = nullptr);

    void SetColor(const QColor& color);
    const QColor& GetColor() const {
        return color;
    }

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    QColor color;
    bool hovered = false;
};

/// Sidebar opened from the home screen's brush button or context menu. Every change applies
/// immediately.
class HomeCustomizePanel : public HomeSidePanel {
    Q_OBJECT

public:
    explicit HomeCustomizePanel(HomeBackground* background, QWidget* parent = nullptr);

    /// Reflects the current settings (mode, icon size, background, recents).
    void SyncFromSettings();

signals:
    /// Empty path means no background.
    void BackgroundChosen(const QString& path);
    void RecentBackgroundRemoved(const QString& path);
    /// `index` is 0 for the solid color and the gradient's start, 1 for the gradient's end.
    void BackgroundKindRequested(HomeBackground::Kind kind);
    void BackgroundColorRequested(int index, const QColor& color);
    void GameListModeRequested(Settings::GameListMode mode);
    void GameIconSizeRequested(u32 size);
    void GameIconBordersRequested(bool show);
    void CartridgeKindRequested(u8 kind);
    void CarouselCardSizeRequested(u8 index);
    void CarouselSideCountRequested(u8 count);
    void CarouselBannerBackgroundRequested(bool show);

protected:
    void OnPaletteChanged() override;
    void OnLanguageChanged() override;

private:
    void Browse();
    void RefreshRecents();
    void RefreshGlyphs();
    void RetranslateUI();
    void UpdateIconSizeLabel();
    QPixmap Thumbnail(const QString& path);
    void OnRecentContextMenu(const QPoint& pos);

    HomeBackground* background;

    QLabel* title_icon = nullptr;
    QLabel* title_label = nullptr;
    QToolButton* close_button = nullptr;
    QLabel* background_label = nullptr;
    QButtonGroup* kind_group = nullptr;
    std::array<QToolButton*, 3> kind_buttons{};
    QWidget* solid_row = nullptr;
    QLabel* solid_label = nullptr;
    ColorSwatchButton* solid_swatch = nullptr;
    QWidget* gradient_row = nullptr;
    QLabel* gradient_label = nullptr;
    ColorSwatchButton* gradient_swatch_start = nullptr;
    ColorSwatchButton* gradient_swatch_end = nullptr;
    BackgroundDropZone* drop_zone = nullptr;
    QLabel* recent_label = nullptr;
    QListWidget* recent_list = nullptr;
    QLabel* mode_label = nullptr;
    QButtonGroup* mode_group = nullptr;
    std::array<QToolButton*, 3> mode_buttons{};
    QCheckBox* border_check = nullptr;
    QWidget* cartridge_row = nullptr;
    QLabel* cartridge_label = nullptr;
    QButtonGroup* cartridge_group = nullptr;
    std::array<QToolButton*, 2> cartridge_buttons{};
    QWidget* card_size_row = nullptr;
    QLabel* card_size_label = nullptr;
    QButtonGroup* card_size_group = nullptr;
    std::array<QToolButton*, 5> card_size_buttons{};
    QWidget* side_count_row = nullptr;
    QLabel* side_count_label = nullptr;
    QButtonGroup* side_count_group = nullptr;
    std::array<QToolButton*, 5> side_count_buttons{};
    QCheckBox* banner_check = nullptr;
    QLabel* icon_size_title = nullptr;
    QLabel* icon_size_value = nullptr;
    QSlider* icon_size_slider = nullptr;

    QHash<QString, QPixmap> thumbnail_cache;
    bool syncing = false;
};
