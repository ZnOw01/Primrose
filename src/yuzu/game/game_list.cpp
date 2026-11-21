// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2015 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>

#include <QAbstractItemView>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QListView>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <qlayoutitem.h>

#include "common/common_types.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/game_presets.h"
#include "core/core.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/loader/loader.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "qt_common/qt_common.h"
#include "qt_common/util/game.h"
#include "qt_common/util/game_art.h"
#include "yuzu/game/carousel.h"
#include "yuzu/game/game_details_panel.h"
#include "yuzu/game/game_grid.h"
#include "yuzu/game/game_list.h"
#include "yuzu/game/game_tree.h"
#include "yuzu/game/home_background.h"
#include "yuzu/game/home_customize_panel.h"
#include "yuzu/game/home_side_panel.h"
#include "yuzu/game/search_field.h"
#include "yuzu/main_window.h"
#include "yuzu/shader_warm_dialog.h"
#include "yuzu/util/controller_navigation.h"

/// The banner backdrop crossfades just behind the slide, so the change reads as one motion.
constexpr int BannerFadeMs = 260;
/// How far up from the bottom the filter bar's fade into the panel surface reaches.
constexpr int BarFadeHeight = 80;

/// Holds the game views and paints the home screen background behind them.
class HomeSurface : public QWidget {
public:
    HomeSurface(HomeBackground* background_, QWidget* parent)
        : QWidget{parent}, background{background_} {
        connect(background, &HomeBackground::FrameChanged, this, [this] {
            cached = {};
            update();
        });

        m_banner_anim = new QVariantAnimation(this);
        m_banner_anim->setDuration(BannerFadeMs);
        m_banner_anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_banner_anim, &QVariantAnimation::valueChanged, this,
                [this](const QVariant& value) {
                    m_banner_alpha = value.toReal();
                    if (m_banner_alpha >= 1.0) {
                        m_banner_from = {};
                    }
                    update();
                });

    }

    /// Whether the filter bar sits at the bottom and blends into the backdrop.
    void SetFilterZoneVisible(bool visible) {
        if (m_filter_zone_visible == visible) {
            return;
        }
        m_filter_zone_visible = visible;
        update();
    }

    /// The game whose banner backs the home screen while the carousel is showing. A game
    /// without one, or an empty id, falls back to the normal background.
    void SetBannerGame(u64 program_id) {
        if (m_banner_id == program_id) {
            return;
        }
        m_banner_id = program_id;
        // Load on the next loop turn, so a slide's first frames are not blocked by the decode.
        if (m_banner_load_queued) {
            return;
        }
        m_banner_load_queued = true;
        QMetaObject::invokeMethod(
            this,
            [this] {
                m_banner_load_queued = false;
                m_banner_from = m_banner_to;
                m_banner_to =
                    HomeBackground::ScaledHero(m_banner_id, size(), devicePixelRatioF());
                m_banner_alpha = m_banner_from.isNull() && m_banner_to.isNull() ? 1.0 : 0.0;
                if (m_banner_alpha < 1.0) {
                    m_banner_anim->stop();
                    m_banner_anim->setStartValue(0.0);
                    m_banner_anim->setEndValue(1.0);
                    m_banner_anim->start();
                }
                update();
            },
            Qt::QueuedConnection);
    }

    /// Re-reads the current game's banner after its artwork file was changed or removed.
    /// SetBannerGame normally ignores a repeat id, so reset it to force a fresh decode.
    void RefreshBanner() {
        const u64 id = m_banner_id;
        m_banner_id = 0;
        SetBannerGame(id);
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        // The banner decodes at a given size, so re-read it at the new one; queued like a
        // slide loads its banner, so the first paint at the new size stretches the old
        // pixmap over the gap instead of blocking on the decode.
        if (m_banner_id != 0 && !size().isEmpty()) {
            RefreshBanner();
        }
    }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        PaintBackdrop(p);
        // A short fade into the panel surface so the filter bar blends into whatever backs it,
        // the same wash the loading screen fades its artwork into.
        if (m_filter_zone_visible) {
            const QColor window = palette().color(QPalette::Window);
            QColor clear = window;
            clear.setAlpha(0);
            QLinearGradient fade(0, height() - BarFadeHeight, 0, height());
            fade.setColorAt(0.0, clear);
            fade.setColorAt(1.0, window);
            p.fillRect(QRect(0, height() - BarFadeHeight, width(), BarFadeHeight), fade);
        }
    }

private:
    /// The backdrop behind everything: the carousel's banner, or the normal background.
    void PaintBackdrop(QPainter& p) {
        // The banner backdrop, when one is set for the centered game: the whole surface, filter
        // bar included, so the backdrop reads as one continuous thing.
        if (UISettings::values.carousel_banner_background.GetValue() &&
            UISettings::values.game_list_mode.GetValue() ==
                Settings::GameListMode::CarouselView) {
            const bool has_from = !m_banner_from.isNull() && m_banner_alpha < 1.0;
            const bool has_to = !m_banner_to.isNull();
            if (has_from || has_to) {
                if (has_from) {
                    if (!has_to) {
                        // The last banner ebbs away over the normal background.
                        p.setOpacity(1.0 - m_banner_alpha);
                    }
                    // A banner decoded at an older, smaller size must still cover the whole surface,
                    // or the uncovered strip reads as a grey panel.
                    p.drawPixmap(rect(), m_banner_from);
                    p.setOpacity(1.0);
                }
                if (has_to && m_banner_alpha > 0.0) {
                    p.setOpacity(m_banner_alpha);
                    p.drawPixmap(rect(), m_banner_to);
                    p.setOpacity(1.0);
                }
                // The same readability scrim the picture path applies.
                QColor scrim = palette().color(QPalette::Window);
                scrim.setAlpha(90);
                p.fillRect(rect(), scrim);
                return;
            }
        }

        // A solid or gradient fill paints directly; only a picture needs the scaled cover.
        const auto kind = background->GetKind();
        if (kind != HomeBackground::Kind::Picture) {
            if (kind == HomeBackground::Kind::Gradient) {
                QLinearGradient gradient(rect().topLeft(), rect().bottomRight());
                gradient.setColorAt(0.0, background->Color());
                gradient.setColorAt(1.0, background->Color2());
                p.fillRect(rect(), gradient);
            } else {
                p.fillRect(rect(), background->Color());
            }
            return;
        }
        if (background->IsEmpty()) {
            return;
        }
        const QImage& frame = background->Frame();
        const qreal dpr = devicePixelRatioF();
        if (cached.isNull() || cached_key != frame.cacheKey() || cached_size != size() ||
            cached.devicePixelRatio() != dpr) {
            cached = QPixmap(size() * dpr);
            cached.setDevicePixelRatio(dpr);
            QPainter cp(&cached);
            cp.setRenderHint(QPainter::SmoothPixmapTransform);
            HomeBackground::PaintCover(cp, rect(), frame);
            // Keep game names and the tree's columns readable on bright or busy backgrounds.
            QColor scrim = palette().color(QPalette::Window);
            scrim.setAlpha(90);
            cp.fillRect(rect(), scrim);
            cached_key = frame.cacheKey();
            cached_size = size();
        }
        p.drawPixmap(0, 0, cached);
    }

    void showEvent(QShowEvent* event) override {
        background->SetActive(true);
        QWidget::showEvent(event);
    }

    void hideEvent(QHideEvent* event) override {
        background->SetActive(false);
        QWidget::hideEvent(event);
    }

private:
    HomeBackground* background;
    QPixmap cached;
    qint64 cached_key = 0;
    QSize cached_size;
    /// The banner backdrop crossfade, and how far it has run.
    QVariantAnimation* m_banner_anim = nullptr;
    QPixmap m_banner_from;
    QPixmap m_banner_to;
    qreal m_banner_alpha = 1.0;
    u64 m_banner_id = 0;
    bool m_banner_load_queued = false;
    bool m_filter_zone_visible = true;
};

GameList::GameList(FileSys::VirtualFilesystem vfs_, FileSys::ManualContentProvider* provider_,
                   PlayTime::PlayTimeManager& play_time_manager_, Core::System& system_,
                   MainWindow* parent)
    : QWidget{parent}, vfs{std::move(vfs_)}, provider{provider_},
      play_time_manager{play_time_manager_}, system{system_} {

    this->main_window = parent;


    outer_layout = new QHBoxLayout(this);
    outer_layout->setContentsMargins(0, 0, 0, 0);
    outer_layout->setSpacing(0);

    background = new HomeBackground(this);
    surface = new HomeSurface(background, this);
    outer_layout->addWidget(surface, 1);

    layout = new QVBoxLayout(surface);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    item_model = new GameListModel(vfs, provider, play_time_manager, system, this);

    SetupViews();

    search_field = new GameListSearchField(this);
    layout->addWidget(search_field);

    controller_navigation = new ControllerNavigation(system.HIDCore(), this);

    SetupScrollAnimation();
    SetupCustomization();
    SetupGameDetails();

    // A click opens the details sidebar; double-click, Enter and controller A start the game.
    // Enter is handled in eventFilter, since `activated` also fires on a single click on
    // platforms that activate items that way.
    for (QAbstractItemView* view :
         {static_cast<QAbstractItemView*>(tree_view), static_cast<QAbstractItemView*>(grid_view),
          static_cast<QAbstractItemView*>(carousel_view)}) {
        connect(view, &QAbstractItemView::doubleClicked, this, &GameList::ValidateEntry);
        connect(view, &QAbstractItemView::clicked, this, &GameList::OnItemClicked);
        connect(view, &QWidget::customContextMenuRequested, this, &GameList::PopupContextMenu);
        // Wait for the selection to settle so holding an arrow key doesn't load every game.
        connect(view->selectionModel(), &QItemSelectionModel::currentChanged, this, [this] {
            if (details_panel->isVisible() && details_panel->maximumWidth() > 0) {
                details_timer->start();
            }
        });
        view->installEventFilter(this);
    }

    connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent, this,
            [this](Qt::Key key) {
                if (system.IsPoweredOn()) {
                    return;
                }
                if (!this->isActiveWindow()) {
                    return;
                }
                QKeyEvent* event = new QKeyEvent(QEvent::KeyPress, key, Qt::NoModifier);
                QCoreApplication::postEvent(m_currentView, event);
            });

    connect(item_model, &GameListModel::PopulatingCompleted, this,
            &GameList::OnPopulatingCompleted);

    connect(item_model, &GameListModel::ShowList, this, &GameList::ShowList);
    connect(item_model, &GameListModel::SaveConfig, this, &GameList::SaveConfig);
    connect(item_model, &GameListModel::PopulatingStarted, this, &GameList::OnPopulate);

    // TODO: impl on grid/carousel
    connect(tree_view, &GameTree::FilterResultReady, search_field,
            [this](int visible, int total) { search_field->setFilterResult(visible, total); });

    qRegisterMetaType<QList<QStandardItem*>>("QList<QStandardItem*>");

    ResetViewMode();
}

void GameList::SetNupConversionRunning(bool running) {
    nup_conversion_running = running;
}

GameList::~GameList() {
    UnloadController();
}

void GameList::SetupViews() {
    tree_view = new GameTree(this);
    grid_view = new GameGrid(this);
    carousel_view = new GameCarousel(this);

    tree_view->SetModel(item_model);
    grid_view->SetModel(item_model);
    carousel_view->SetModel(item_model);
}

QString GameList::GetLastFilterResultItem() const {
    return tree_view->GetLastFilterResultItem();
}

void GameList::ClearFilter() {
    search_field->clear();
}

void GameList::SetFilterFocus() {
    if (item_model->rowCount() > 0) {
        search_field->setFocus();
    }
}

void GameList::SetFilterVisible(bool visibility) {
    search_field->setVisible(visibility);
    surface->SetFilterZoneVisible(visibility);
}

bool GameList::IsEmpty() const {
    return item_model->IsEmpty();
}

void GameList::OnPopulate() {
    m_populating = true;
    m_currentView->setEnabled(false);

    switch (game_list_mode) {
    case Settings::GameListMode::TreeView:
        tree_view->UpdateColumnVisibility(item_model);
        break;
    case Settings::GameListMode::GridView:
        grid_view->UpdateIconSize();
        break;
    case Settings::GameListMode::CarouselView:
        carousel_view->UpdateIconSize();
        break;
    }
}

void GameList::PopulateAsync(QVector<UISettings::GameDir>& game_dirs) {
    item_model->PopulateAsync(game_dirs);
}

void GameList::SetScanEnabled(bool enabled) {
    item_model->SetScanEnabled(enabled);
}

void GameList::SaveInterfaceLayout() {
    tree_view->SaveInterfaceLayout();
}

void GameList::LoadInterfaceLayout() {
    tree_view->LoadInterfaceLayout();
}

QStandardItemModel* GameList::GetModel() const {
    return item_model;
}

void GameList::UnloadController() {
    controller_navigation->UnloadController();
}

void GameList::ResetViewMode() {
    const auto mode = UISettings::values.game_list_mode.GetValue();
    game_list_mode = mode;

    if (m_currentView)
        layout->removeWidget(m_currentView);

    bool newTreeMode = false;

    switch (mode) {
    case Settings::GameListMode::TreeView:
        m_currentView = tree_view;
        newTreeMode = true;

        break;
    case Settings::GameListMode::GridView:
        m_currentView = grid_view;
        newTreeMode = false;

        break;
    case Settings::GameListMode::CarouselView:
        m_currentView = carousel_view;
        newTreeMode = false;

        break;
    default:
        UNREACHABLE();
    }

    tree_view->setVisible(false);
    grid_view->setVisible(false);
    carousel_view->setVisible(false);

    tree_view->setEnabled(false);
    grid_view->setEnabled(false);
    carousel_view->setEnabled(false);

    m_currentView->setVisible(true);
    // MainWindow switches the view right after starting the first populate; keep it disabled
    // until that finishes, as OnPopulate does for every other populate.
    m_currentView->setEnabled(!m_populating);
    layout->insertWidget(0, m_currentView);

    auto view = m_currentView->viewport();
    view->installEventFilter(this);

    view->grabGesture(Qt::SwipeGesture);
    view->grabGesture(Qt::PanGesture);

    QScroller::grabGesture(view, QScroller::LeftMouseButtonGesture);

    // The banner backdrop belongs to the carousel; leaving the mode must repaint it away.
    surface->update();

    auto scroller = QScroller::scroller(view);
    QScrollerProperties props;
    props.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
                          QScrollerProperties::OvershootAlwaysOff);
    props.setScrollMetric(QScrollerProperties::VerticalOvershootPolicy,
                          QScrollerProperties::OvershootAlwaysOff);
    scroller->setScrollerProperties(props);

    if (m_isTreeMode != newTreeMode) {
        m_isTreeMode = newTreeMode;
        item_model->SetFlat(!m_isTreeMode);
        if (!UISettings::values.game_dirs.empty()) {
            item_model->PopulateAsync(UISettings::values.game_dirs);
        }
    }

    if (customize_panel) {
        customize_panel->SyncFromSettings();
    }
}

void GameList::SetupCustomization() {
    customize_panel = new HomeCustomizePanel(background, this);
    customize_panel->setMaximumWidth(0);
    customize_panel->hide();
    outer_layout->addWidget(customize_panel);

    customize_animation = MakeSlideAnimation(customize_panel);

    connect(customize_panel, &HomeCustomizePanel::CloseRequested, this,
            [this] { ShowCustomizePanel(false); });
    connect(customize_panel, &HomeCustomizePanel::BackgroundChosen, this,
            &GameList::SetHomeBackground);
    connect(customize_panel, &HomeCustomizePanel::RecentBackgroundRemoved, this,
            &GameList::RemoveRecentBackground);

    // Applies the saved background: the saved picture file for the picture kind, or the saved
    // colors for a solid or gradient fill.
    const auto saved_color = [](const Setting<std::string>& setting) {
        return HomeBackground::ColorFromString(QString::fromStdString(setting.GetValue()));
    };
    const auto apply_background = [this, saved_color](HomeBackground::Kind kind) {
        switch (kind) {
        case HomeBackground::Kind::Solid:
            background->SetSolid(saved_color(UISettings::values.home_background_color));
            break;
        case HomeBackground::Kind::Gradient:
            background->SetGradient(saved_color(UISettings::values.home_background_color),
                                     saved_color(UISettings::values.home_background_color2));
            break;
        case HomeBackground::Kind::Picture: {
            const QString saved =
                QString::fromStdString(UISettings::values.home_background.GetValue());
            if (!saved.isEmpty() && !background->SetSource(saved)) {
                LOG_WARNING(Frontend, "Home screen background {} could not be loaded",
                            saved.toStdString());
            }
            break;
        }
        }
        UpdateViewTransparency();
    };
    connect(customize_panel, &HomeCustomizePanel::BackgroundKindRequested, this,
            [this, apply_background](HomeBackground::Kind kind) {
                UISettings::values.home_background_kind = static_cast<u8>(kind);
                apply_background(kind);
                customize_panel->SyncFromSettings();
                emit SaveConfig();
            });
    connect(customize_panel, &HomeCustomizePanel::BackgroundColorRequested, this,
            [this, apply_background](int index, const QColor& color) {
                auto& setting = index == 1 ? UISettings::values.home_background_color2
                                           : UISettings::values.home_background_color;
                setting = color.name().toStdString();
                apply_background(HomeBackground::KindFromInt(
                    UISettings::values.home_background_kind.GetValue()));
                customize_panel->SyncFromSettings();
                emit SaveConfig();
            });
    connect(customize_panel, &HomeCustomizePanel::GameListModeRequested, this,
            &GameList::GameListModeRequested);
    connect(customize_panel, &HomeCustomizePanel::GameIconSizeRequested, this,
            &GameList::GameIconSizeRequested);
    connect(customize_panel, &HomeCustomizePanel::GameIconBordersRequested, this,
            [this](bool show) {
                UISettings::values.game_icon_borders = show;
                grid_view->UpdateIconSize();
                carousel_view->viewport()->update();
            });
    connect(customize_panel, &HomeCustomizePanel::CartridgeKindRequested, this,
            [this](u8 kind) {
                UISettings::values.game_cartridge_kind = kind;
                grid_view->UpdateIconSize();
                carousel_view->viewport()->update();
                emit SaveConfig();
            });
    connect(customize_panel, &HomeCustomizePanel::CarouselCardSizeRequested, this,
            [this](u8 index) {
                UISettings::values.carousel_card_size = index;
                carousel_view->viewport()->update();
                emit SaveConfig();
            });
    connect(customize_panel, &HomeCustomizePanel::CarouselSideCountRequested, this,
            [this](u8 count) {
                UISettings::values.carousel_side_count = count;
                carousel_view->viewport()->update();
                emit SaveConfig();
            });
    connect(customize_panel, &HomeCustomizePanel::CarouselBannerBackgroundRequested, this,
            [this](bool show) {
                UISettings::values.carousel_banner_background = show;
                surface->SetBannerGame(carousel_view->CenteredProgramId());
                carousel_view->viewport()->update();
                emit SaveConfig();
            });
    // The backdrop follows the carousel's centered game, including a slide's destination.
    connect(carousel_view, &GameCarousel::CenteredGameChanged, surface,
            &HomeSurface::SetBannerGame);

    customize_button = new FloatingActionButton(surface);
    customize_button->setToolTip(tr("Customize Home Screen"));
    RefreshCustomizeButtonIcon();
    connect(customize_button, &QAbstractButton::clicked, this, [this] {
        ShowCustomizePanel(!(customize_panel->isVisible() && customize_panel->maximumWidth() > 0));
    });
    surface->installEventFilter(this);
    search_field->installEventFilter(this);
    PlaceCustomizeButton();

    // Restore the saved background kind, and with it the saved picture when it is the kind.
    apply_background(
        HomeBackground::KindFromInt(UISettings::values.home_background_kind.GetValue()));
}

void GameList::PlaceCustomizeButton() {
    constexpr int Margin = 20;
    const int bottom = search_field->isVisible() ? search_field->geometry().top() : surface->height();
    customize_button->move(surface->width() - customize_button->width() - Margin,
                           bottom - customize_button->height() - Margin / 2);
    customize_button->raise();
}

void GameList::RefreshCustomizeButtonIcon() {
    customize_button->setIcon(HomeGlyphs::Brush(palette().color(QPalette::HighlightedText),
                                                devicePixelRatioF(), 24));
}

QPropertyAnimation* GameList::MakeSlideAnimation(QWidget* panel) {
    auto* animation = new QPropertyAnimation(panel, "maximumWidth", this);
    animation->setDuration(180);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::valueChanged, this, [this] {
        if (panels_overlay) {
            UpdatePanelPlacement();
        }
    });
    connect(animation, &QPropertyAnimation::finished, this, [this, panel] {
        if (panel->maximumWidth() == 0) {
            const bool had_focus = panel->isAncestorOf(QApplication::focusWidget());
            panel->hide();
            if (had_focus) {
                m_currentView->setFocus();
            }
        }
    });
    return animation;
}

void GameList::UpdatePanelPlacement() {
    // Below this width, docking a sidebar would leave too little room for the games.
    constexpr int OverlayBelowWidth = 900;
    const bool overlay = width() < OverlayBelowWidth;
    if (overlay != panels_overlay) {
        panels_overlay = overlay;
        for (QWidget* panel : {static_cast<QWidget*>(customize_panel),
                               static_cast<QWidget*>(details_panel)}) {
            if (overlay) {
                outer_layout->removeWidget(panel);
            } else {
                outer_layout->addWidget(panel);
            }
        }
    }
    if (!panels_overlay) {
        return;
    }
    for (QWidget* panel :
         {static_cast<QWidget*>(customize_panel), static_cast<QWidget*>(details_panel)}) {
        if (panel->isVisible()) {
            const int panel_width = std::min(panel->maximumWidth(), HomeSidePanel::PanelWidth);
            panel->setGeometry(width() - panel_width, 0, panel_width, height());
            panel->raise();
        }
    }
}

void GameList::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (customize_panel && details_panel) {
        UpdatePanelPlacement();
    }
}

void GameList::SlidePanel(QWidget* panel, QPropertyAnimation* animation, int width) {
    if (width > 0) {
        panel->show();
        UpdatePanelPlacement();
    } else if (panel->isHidden()) {
        return;
    }
    animation->stop();
    animation->setStartValue(panel->maximumWidth());
    animation->setEndValue(width);
    animation->start();
}

void GameList::ShowCustomizePanel(bool show) {
    if (show) {
        HideGameDetails();
        customize_panel->SyncFromSettings();
        customize_panel->show();
        customize_panel->setFocus();
    }
    SlidePanel(customize_panel, customize_animation, show ? HomeCustomizePanel::PanelWidth : 0);
}

void GameList::SetupGameDetails() {
    details_panel = new GameDetailsPanel(this);
    details_panel->setMaximumWidth(0);
    details_panel->hide();
    outer_layout->addWidget(details_panel);
    details_animation = MakeSlideAnimation(details_panel);

    details_timer = new QTimer(this);
    details_timer->setSingleShot(true);
    details_timer->setInterval(120);
    connect(details_timer, &QTimer::timeout, this, [this] {
        const QModelIndex current = m_currentView->currentIndex();
        if (current.isValid() && current.sibling(current.row(), 0) != details_index &&
            details_panel->isVisible() && details_panel->maximumWidth() > 0) {
            ShowGameDetails(current);
        }
    });
    details_pool = new QThreadPool(this);
    details_pool->setMaxThreadCount(1);

    connect(details_panel, &GameDetailsPanel::CloseRequested, this, &GameList::HideGameDetails);
    connect(details_panel, &GameDetailsPanel::PlayRequested, this, [this] {
        if (details_index.isValid()) {
            ValidateEntry(details_index);
        }
    });
    connect(details_panel, &GameDetailsPanel::ConfigureRequested, this, [this] {
        emit OpenPerGameGeneralRequested(details_panel->Game().path.toStdString());
        // The dialog is modal: refresh the custom settings and add-on state it may have changed.
        if (details_index.isValid()) {
            LoadGameDetailsExtras();
        }
    });
    connect(details_panel, &GameDetailsPanel::OpenLocationRequested, this, [this] {
        emit OpenDirectory(QFileInfo(details_panel->Game().path).absolutePath());
    });
    connect(details_panel, &GameDetailsPanel::PrecompileShadersRequested, this, [this] {
        const GameDetailsInfo& game = details_panel->Game();
        emit PrepareShaderWarmRequested(game.program_id, game.path.toStdString());
        if (details_index.isValid()) {
            LoadGameDetailsExtras();
        }
    });
    connect(details_panel, &GameDetailsPanel::DisabledAddOnsChanged, this,
            [this](const QStringList& disabled) {
                const u64 program_id = details_panel->Game().program_id;
                if (program_id == 0) {
                    return;
                }
                std::vector<std::string> list;
                for (const QString& key : disabled) {
                    list.push_back(key.toStdString());
                }
                Settings::values.disabled_addons[program_id] = std::move(list);
                Common::FS::RemoveFile(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) /
                                       "game_list" / fmt::format("{:016X}.pv.txt", program_id));
                add_ons_changed = true;
                emit SaveConfig();
            });
    connect(details_panel, &GameDetailsPanel::FavoriteToggled, this,
            [this] { ToggleFavorite(details_panel->Game().program_id); });
    connect(details_panel, &GameDetailsPanel::MoreRequested, this, [this](const QPoint& pos) {
        const GameDetailsInfo& game = details_panel->Game();
        QMenu menu;
        AddGamePopup(menu, game.program_id, game.path.toStdString());
        menu.exec(pos);
    });
    connect(details_panel, &GameDetailsPanel::IconChanged, this, &GameList::UpdateGameIcon);
    // A saved or removed banner changes the backdrop pixels without changing the centered game.
    connect(details_panel, &GameDetailsPanel::HeroChanged, this,
            [this](u64) { surface->RefreshBanner(); });
}

void GameList::ShowGameDetailsForPath(const QString& path) {
    pending_details_path = path;
    RevealPendingGame();
}

QModelIndex GameList::FindGame(const QString& path) const {
    // Loose dumps boot <dir>/main while the list holds <dir>.
    const auto matches = [&](const QString& item_path) {
        return !item_path.isEmpty() &&
               (item_path == path || path.startsWith(item_path + QLatin1Char('/')) ||
                path.startsWith(item_path + QLatin1Char('\\')));
    };
    QModelIndex favorite_match;
    const std::function<QModelIndex(QStandardItem*, bool)> visit =
        [&](QStandardItem* parent, bool in_favorites) -> QModelIndex {
        for (int row = 0; row < parent->rowCount(); ++row) {
            QStandardItem* child = parent->child(row, 0);
            if (!child) {
                continue;
            }
            const auto type = child->data(GameListItem::TypeRole).value<GameListItemType>();
            if (type == GameListItemType::Game &&
                matches(child->data(GameListItemPath::FullPathRole).toString())) {
                // Prefer the row under its directory over the copy under Favorites.
                if (!in_favorites) {
                    return child->index();
                }
                if (!favorite_match.isValid()) {
                    favorite_match = child->index();
                }
            }
            if (const QModelIndex found =
                    visit(child, in_favorites || type == GameListItemType::Favorites);
                found.isValid()) {
                return found;
            }
        }
        return {};
    };
    const QModelIndex found = visit(item_model->invisibleRootItem(), false);
    return found.isValid() ? found : favorite_match;
}

bool GameList::RevealPendingGame() {
    if (pending_details_path.isEmpty()) {
        return false;
    }
    const QModelIndex index = FindGame(pending_details_path);
    if (!index.isValid()) {
        return false;
    }
    m_currentView->setCurrentIndex(index);
    m_currentView->scrollTo(index);
    ShowGameDetails(index);
    return true;
}

void GameList::OnItemClicked(const QModelIndex& index) {
    pending_details_path.clear();
    const auto type = index.sibling(index.row(), 0).data(GameListItem::TypeRole);
    if (type.value<GameListItemType>() == GameListItemType::Game) {
        ShowGameDetails(index);
    }
}

void GameList::ShowGameDetails(const QModelIndex& index) {
    const QModelIndex selected = index.sibling(index.row(), 0);
    if (selected.data(GameListItem::TypeRole).value<GameListItemType>() != GameListItemType::Game) {
        return;
    }
    const auto column_text = [&](int column) {
        return selected.sibling(selected.row(), column).data(Qt::DisplayRole).toString().trimmed();
    };

    GameDetailsInfo info;
    info.path = selected.data(GameListItemPath::FullPathRole).toString();
    info.program_id = selected.data(GameListItemPath::ProgramIdRole).toULongLong();
    info.title = selected.data(GameListItemPath::TitleRole).toString().trimmed();
    if (info.title.isEmpty()) {
        info.title = QFileInfo(info.path).completeBaseName();
    }
    info.developer = selected.data(GameListItemPath::DeveloperRole).toString().trimmed();
    info.version = selected.data(GameListItemPath::VersionRole).toString();
    info.file_type = column_text(GameListModel::COLUMN_FILE_TYPE);
    info.size = column_text(GameListModel::COLUMN_SIZE);
    info.play_time = column_text(GameListModel::COLUMN_PLAY_TIME);
    info.favorite = UISettings::values.favorited_ids.contains(info.program_id);
    // Prefer the full-size custom icon; list icons are scaled down to the icon size setting.
    if (QtCommon::GameArt::HasIcon(info.program_id)) {
        info.icon.load(QtCommon::GameArt::IconPath(info.program_id));
    }
    if (info.icon.isNull()) {
        info.icon = selected.data(Qt::DecorationRole).value<QPixmap>();
    }

    details_index = selected;
    details_panel->SetGame(info);
    LoadGameDetailsExtras();
    if (customize_panel->isVisible()) {
        ShowCustomizePanel(false);
    }
    SlidePanel(details_panel, details_animation, GameDetailsPanel::PanelWidth);
}

void GameList::LoadGameDetailsExtras() {
    const GameDetailsInfo& game = details_panel->Game();
    const u64 program_id = game.program_id;
    const QString path = game.path;
    if (program_id == 0) {
        details_panel->SetExtras(GameDetailsExtras{.program_id = program_id, .path = path});
        return;
    }

    std::vector<std::string> disabled;
    if (const auto it = Settings::values.disabled_addons.find(program_id);
        it != Settings::values.disabled_addons.end()) {
        disabled = it->second;
    }

    details_pool->start([guard = QPointer<GameList>(this), vfs_ = vfs, &system_ = system,
                         program_id, path, disabled = std::move(disabled)] {
        GameDetailsExtras extras{.program_id = program_id, .path = path};

        FileSys::VirtualFile update_raw;
        if (const auto file = Core::GetGameFileFromPath(vfs_, path.toStdString())) {
            if (const auto loader = Loader::GetLoader(system_, file)) {
                loader->ReadUpdateRaw(update_raw);
            }
        }
        const FileSys::PatchManager patch_manager{program_id, system_.GetFileSystemController(),
                                                  system_.GetContentProvider()};
        // Same rules as the per-game Add-Ons tab: one update at most is enabled.
        bool has_enabled_update = false;
        for (const auto& patch : patch_manager.GetPatches(update_raw)) {
            GameAddOn add_on;
            add_on.name = QString::fromStdString(patch.name);
            add_on.version = QString::fromStdString(patch.version);
            add_on.is_update = patch.type == FileSys::PatchType::Update;
            const bool external_update = add_on.is_update &&
                                         patch.source == FileSys::PatchSource::External &&
                                         patch.numeric_version != 0;
            add_on.key = external_update
                             ? QStringLiteral("Update@%1").arg(patch.numeric_version)
                             : add_on.name;
            add_on.enabled = std::find(disabled.begin(), disabled.end(),
                                       add_on.key.toStdString()) == disabled.end();
            if (add_on.is_update && add_on.enabled) {
                add_on.enabled = !has_enabled_update;
                has_enabled_update = true;
            }
            extras.add_ons.push_back(std::move(add_on));
        }

        std::error_code ec;
        const auto cache_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
                               fmt::format("{:016x}", program_id);
        for (auto it = std::filesystem::recursive_directory_iterator(cache_dir, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec)) {
                const auto size = it->file_size(ec);
                if (!ec) {
                    extras.pipeline_cache_bytes += size;
                }
            }
        }
        extras.shaders_precompiled = ShaderWarmFilesExist(program_id);
        extras.custom_config = Common::FS::Exists(
            Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "custom" /
            fmt::format("{:016X}.ini", program_id));

        QMetaObject::invokeMethod(qApp, [guard, extras = std::move(extras)] {
            if (guard) {
                guard->details_panel->SetExtras(extras);
            }
        });
    });
}

void GameList::HideGameDetails() {
    pending_details_path.clear();
    details_index = QPersistentModelIndex{};
    details_timer->stop();
    SlidePanel(details_panel, details_animation, 0);
    // The Add-ons column only picks up toggles made in the sidebar on a rescan.
    if (add_ons_changed) {
        add_ons_changed = false;
        RefreshGameDirectory();
    }
}

void GameList::UpdateGameIcon(u64 program_id) {
    const u32 size = UISettings::values.game_icon_size.GetValue();
    QPixmap custom;
    if (QtCommon::GameArt::HasIcon(program_id)) {
        custom.load(QtCommon::GameArt::IconPath(program_id));
        if (!custom.isNull()) {
            custom = custom.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
    }

    // The same game can appear under its directory and under Favorites.
    const std::function<void(QStandardItem*)> visit = [&](QStandardItem* parent) {
        for (int row = 0; row < parent->rowCount(); ++row) {
            QStandardItem* child = parent->child(row, 0);
            if (!child) {
                continue;
            }
            if (child->type() == static_cast<int>(GameListItemType::Game) &&
                child->data(GameListItemPath::ProgramIdRole).toULongLong() == program_id) {
                child->setData(custom.isNull() ? child->data(GameListItemPath::OriginalIconRole)
                                               : QVariant(custom),
                               Qt::DecorationRole);
            }
            visit(child);
        }
    };
    visit(item_model->invisibleRootItem());

    if (details_index.isValid() && details_panel->Game().program_id == program_id) {
        QPixmap icon;
        if (QtCommon::GameArt::HasIcon(program_id)) {
            icon.load(QtCommon::GameArt::IconPath(program_id));
        }
        if (icon.isNull()) {
            icon = details_index.data(Qt::DecorationRole).value<QPixmap>();
        }
        details_panel->SetIcon(icon);
    }
}

void GameList::SetHomeBackground(const QString& path) {
    if (!background->SetSource(path)) {
        QMessageBox::warning(this, tr("Home Screen"),
                             HomeBackground::IsVideo(path)
                                 ? tr("Video backgrounds need a build with QtMultimedia.")
                                 : tr("\"%1\" could not be loaded as a background.")
                                       .arg(QFileInfo(path).fileName()));
        return;
    }

    UISettings::values.home_background.SetValue(path.toStdString());
    if (!path.isEmpty()) {
        constexpr qsizetype MaxRecentBackgrounds = 8;
        auto& recents = UISettings::values.recent_home_backgrounds;
        recents.removeAll(path);
        recents.prepend(path);
        while (recents.size() > MaxRecentBackgrounds) {
            recents.removeLast();
        }
    }

    UpdateViewTransparency();
    customize_panel->SyncFromSettings();
    emit SaveConfig();
}

void GameList::RemoveRecentBackground(const QString& path) {
    UISettings::values.recent_home_backgrounds.removeAll(path);
    customize_panel->SyncFromSettings();
    emit SaveConfig();
}

void GameList::UpdateViewTransparency() {
    if (background->IsEmpty()) {
        tree_view->setStyleSheet(QStringLiteral("QTreeView{ border: none; }"));
        grid_view->setStyleSheet({});
        carousel_view->setStyleSheet({});
        return;
    }

    QColor alternate = palette().color(QPalette::AlternateBase);
    alternate.setAlpha(110);
    tree_view->setStyleSheet(
        QStringLiteral("QTreeView{ border: none; background: transparent; "
                       "alternate-background-color: rgba(%1, %2, %3, %4); }")
            .arg(alternate.red())
            .arg(alternate.green())
            .arg(alternate.blue())
            .arg(alternate.alpha()));
    const QString list_style =
        QStringLiteral("QListView{ border: none; background: transparent; }");
    grid_view->setStyleSheet(list_style);
    carousel_view->setStyleSheet(list_style);
}

void GameList::OnTextChanged(const QString& new_text) {
    const QString edit_filter_text = new_text.toLower();

    if (m_isTreeMode) {
        tree_view->ApplyFilter(edit_filter_text, item_model);
    } else {
        grid_view->ApplyFilter(edit_filter_text, item_model);
    }
}

void GameList::OnFilterCloseClicked() {
    main_window->filterBarSetChecked(false);
}

void GameList::OnPopulatingCompleted(const QStringList& watch_list) {
    emit ShowList(!item_model->IsEmpty());

    // favorites row
    if (m_isTreeMode) {
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(),
                                UISettings::values.favorited_ids.size() == 0);
        tree_view->setExpanded(item_model->invisibleRootItem()->child(0)->index(),
                               UISettings::values.favorites_expanded.GetValue());
    }

    // restore collapsed/expanded flags
    if (m_isTreeMode) {
        const auto* root = item_model->invisibleRootItem();
        for (int i = 1; i < root->rowCount() - 1; ++i) {
            const auto* dir_item = root->child(i);
            const auto type = dir_item->data(GameListItem::TypeRole).value<GameListItemType>();
            if (type == GameListItemType::CustomDir || type == GameListItemType::SdmcDir ||
                type == GameListItemType::UserNandDir || type == GameListItemType::SysNandDir) {
                const int dir_index = dir_item->data(GameListDir::GameDirRole).toInt();
                if (dir_index >= 0 && dir_index < UISettings::values.game_dirs.size()) {
                    tree_view->setExpanded(dir_item->index(),
                                           UISettings::values.game_dirs[dir_index].expanded);
                }
            }
        }
    }

    // Watcher updates
    auto* watcher = item_model->GetWatcher();
    auto current_watch_list = watcher->directories();

    constexpr qsizetype LIMIT_WATCH_DIRECTORIES = 5000;
    constexpr int SLICE_SIZE = 25;

    QStringList to_remove, to_add;

    const auto slice = [&](const QStringList& list,
                           std::function<void(const QStringList&)> callback) {
        const int len = (std::min)(list.size(), LIMIT_WATCH_DIRECTORIES);
        for (int i = 0; i < len; i += SLICE_SIZE) {
            auto chunk = list.mid(i, SLICE_SIZE);
            if (!chunk.isEmpty()) {
                callback(chunk);
            }
        }
    };

    // remove any paths not in the new watch list
    for (const auto& path : std::as_const(current_watch_list)) {
        if (!watch_list.contains(path)) {
            to_remove.emplaceBack(path);
        }
    }

    slice(to_remove, [watcher](const QStringList& chunk) { watcher->removePaths(chunk); });

    // add any paths not in the old watch list
    for (const auto& path : std::as_const(watch_list)) {
        if (!current_watch_list.contains(path)) {
            to_add.emplaceBack(path);
        }
    }

    slice(to_add, [watcher](const QStringList& chunk) { watcher->addPaths(chunk); });

    m_populating = false;
    m_currentView->setEnabled(true);

    // A repopulate replaces every row: reopen the game that was asked for, or close the
    // sidebar since the game it showed is gone.
    if (RevealPendingGame()) {
        pending_details_path.clear();
    } else if (details_panel->isVisible() && !details_index.isValid()) {
        HideGameDetails();
    }

    int children_total = 0;
    for (int i = 1; i < item_model->rowCount() - 1; ++i) {
        children_total += item_model->item(i, 0)->rowCount();
    }

    search_field->setFilterResult(children_total, children_total);
    if (children_total > 0) {
        search_field->setFocus();
    }

    // TODO: carousel/grid impl.
    item_model->sort(tree_view->header()->sortIndicatorSection(),
                     tree_view->header()->sortIndicatorOrder());

    // A repopulate (a directory removed, deep scan toggled) replaces the rows: apply the search
    // text the user still has typed to them.
    if (!search_field->filterText().isEmpty()) {
        OnTextChanged(search_field->filterText());
    }

    emit PopulatingCompleted();
}

void GameList::RefreshGameDirectory() {
    item_model->RefreshGameDirectory();
}

void GameList::RefreshExternalContent() {
    item_model->RefreshExternalContent();
}

void GameList::UpdateIconSizes() {
    if (customize_panel) {
        customize_panel->SyncFromSettings();
    }

    switch (game_list_mode) {
    case Settings::GameListMode::GridView:
        grid_view->UpdateIconSize();
        break;
    case Settings::GameListMode::CarouselView:
        carousel_view->UpdateIconSize();
        break;
    case Settings::GameListMode::TreeView:
        break;
    }
}

void GameList::ValidateEntry(const QModelIndex& item) {
    const auto selected = item.sibling(item.row(), 0);

    switch (selected.data(GameListItem::TypeRole).value<GameListItemType>()) {
    case GameListItemType::Game: {
        const QString file_path = selected.data(GameListItemPath::FullPathRole).toString();
        if (file_path.isEmpty())
            return;
        const QFileInfo file_info(file_path);
        if (!file_info.exists())
            return;

        if (file_info.isDir()) {
            const QDir dir{file_path};
            const QStringList matching_main = dir.entryList({QStringLiteral("main")}, QDir::Files);
            if (matching_main.size() == 1) {
                emit GameChosen(dir.path() + QDir::separator() + matching_main[0]);
            }
            return;
        }

        const auto title_id = selected.data(GameListItemPath::ProgramIdRole).toULongLong();

        search_field->clear();
        emit GameChosen(file_path, title_id);
        break;
    }
    case GameListItemType::AddDir:
        emit AddDirectory();
        break;
    default:
        break;
    }
}

void GameList::ToggleFavorite(u64 program_id) {
    item_model->ToggleFavorite(program_id);
    if (details_panel && details_panel->Game().program_id == program_id) {
        details_panel->SetFavorite(UISettings::values.favorited_ids.contains(program_id));
    }

    if (UISettings::values.favorited_ids.contains(program_id)) {
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(),
                                !search_field->filterText().isEmpty());
        item_model->sort(tree_view->header()->sortIndicatorSection(),
                         tree_view->header()->sortIndicatorOrder());
    } else {
        if (UISettings::values.favorited_ids.size() == 0) {
            tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(), true);
        }
    }
}

void GameList::PopupContextMenu(const QPoint& menu_location) {
    QModelIndex item = m_currentView->indexAt(menu_location);
    if (!item.isValid()) {
        QMenu blank_menu;
        if (!m_isTreeMode) {
            QAction* addGameDirAction = blank_menu.addAction(tr("&Add New Game Directory"));
            connect(addGameDirAction, &QAction::triggered, this, &GameList::AddDirectory);
            blank_menu.addSeparator();
        }
        QAction* customize = blank_menu.addAction(tr("&Customize Home Screen..."));
        connect(customize, &QAction::triggered, this, [this] { ShowCustomizePanel(true); });
        blank_menu.exec(m_currentView->viewport()->mapToGlobal(menu_location));
        return;
    }

    const auto selected = item.sibling(item.row(), 0);
    QMenu context_menu;
    switch (selected.data(GameListItem::TypeRole).value<GameListItemType>()) {
    case GameListItemType::Game:
        AddGamePopup(context_menu, selected.data(GameListItemPath::ProgramIdRole).toULongLong(),
                     selected.data(GameListItemPath::FullPathRole).toString().toStdString());
        break;
    case GameListItemType::CustomDir:
        AddPermDirPopup(context_menu, selected);
        AddCustomDirPopup(context_menu, selected);
        break;
    case GameListItemType::SdmcDir:
    case GameListItemType::UserNandDir:
    case GameListItemType::SysNandDir:
        AddPermDirPopup(context_menu, selected);
        break;
    case GameListItemType::Favorites:
        AddFavoritesPopup(context_menu);
        break;
    default:
        break;
    }
    context_menu.exec(m_currentView->viewport()->mapToGlobal(menu_location));
}

void GameList::AddGamePopup(QMenu& context_menu, u64 program_id, const std::string& path) {
    // TODO(crueter): Refactor this and make it less bad
    QAction* favorite = context_menu.addAction(tr("Favorite"));
    context_menu.addSeparator();
    QAction* start_game = context_menu.addAction(tr("Start Game"));
    QAction* start_game_global =
        context_menu.addAction(tr("Start Game without Custom Configuration"));
    context_menu.addSeparator();
    QAction* open_save_location = context_menu.addAction(tr("Open Save Data Location"));
    QAction* open_mod_location = context_menu.addAction(tr("Open Mod Data Location"));
    QAction* open_transferable_shader_cache =
        context_menu.addAction(tr("Open Transferable Pipeline Cache"));
    const bool has_shader_warm = ShaderWarmFilesExist(program_id);
    QAction* prepare_shaders = context_menu.addAction(
        has_shader_warm ? tr("Pre-compile Shaders Again") : tr("Pre-compile Shaders"));
    QAction* delete_shader_warm = nullptr;
    if (has_shader_warm) {
        delete_shader_warm = context_menu.addAction(tr("Delete Pre-compiled Shaders"));
    }
    QAction* ryujinx = context_menu.addAction(tr("Link to Ryujinx"));
    context_menu.addSeparator();
    QMenu* remove_menu = context_menu.addMenu(tr("Remove"));
    QAction* remove_update = remove_menu->addAction(tr("Remove Installed Update"));
    QAction* remove_dlc = remove_menu->addAction(tr("Remove All Installed DLC"));
    QAction* remove_custom_config = remove_menu->addAction(tr("Remove Custom Configuration"));
    QAction* remove_cache_storage = remove_menu->addAction(tr("Remove Cache Storage"));
    QAction* remove_gl_shader_cache = remove_menu->addAction(tr("Remove OpenGL Pipeline Cache"));
    QAction* remove_vk_shader_cache = remove_menu->addAction(tr("Remove Vulkan Pipeline Cache"));
    remove_menu->addSeparator();
    QAction* remove_shader_cache = remove_menu->addAction(tr("Remove All Pipeline Caches"));
    QAction* remove_all_content = remove_menu->addAction(tr("Remove All Installed Contents"));
    QMenu* play_time_menu = context_menu.addMenu(tr("Manage Play Time"));
    QAction* set_play_time = play_time_menu->addAction(tr("Edit Play Time Data"));
    QAction* remove_play_time_data = play_time_menu->addAction(tr("Remove Play Time Data"));
    const bool is_nup_archive =
        !FileSys::Nup::DumpRootFromMain(path) &&
        QString::fromStdString(path).endsWith(QStringLiteral(".nup"), Qt::CaseInsensitive);
    // Music extraction reads the Alto-family Sound tree straight out of a NUP archive or a
    // loose dump; other content types (installed NSPs, XCI) are not supported.
    QAction* extract_music = nullptr;
    if (is_nup_archive || FileSys::Nup::DumpRootFromMain(path)) {
        extract_music = context_menu.addAction(tr("Dump Audio"));
        extract_music->setEnabled(!nup_conversion_running);
    }
    QAction* extract_nup = nullptr;
    if (is_nup_archive) {
        extract_nup = context_menu.addAction(tr("Dump NUP..."));
        extract_nup->setEnabled(!nup_conversion_running);
    }
    QMenu* dump_romfs_menu = context_menu.addMenu(tr("Dump RomFS"));
    QAction* dump_romfs = dump_romfs_menu->addAction(tr("Dump RomFS"));
    QAction* dump_romfs_sdmc = dump_romfs_menu->addAction(tr("Dump RomFS to SDMC"));
    QAction* verify_integrity = context_menu.addAction(tr("Verify Integrity"));
    QAction* copy_tid = context_menu.addAction(tr("Copy Title ID to Clipboard"));
    QAction* convert_nup = nullptr;
    if (FileSys::Nup::DumpRootFromMain(path)) {
        convert_nup = context_menu.addAction(tr("Convert to NUP..."));
        convert_nup->setEnabled(!nup_conversion_running);
    }
#if !defined(__APPLE__)
    QMenu* shortcut_menu = context_menu.addMenu(tr("Create Shortcut"));
    QAction* create_desktop_shortcut = shortcut_menu->addAction(tr("Add to Desktop"));
    QAction* create_applications_menu_shortcut =
        shortcut_menu->addAction(tr("Add to Applications Menu"));
#endif
    context_menu.addSeparator();
    QAction* edit_nup = nullptr;
    if (is_nup_archive) {
        edit_nup = context_menu.addAction(tr("Edit NUP Metadata..."));
        edit_nup->setEnabled(!nup_conversion_running);
    }
    QAction* properties = context_menu.addAction(tr("Configure Game"));
    if (Settings::FindGamePreset(program_id) != nullptr) {
        QAction* reset_settings_prompt =
            context_menu.addAction(tr("Show Recommended Settings Prompt Again"));
        reset_settings_prompt->setToolTip(
            tr("Ask about recommended settings the next time you start this game from the game "
               "list."));
        connect(reset_settings_prompt, &QAction::triggered, this,
                [this, program_id] { emit ResetRecommendedSettingsPromptRequested(program_id); });
    }

    favorite->setVisible(program_id != 0);
    favorite->setCheckable(true);
    favorite->setChecked(UISettings::values.favorited_ids.contains(program_id));
    open_save_location->setVisible(program_id != 0);
    open_mod_location->setVisible(program_id != 0);
    open_transferable_shader_cache->setVisible(program_id != 0);
    prepare_shaders->setVisible(program_id != 0);
    remove_update->setVisible(program_id != 0);
    remove_dlc->setVisible(program_id != 0);
    remove_gl_shader_cache->setVisible(program_id != 0);
    remove_vk_shader_cache->setVisible(program_id != 0);
    remove_shader_cache->setVisible(program_id != 0);
    remove_all_content->setVisible(program_id != 0);

    connect(favorite, &QAction::triggered, this,
            [this, program_id]() { ToggleFavorite(program_id); });
    connect(open_save_location, &QAction::triggered, this, [this, program_id, path]() {
        emit OpenFolderRequested(program_id, GameListOpenTarget::SaveData, path);
    });
    connect(start_game, &QAction::triggered, this,
            [this, path]() { emit BootGame(QString::fromStdString(path), StartGameType::Normal); });
    connect(start_game_global, &QAction::triggered, this,
            [this, path]() { emit BootGame(QString::fromStdString(path), StartGameType::Global); });
    connect(open_mod_location, &QAction::triggered, this, [this, program_id, path]() {
        emit OpenFolderRequested(program_id, GameListOpenTarget::ModData, path);
    });
    connect(open_transferable_shader_cache, &QAction::triggered, this,
            [this, program_id]() { emit OpenTransferableShaderCacheRequested(program_id); });
    connect(prepare_shaders, &QAction::triggered, this,
            [this, program_id, path]() { emit PrepareShaderWarmRequested(program_id, path); });
    if (delete_shader_warm) {
        connect(delete_shader_warm, &QAction::triggered, this,
                [this, program_id]() { emit DeleteShaderWarmRequested(program_id); });
    }
    connect(remove_all_content, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id, QtCommon::Game::InstalledEntryType::Game);
    });
    connect(remove_update, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id, QtCommon::Game::InstalledEntryType::Update);
    });
    connect(remove_dlc, &QAction::triggered, this, [this, program_id]() {
        emit RemoveInstalledEntryRequested(program_id,
                                           QtCommon::Game::InstalledEntryType::AddOnContent);
    });
    connect(remove_gl_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::GlShaderCache,
                                 path);
    });
    connect(remove_vk_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::VkShaderCache,
                                 path);
    });
    connect(remove_shader_cache, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::AllShaderCache,
                                 path);
    });
    connect(remove_custom_config, &QAction::triggered, this, [this, program_id, path]() {
        emit RemoveFileRequested(program_id,
                                 QtCommon::Game::GameListRemoveTarget::CustomConfiguration, path);
    });
    connect(set_play_time, &QAction::triggered, this,
            [this, program_id]() { emit SetPlayTimeRequested(program_id); });
    connect(remove_play_time_data, &QAction::triggered, this,
            [this, program_id]() { emit RemovePlayTimeRequested(program_id); });
    connect(remove_cache_storage, &QAction::triggered, this, [this, program_id, path] {
        emit RemoveFileRequested(program_id, QtCommon::Game::GameListRemoveTarget::CacheStorage,
                                 path);
    });
    connect(dump_romfs, &QAction::triggered, this, [this, program_id, path]() {
        emit DumpRomFSRequested(program_id, path, DumpRomFSTarget::Normal);
    });
    connect(dump_romfs_sdmc, &QAction::triggered, this, [this, program_id, path]() {
        emit DumpRomFSRequested(program_id, path, DumpRomFSTarget::SDMC);
    });
    connect(verify_integrity, &QAction::triggered, this,
            [this, path]() { emit VerifyIntegrityRequested(path); });
    connect(copy_tid, &QAction::triggered, this,
            [this, program_id]() { emit CopyTIDRequested(program_id); });
    if (convert_nup) {
        connect(convert_nup, &QAction::triggered, this,
                [this, path]() { emit ConvertToNupRequested(path); });
    }
    if (extract_nup) {
        connect(extract_nup, &QAction::triggered, this,
                [this, path]() { emit ExtractNupRequested(path); });
    }
    if (extract_music) {
        connect(extract_music, &QAction::triggered, this,
                [this, path]() { emit ExtractMusicRequested(path); });
    }
    if (edit_nup) {
        connect(edit_nup, &QAction::triggered, this,
                [this, path]() { emit EditNupMetadataRequested(path); });
    }
#if !defined(__APPLE__)
    connect(create_desktop_shortcut, &QAction::triggered, this, [this, program_id, path]() {
        emit CreateShortcut(program_id, path, QtCommon::Game::ShortcutTarget::Desktop);
    });
    connect(create_applications_menu_shortcut, &QAction::triggered, this,
            [this, program_id, path]() {
                emit CreateShortcut(program_id, path, QtCommon::Game::ShortcutTarget::Applications);
            });
#endif
    connect(properties, &QAction::triggered, this,
            [this, path]() { emit OpenPerGameGeneralRequested(path); });

    connect(ryujinx, &QAction::triggered, this,
            [this, program_id]() { emit LinkToRyujinxRequested(program_id); });
};

void GameList::AddCustomDirPopup(QMenu& context_menu, QModelIndex selected) {
    UISettings::GameDir& game_dir =
        UISettings::values.game_dirs[selected.data(GameListDir::GameDirRole).toInt()];

    QAction* deep_scan = context_menu.addAction(tr("Scan Subfolders"));
    QAction* delete_dir = context_menu.addAction(tr("Remove Game Directory"));

    deep_scan->setCheckable(true);
    deep_scan->setChecked(game_dir.deep_scan);

    connect(deep_scan, &QAction::triggered, this, [this, &game_dir] {
        game_dir.deep_scan = !game_dir.deep_scan;
        PopulateAsync(UISettings::values.game_dirs);
    });
    connect(delete_dir, &QAction::triggered, this, [this, &game_dir] {
        UISettings::values.game_dirs.removeOne(game_dir);
        // Repopulate instead of removing the row: a running worker adds games under the
        // directory rows it created, and the other rows' GameDirRole indices would shift.
        PopulateAsync(UISettings::values.game_dirs);
    });
}

void GameList::AddPermDirPopup(QMenu& context_menu, QModelIndex selected) {
    const int game_dir_index = selected.data(GameListDir::GameDirRole).toInt();

    QAction* move_up = context_menu.addAction(tr("\u25B2 Move Up"));
    QAction* move_down = context_menu.addAction(tr("\u25bc Move Down"));
    QAction* open_directory_location = context_menu.addAction(tr("Open Directory Location"));

    const int row = selected.row();

    move_up->setEnabled(row > 1);
    move_down->setEnabled(row < item_model->rowCount() - 2);

    connect(move_up, &QAction::triggered, this, [this, selected, row, game_dir_index] {
        const int other_index = selected.sibling(row - 1, 0).data(GameListDir::GameDirRole).toInt();
        std::swap(UISettings::values.game_dirs[game_dir_index],
                  UISettings::values.game_dirs[other_index]);
        item_model->setData(selected, QVariant(other_index), GameListDir::GameDirRole);
        item_model->setData(selected.sibling(row - 1, 0), QVariant(game_dir_index),
                            GameListDir::GameDirRole);
        QList<QStandardItem*> items = item_model->takeRow(row);
        item_model->invisibleRootItem()->insertRow(row - 1, items);
        tree_view->setExpanded(selected.sibling(row - 1, 0),
                               UISettings::values.game_dirs[other_index].expanded);
    });

    connect(move_down, &QAction::triggered, this, [this, selected, row, game_dir_index] {
        const int other_index = selected.sibling(row + 1, 0).data(GameListDir::GameDirRole).toInt();
        std::swap(UISettings::values.game_dirs[game_dir_index],
                  UISettings::values.game_dirs[other_index]);
        item_model->setData(selected, QVariant(other_index), GameListDir::GameDirRole);
        item_model->setData(selected.sibling(row + 1, 0), QVariant(game_dir_index),
                            GameListDir::GameDirRole);
        const QList<QStandardItem*> items = item_model->takeRow(row);
        item_model->invisibleRootItem()->insertRow(row + 1, items);
        tree_view->setExpanded(selected.sibling(row + 1, 0),
                               UISettings::values.game_dirs[other_index].expanded);
    });

    connect(open_directory_location, &QAction::triggered, this, [this, game_dir_index] {
        emit OpenDirectory(
            QString::fromStdString(UISettings::values.game_dirs[game_dir_index].path));
    });
}

void GameList::AddFavoritesPopup(QMenu& context_menu) {
    QAction* clear = context_menu.addAction(tr("Clear"));

    connect(clear, &QAction::triggered, this, [this] {
        UISettings::values.favorited_ids.clear();
        item_model->invisibleRootItem()->child(0)->removeRows(
            0, item_model->invisibleRootItem()->child(0)->rowCount());
        tree_view->setRowHidden(0, item_model->invisibleRootItem()->index(), true);
    });
}

void GameList::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    } else if (event->type() == QEvent::PaletteChange && background) {
        UpdateViewTransparency();
        if (customize_button) {
            RefreshCustomizeButtonIcon();
        }
    }

    QWidget::changeEvent(event);
}

void GameList::RetranslateUI() {
    item_model->RetranslateUI();
}

void GameList::SetupScrollAnimation() {
    auto setup = [this](QVariantAnimation* anim, QScrollBar* bar) {
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->setDuration(200);
        connect(anim, &QVariantAnimation::valueChanged, this,
                [bar](const QVariant& value) { bar->setValue(value.toInt()); });
    };

    vertical_scroll = new QVariantAnimation(this);
    horizontal_scroll = new QVariantAnimation(this);

    setup(vertical_scroll, tree_view->verticalScrollBar());
    setup(horizontal_scroll, tree_view->horizontalScrollBar());

    setup(vertical_scroll, grid_view->verticalScrollBar());
    setup(horizontal_scroll, grid_view->horizontalScrollBar());
}

bool GameList::eventFilter(QObject* obj, QEvent* event) {
    if (customize_button && (obj == surface || obj == search_field)) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
        case QEvent::Hide:
            PlaceCustomizeButton();
            break;
        default:
            break;
        }
    }
    if (!m_currentView) {
        return QWidget::eventFilter(obj, event);
    }
    if (obj == m_currentView && event->type() == QEvent::KeyPress) {
        const auto* key_event = static_cast<QKeyEvent*>(event);
        switch (key_event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (m_currentView->currentIndex().isValid()) {
                ValidateEntry(m_currentView->currentIndex());
                return true;
            }
            break;
        case Qt::Key_Escape:
            if (details_panel->isVisible()) {
                HideGameDetails();
                return true;
            }
            break;
        // Sent by controller Y and X.
        case Qt::Key_Favorites:
        case Qt::Key_Settings: {
            const QModelIndex current = m_currentView->currentIndex();
            const QModelIndex game = current.sibling(current.row(), 0);
            if (!game.isValid() || game.data(GameListItem::TypeRole).value<GameListItemType>() !=
                                       GameListItemType::Game) {
                break;
            }
            if (key_event->key() == Qt::Key_Favorites) {
                if (const u64 id = game.data(GameListItemPath::ProgramIdRole).toULongLong()) {
                    ToggleFavorite(id);
                }
            } else {
                emit OpenPerGameGeneralRequested(
                    game.data(GameListItemPath::FullPathRole).toString().toStdString());
            }
            return true;
        }
        default:
            break;
        }
    }

    if (obj == m_currentView->viewport() && event->type() == QEvent::Wheel) {
        QWheelEvent* wheelEvent = static_cast<QWheelEvent*>(event);

        bool horizontal = wheelEvent->modifiers() & Qt::ShiftModifier;

        int deltaX = wheelEvent->angleDelta().x();
        int deltaY = wheelEvent->angleDelta().y();

        if (horizontal && deltaY != 0 && deltaX == 0) {
            deltaX = deltaY;
            deltaY = 0;
        }

        if (deltaY != 0) {
            if (vertical_scroll->state() == QAbstractAnimation::Stopped)
                vertical_scroll_target = m_currentView->verticalScrollBar()->value();

            vertical_scroll_target -= deltaY;
            vertical_scroll_target =
                qBound(0, vertical_scroll_target, m_currentView->verticalScrollBar()->maximum());

            vertical_scroll->stop();
            vertical_scroll->setStartValue(m_currentView->verticalScrollBar()->value());
            vertical_scroll->setEndValue(vertical_scroll_target);
            vertical_scroll->start();
        }

        if (deltaX != 0) {
            if (horizontal_scroll->state() == QAbstractAnimation::Stopped)
                horizontal_scroll_target = m_currentView->horizontalScrollBar()->value();

            horizontal_scroll_target -= deltaX;
            horizontal_scroll_target = qBound(0, horizontal_scroll_target,
                                              m_currentView->horizontalScrollBar()->maximum());

            horizontal_scroll->stop();
            horizontal_scroll->setStartValue(m_currentView->horizontalScrollBar()->value());
            horizontal_scroll->setEndValue(horizontal_scroll_target);
            horizontal_scroll->start();
        }

        return true;
    }

    if (obj == m_currentView->viewport() && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent* mouseEvent = static_cast<QMouseEvent*>(event);

        QModelIndex index = m_currentView->indexAt(mouseEvent->pos());
        if (!index.isValid()) {
            m_currentView->selectionModel()->clearSelection();
            m_currentView->setCurrentIndex(QModelIndex());
            HideGameDetails();
        }
    }

    if (obj == grid_view->viewport() && event->type() == QEvent::Resize) {
        grid_view->UpdateIconSize();
        return true;
    }

    if (obj == carousel_view->viewport() && event->type() == QEvent::Resize) {
        carousel_view->UpdateIconSize();
        return true;
    }

    return QWidget::eventFilter(obj, event);
}

GameListPlaceholder::GameListPlaceholder(MainWindow* parent) : QWidget{parent} {
    layout = new QVBoxLayout;
    image = new QLabel;
    text = new QLabel;
    layout->setAlignment(Qt::AlignCenter);
    image->setPixmap(QIcon::fromTheme(QStringLiteral("plus_folder")).pixmap(200));

    RetranslateUI();
    QFont font = text->font();
    font.setPointSize(20);
    text->setFont(font);
    text->setAlignment(Qt::AlignHCenter);
    image->setAlignment(Qt::AlignHCenter);

    layout->addWidget(image);
    layout->addWidget(text);
    setLayout(layout);
}

GameListPlaceholder::~GameListPlaceholder() = default;

void GameListPlaceholder::mouseDoubleClickEvent(QMouseEvent* event) {
    emit GameListPlaceholder::AddDirectory();
}

void GameListPlaceholder::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }

    QWidget::changeEvent(event);
}

void GameListPlaceholder::RetranslateUI() {
    text->setText(tr("Double-click to add a new folder to the game list"));
}
