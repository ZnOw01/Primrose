// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QSlider>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>

#include "qt_common/config/uisettings.h"
#include "yuzu/game/home_background.h"
#include "yuzu/game/home_customize_panel.h"

namespace {

constexpr std::array<u32, 4> IconSizeSteps{32, 64, 128, 256};
constexpr QSize RecentThumbSize{88, 50};
constexpr int CornerRadius = 8;

constexpr std::array<Settings::GameListMode, 3> Modes{
    Settings::GameListMode::TreeView,
    Settings::GameListMode::GridView,
    Settings::GameListMode::CarouselView,
};

/// How many stacked cards each side of the centered one can show.
constexpr std::array<int, 5> SideCounts{3, 4, 5, 6, 8};

constexpr std::array<HomeBackground::Kind, 3> Kinds{
    HomeBackground::Kind::Picture,
    HomeBackground::Kind::Solid,
    HomeBackground::Kind::Gradient,
};

int IconSizeToStep(u32 size) {
    int best = 0;
    for (int i = 0; i < static_cast<int>(IconSizeSteps.size()); ++i) {
        if (std::abs(static_cast<int>(IconSizeSteps[i]) - static_cast<int>(size)) <
            std::abs(static_cast<int>(IconSizeSteps[best]) - static_cast<int>(size))) {
            best = i;
        }
    }
    return best + 1;
}

QString LocalPathFromMime(const QMimeData* mime) {
    if (!mime || !mime->hasUrls() || mime->urls().size() != 1) {
        return {};
    }
    const QUrl url = mime->urls().constFirst();
    return url.isLocalFile() ? url.toLocalFile() : QString{};
}

QIcon MakeModeIcon(Settings::GameListMode mode, const QColor& color, qreal dpr) {
    constexpr int size = 40;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);

    switch (mode) {
    case Settings::GameListMode::TreeView:
        for (int row = 0; row < 4; ++row) {
            const qreal y = 5 + row * 8.5;
            p.drawRoundedRect(QRectF(4, y, 6, 6), 1.5, 1.5);
            p.drawRoundedRect(QRectF(14, y + 1.5, row % 2 ? 16 : 22, 3), 1.5, 1.5);
        }
        break;
    case Settings::GameListMode::GridView:
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 3; ++col) {
                p.drawRoundedRect(QRectF(4 + col * 11.5, 4 + row * 11.5, 9, 9), 2, 2);
            }
        }
        break;
    case Settings::GameListMode::CarouselView: {
        QColor side = color;
        side.setAlphaF(color.alphaF() * 0.5);
        p.setBrush(side);
        p.drawRoundedRect(QRectF(1, 11, 8, 18), 2, 2);
        p.drawRoundedRect(QRectF(31, 11, 8, 18), 2, 2);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(11, 6, 18, 28), 3, 3);
        break;
    }
    }
    return QIcon(pixmap);
}

QIcon MakeKindIcon(HomeBackground::Kind kind, const QColor& color, qreal dpr) {
    constexpr int size = 40;
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);

    switch (kind) {
    case HomeBackground::Kind::Picture: {
        // A pale photo frame with a sun and a mountain.
        QColor frame = color;
        frame.setAlphaF(color.alphaF() * 0.35);
        p.setBrush(frame);
        p.drawRoundedRect(QRectF(4, 4, 32, 32), 3, 3);
        p.setBrush(color);
        p.drawEllipse(QRectF(10, 10, 7, 7));
        p.drawPolygon(QPolygonF{QPointF(8, 31), QPointF(18, 19), QPointF(28, 31)});
        break;
    }
    case HomeBackground::Kind::Solid:
        p.setBrush(color);
        p.drawRoundedRect(QRectF(4, 4, 32, 32), 3, 3);
        break;
    case HomeBackground::Kind::Gradient: {
        // A diagonal blend from the full color to a faint tint of it.
        QLinearGradient blend(QPointF(4, 4), QPointF(36, 36));
        blend.setColorAt(0.0, color);
        QColor end = color;
        end.setAlphaF(color.alphaF() * 0.25);
        blend.setColorAt(1.0, end);
        p.setBrush(blend);
        p.drawRoundedRect(QRectF(4, 4, 32, 32), 3, 3);
        break;
    }
    }
    return QIcon(pixmap);
}

/// Draws each recent background as a full-cell rounded thumbnail with an even border.
class RecentThumbDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const QRect rect(option.rect.center() - QPoint(RecentThumbSize.width() / 2,
                                                       RecentThumbSize.height() / 2),
                         RecentThumbSize);
        const QIcon thumb = index.data(Qt::DecorationRole).value<QIcon>();

        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setRenderHint(QPainter::SmoothPixmapTransform);
        thumb.paint(p, rect);

        const QPalette& pal = option.palette;
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        // Mid is black on some dark palettes, so derive the idle border from the text colour.
        QColor color = pal.color(QPalette::Text);
        color.setAlpha(70);
        qreal width = 1.0;
        if (selected) {
            color = pal.color(QPalette::Highlight);
            width = 2.0;
        } else if (hovered) {
            color = pal.color(QPalette::Highlight);
            color.setAlpha(160);
        }
        const qreal inset = width / 2.0;
        p->setPen(QPen(color, width));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(QRectF(rect).adjusted(inset, inset, -inset, -inset), 6, 6);
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return RecentThumbSize + QSize(8, 8);
    }
};

} // namespace

BackgroundDropZone::BackgroundDropZone(HomeBackground* background_, QWidget* parent)
    : QFrame{parent}, background{background_} {
    setAcceptDrops(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    connect(background, &HomeBackground::FrameChanged, this, qOverload<>(&QWidget::update));
}

bool BackgroundDropZone::hasHeightForWidth() const {
    return true;
}

int BackgroundDropZone::heightForWidth(int width) const {
    return width * 9 / 16;
}

QSize BackgroundDropZone::sizeHint() const {
    const int width = HomeCustomizePanel::PanelWidth - 32;
    return {width, heightForWidth(width)};
}

void BackgroundDropZone::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const QRectF frame_rect = QRectF(rect()).adjusted(1, 1, -1, -1);
    QPainterPath clip;
    clip.addRoundedRect(frame_rect, CornerRadius, CornerRadius);

    const QPalette& pal = palette();
    const bool has_image = !background->IsEmpty();

    p.save();
    p.setClipPath(clip);
    p.fillRect(rect(), pal.color(QPalette::Base));
    if (has_image) {
        HomeBackground::PaintCover(p, rect(), background->Frame());
    }
    p.restore();

    QString hint;
    if (drag_active) {
        hint = tr("Drop to set as background");
    } else if (!has_image) {
        hint = tr("Drop an image or video here\nor click to browse");
    } else if (hovered || hasFocus()) {
        hint = tr("Click or drop a file to change");
    }

    if (!hint.isEmpty()) {
        if (has_image) {
            QColor scrim = pal.color(QPalette::Window);
            scrim.setAlpha(170);
            p.fillPath(clip, scrim);
        }
        p.setPen(pal.color(QPalette::Text));
        p.drawText(rect().adjusted(12, 12, -12, -12), Qt::AlignCenter | Qt::TextWordWrap, hint);
    }

    QColor border_color = pal.color(QPalette::Text);
    border_color.setAlpha(70);
    if (drag_active || (hovered && has_image)) {
        border_color = pal.color(QPalette::Highlight);
    } else if (!has_image) {
        // Keep the empty drop zone visible; hover only strengthens the gray.
        border_color = pal.color(QPalette::Text);
        border_color.setAlpha(hovered ? 150 : 90);
    }
    QPen border(border_color, drag_active ? 2 : 1);
    if (!has_image || drag_active) {
        border.setStyle(Qt::DashLine);
    }
    p.setPen(border);
    p.setBrush(Qt::NoBrush);
    p.drawPath(clip);
}

void BackgroundDropZone::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        emit BrowseRequested();
    }
    QFrame::mouseReleaseEvent(event);
}

void BackgroundDropZone::enterEvent(QEnterEvent* event) {
    hovered = true;
    update();
    QFrame::enterEvent(event);
}

void BackgroundDropZone::leaveEvent(QEvent* event) {
    hovered = false;
    update();
    QFrame::leaveEvent(event);
}

void BackgroundDropZone::dragEnterEvent(QDragEnterEvent* event) {
    if (HomeBackground::CanLoad(LocalPathFromMime(event->mimeData()))) {
        drag_active = true;
        update();
        event->acceptProposedAction();
    }
}

void BackgroundDropZone::dragLeaveEvent(QDragLeaveEvent* event) {
    drag_active = false;
    update();
    QFrame::dragLeaveEvent(event);
}

void BackgroundDropZone::dropEvent(QDropEvent* event) {
    drag_active = false;
    update();
    const QString path = LocalPathFromMime(event->mimeData());
    if (!path.isEmpty()) {
        event->acceptProposedAction();
        emit FileDropped(path);
    }
}

ColorSwatchButton::ColorSwatchButton(QWidget* parent) : QPushButton{parent} {
    setCursor(Qt::PointingHandCursor);
    setFixedSize(72, 24);
}

void ColorSwatchButton::SetColor(const QColor& color_) {
    if (color != color_) {
        color = color_;
        update();
    }
}

void ColorSwatchButton::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // The body shows the background color itself; only the border is palette-derived.
    const QPalette& pal = palette();
    QColor border = pal.color(QPalette::Text);
    border.setAlpha(70);
    if (hasFocus()) {
        border = pal.color(QPalette::Highlight);
    } else if (hovered) {
        border = pal.color(QPalette::Highlight);
        border.setAlpha(160);
    }
    p.setPen(QPen(border, 1));
    p.setBrush(color.isValid() ? color : pal.color(QPalette::Base));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), CornerRadius, CornerRadius);
}

void ColorSwatchButton::enterEvent(QEnterEvent* event) {
    hovered = true;
    update();
    QPushButton::enterEvent(event);
}

void ColorSwatchButton::leaveEvent(QEvent* event) {
    hovered = false;
    update();
    QPushButton::leaveEvent(event);
}

HomeCustomizePanel::HomeCustomizePanel(HomeBackground* background_, QWidget* parent)
    : HomeSidePanel{parent}, background{background_} {
    QWidget* scroll_content = Content();
    auto* layout = new QVBoxLayout(scroll_content);
    layout->setContentsMargins(16, 12, 16, 16);
    layout->setSpacing(8);

    // Header
    auto* header = new QHBoxLayout;
    header->setSpacing(8);
    title_icon = new QLabel(scroll_content);
    title_label = MakeHeading(scroll_content, 1.4);
    close_button = new QToolButton(scroll_content);
    close_button->setAutoRaise(true);
    close_button->setIconSize(QSize(16, 16));
    connect(close_button, &QToolButton::clicked, this, &HomeCustomizePanel::CloseRequested);
    header->addWidget(title_icon);
    header->addWidget(title_label, 1);
    header->addWidget(close_button);
    layout->addLayout(header);

    // Background
    background_label = MakeHeading(scroll_content, 1.15);
    layout->addWidget(background_label);

    // The background kind: a picture, a solid color or a two-color gradient.
    kind_group = new QButtonGroup(this);
    kind_group->setExclusive(true);
    auto* kind_row = new QHBoxLayout;
    kind_row->setSpacing(8);
    for (size_t i = 0; i < Kinds.size(); ++i) {
        auto* button = new QToolButton(scroll_content);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(40, 40));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(84);
        kind_group->addButton(button, static_cast<int>(i));
        kind_row->addWidget(button);
        kind_buttons[i] = button;
    }
    connect(kind_group, &QButtonGroup::idClicked, this, [this](int id) {
        emit BackgroundKindRequested(Kinds[id]);
    });
    layout->addLayout(kind_row);

    const auto choose_color = [this](ColorSwatchButton* swatch, int index) {
        const QColor picked = QColorDialog::getColor(swatch->GetColor(), this,
                                                    tr("Choose Home Screen Background Color"));
        if (picked.isValid()) {
            emit BackgroundColorRequested(index, picked);
        }
    };
    solid_row = new QWidget(scroll_content);
    auto* solid_layout = new QHBoxLayout(solid_row);
    solid_layout->setContentsMargins(0, 0, 0, 0);
    solid_layout->setSpacing(8);
    solid_label = new QLabel(solid_row);
    solid_swatch = new ColorSwatchButton(solid_row);
    solid_layout->addWidget(solid_label);
    solid_layout->addStretch(1);
    solid_layout->addWidget(solid_swatch);
    connect(solid_swatch, &QAbstractButton::clicked, this,
            [this, choose_color] { choose_color(solid_swatch, 0); });
    layout->addWidget(solid_row);

    gradient_row = new QWidget(scroll_content);
    auto* gradient_layout = new QHBoxLayout(gradient_row);
    gradient_layout->setContentsMargins(0, 0, 0, 0);
    gradient_layout->setSpacing(8);
    gradient_label = new QLabel(gradient_row);
    gradient_swatch_start = new ColorSwatchButton(gradient_row);
    gradient_swatch_end = new ColorSwatchButton(gradient_row);
    gradient_layout->addWidget(gradient_label);
    gradient_layout->addStretch(1);
    gradient_layout->addWidget(gradient_swatch_start);
    gradient_layout->addWidget(gradient_swatch_end);
    connect(gradient_swatch_start, &QAbstractButton::clicked, this,
            [this, choose_color] { choose_color(gradient_swatch_start, 0); });
    connect(gradient_swatch_end, &QAbstractButton::clicked, this,
            [this, choose_color] { choose_color(gradient_swatch_end, 1); });
    layout->addWidget(gradient_row);

    drop_zone = new BackgroundDropZone(background, scroll_content);
    connect(drop_zone, &BackgroundDropZone::BrowseRequested, this, &HomeCustomizePanel::Browse);
    connect(drop_zone, &BackgroundDropZone::FileDropped, this,
            &HomeCustomizePanel::BackgroundChosen);
    layout->addWidget(drop_zone);

    recent_label = new QLabel(scroll_content);
    layout->addWidget(recent_label);

    recent_list = new QListWidget(scroll_content);
    recent_list->setViewMode(QListView::IconMode);
    recent_list->setFlow(QListView::LeftToRight);
    recent_list->setWrapping(false);
    recent_list->setMovement(QListView::Static);
    recent_list->setIconSize(RecentThumbSize);
    recent_list->setGridSize(RecentThumbSize + QSize(8, 8));
    recent_list->setUniformItemSizes(true);
    recent_list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    recent_list->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    recent_list->setFrameShape(QFrame::NoFrame);
    recent_list->setFixedHeight(RecentThumbSize.height() + 8 +
                                recent_list->horizontalScrollBar()->sizeHint().height());
    recent_list->setContextMenuPolicy(Qt::CustomContextMenu);
    recent_list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; }"));
    recent_list->setItemDelegate(new RecentThumbDelegate(recent_list));
    recent_list->setMouseTracking(true);
    connect(recent_list, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        emit BackgroundChosen(item->data(Qt::UserRole).toString());
    });
    connect(recent_list, &QListWidget::customContextMenuRequested, this,
            &HomeCustomizePanel::OnRecentContextMenu);
    layout->addWidget(recent_list);

    layout->addSpacing(12);

    // Display mode
    mode_label = MakeHeading(scroll_content, 1.15);
    layout->addWidget(mode_label);

    auto* mode_row = new QHBoxLayout;
    mode_row->setSpacing(8);
    mode_group = new QButtonGroup(this);
    mode_group->setExclusive(true);
    for (size_t i = 0; i < Modes.size(); ++i) {
        auto* button = new QToolButton(scroll_content);
        button->setCheckable(true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIconSize(QSize(40, 40));
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(84);
        mode_group->addButton(button, static_cast<int>(i));
        mode_row->addWidget(button);
        mode_buttons[i] = button;
    }
    connect(mode_group, &QButtonGroup::idClicked, this, [this](int id) {
        emit GameListModeRequested(Modes[id]);
    });
    layout->addLayout(mode_row);

    // The platform frame drawn over game icons; only the icon modes can show it.
    border_check = new QCheckBox(scroll_content);
    border_check->setCursor(Qt::PointingHandCursor);
    connect(border_check, &QCheckBox::toggled, this, [this](bool show) {
        if (!syncing) {
            emit GameIconBordersRequested(show);
        }
    });
    layout->addWidget(border_check);

    // The frame the cards wear when icon borders are on.
    cartridge_row = new QWidget(scroll_content);
    auto* cartridge_layout = new QVBoxLayout(cartridge_row);
    cartridge_layout->setContentsMargins(0, 0, 0, 0);
    cartridge_layout->setSpacing(8);
    cartridge_label = MakeHeading(cartridge_row);
    cartridge_layout->addWidget(cartridge_label);
    auto* cartridge_buttons_row = new QHBoxLayout;
    cartridge_buttons_row->setSpacing(8);
    cartridge_group = new QButtonGroup(this);
    cartridge_group->setExclusive(true);
    for (size_t i = 0; i < cartridge_buttons.size(); ++i) {
        auto* button = new QToolButton(cartridge_row);
        button->setCheckable(true);
        button->setFixedHeight(28);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        cartridge_group->addButton(button, static_cast<int>(i));
        cartridge_buttons_row->addWidget(button);
        cartridge_buttons[i] = button;
    }
    connect(cartridge_group, &QButtonGroup::idClicked, this, [this](int id) {
        emit CartridgeKindRequested(static_cast<u8>(id));
    });
    cartridge_layout->addLayout(cartridge_buttons_row);
    layout->addWidget(cartridge_row);

    // The centered card's share of the carousel view's height.
    card_size_row = new QWidget(scroll_content);
    auto* card_size_layout = new QVBoxLayout(card_size_row);
    card_size_layout->setContentsMargins(0, 0, 0, 0);
    card_size_layout->setSpacing(8);
    card_size_label = MakeHeading(card_size_row);
    card_size_layout->addWidget(card_size_label);
    auto* card_size_buttons_row = new QHBoxLayout;
    card_size_buttons_row->setSpacing(8);
    card_size_group = new QButtonGroup(this);
    card_size_group->setExclusive(true);
    for (size_t i = 0; i < card_size_buttons.size(); ++i) {
        auto* button = new QToolButton(card_size_row);
        button->setCheckable(true);
        button->setFixedHeight(28);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        card_size_group->addButton(button, static_cast<int>(i));
        card_size_buttons_row->addWidget(button);
        card_size_buttons[i] = button;
    }
    connect(card_size_group, &QButtonGroup::idClicked, this, [this](int id) {
        emit CarouselCardSizeRequested(static_cast<u8>(id));
    });
    card_size_layout->addLayout(card_size_buttons_row);
    layout->addWidget(card_size_row);

    // How many cards the carousel stacks up on each side of the centered one.
    side_count_row = new QWidget(scroll_content);
    auto* side_count_layout = new QVBoxLayout(side_count_row);
    side_count_layout->setContentsMargins(0, 0, 0, 0);
    side_count_layout->setSpacing(8);
    side_count_label = MakeHeading(side_count_row);
    side_count_layout->addWidget(side_count_label);
    auto* side_count_buttons_row = new QHBoxLayout;
    side_count_buttons_row->setSpacing(8);
    side_count_group = new QButtonGroup(this);
    side_count_group->setExclusive(true);
    for (size_t i = 0; i < side_count_buttons.size(); ++i) {
        auto* button = new QToolButton(side_count_row);
        button->setCheckable(true);
        button->setFixedHeight(28);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // The button ids are the counts themselves, so the signal can carry one straight out.
        side_count_group->addButton(button, SideCounts[i]);
        side_count_buttons_row->addWidget(button);
        side_count_buttons[i] = button;
    }
    connect(side_count_group, &QButtonGroup::idClicked, this, [this](int id) {
        emit CarouselSideCountRequested(static_cast<u8>(id));
    });
    side_count_layout->addLayout(side_count_buttons_row);
    layout->addWidget(side_count_row);

    // The centered game's banner can stand in for the background.
    banner_check = new QCheckBox(scroll_content);
    banner_check->setCursor(Qt::PointingHandCursor);
    connect(banner_check, &QCheckBox::toggled, this, [this](bool show) {
        if (!syncing) {
            emit CarouselBannerBackgroundRequested(show);
        }
    });
    layout->addWidget(banner_check);

    // Icon size
    auto* size_row = new QHBoxLayout;
    icon_size_title = new QLabel(scroll_content);
    icon_size_value = new QLabel(scroll_content);
    icon_size_value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    size_row->addWidget(icon_size_title);
    size_row->addWidget(icon_size_value, 1);
    layout->addSpacing(4);
    layout->addLayout(size_row);

    icon_size_slider = new QSlider(Qt::Horizontal, scroll_content);
    icon_size_slider->setRange(1, static_cast<int>(IconSizeSteps.size()));
    icon_size_slider->setPageStep(1);
    icon_size_slider->setTickPosition(QSlider::TicksBelow);
    icon_size_slider->setTickInterval(1);
    // Changing the icon size rebuilds the game list, so apply on release, not while dragging.
    connect(icon_size_slider, &QSlider::valueChanged, this, [this](int) {
        UpdateIconSizeLabel();
        if (!syncing && !icon_size_slider->isSliderDown()) {
            emit GameIconSizeRequested(IconSizeSteps[icon_size_slider->value() - 1]);
        }
    });
    connect(icon_size_slider, &QSlider::sliderReleased, this, [this] {
        emit GameIconSizeRequested(IconSizeSteps[icon_size_slider->value() - 1]);
    });
    layout->addWidget(icon_size_slider);

    layout->addStretch(1);

    RetranslateUI();
    RefreshGlyphs();
    SyncFromSettings();

}

void HomeCustomizePanel::SyncFromSettings() {
    syncing = true;

    const auto mode = UISettings::values.game_list_mode.GetValue();
    for (size_t i = 0; i < Modes.size(); ++i) {
        mode_buttons[i]->setChecked(Modes[i] == mode);
    }

    // The card size choice only exists in the carousel.
    card_size_row->setVisible(mode == Settings::GameListMode::CarouselView);
    size_t size_index = UISettings::values.carousel_card_size.GetValue();
    if (size_index >= card_size_buttons.size()) {
        size_index = 0;
    }
    for (size_t i = 0; i < card_size_buttons.size(); ++i) {
        card_size_buttons[i]->setChecked(i == size_index);
    }

    // The stack depth choice only exists in the carousel; a hand-edited config value checks
    // the nearest offered count.
    side_count_row->setVisible(mode == Settings::GameListMode::CarouselView);
    const int side_count = UISettings::values.carousel_side_count.GetValue();
    size_t nearest = 0;
    for (size_t i = 1; i < SideCounts.size(); ++i) {
        if (std::abs(SideCounts[i] - side_count) < std::abs(SideCounts[nearest] - side_count)) {
            nearest = i;
        }
    }
    for (size_t i = 0; i < side_count_buttons.size(); ++i) {
        side_count_buttons[i]->setChecked(i == nearest);
    }
    banner_check->setVisible(mode == Settings::GameListMode::CarouselView);
    {
        const QSignalBlocker blocker(banner_check);
        banner_check->setChecked(UISettings::values.carousel_banner_background.GetValue());
    }
    // The frame choice only matters while icon borders are drawn.
    const u8 cartridge_kind = std::min<u8>(1, UISettings::values.game_cartridge_kind.GetValue());
    for (size_t i = 0; i < cartridge_buttons.size(); ++i) {
        cartridge_buttons[i]->setChecked(i == cartridge_kind);
    }

    const auto kind =
        HomeBackground::KindFromInt(UISettings::values.home_background_kind.GetValue());
    for (size_t i = 0; i < Kinds.size(); ++i) {
        kind_buttons[i]->setChecked(Kinds[i] == kind);
    }
    const QColor color = HomeBackground::ColorFromString(
        QString::fromStdString(UISettings::values.home_background_color.GetValue()));
    const QColor color2 = HomeBackground::ColorFromString(
        QString::fromStdString(UISettings::values.home_background_color2.GetValue()));
    solid_swatch->SetColor(color);
    gradient_swatch_start->SetColor(color);
    gradient_swatch_end->SetColor(color2);
    // Only the picture kind uses the drop zone; each fill kind shows its own color row.
    drop_zone->setVisible(kind == HomeBackground::Kind::Picture);
    solid_row->setVisible(kind == HomeBackground::Kind::Solid);
    gradient_row->setVisible(kind == HomeBackground::Kind::Gradient);

    const u32 icon_size = UISettings::values.game_icon_size.GetValue();
    if (!icon_size_slider->isSliderDown()) {
        icon_size_slider->setValue(IconSizeToStep(icon_size));
    }
    UpdateIconSizeLabel();

    {
        const QSignalBlocker blocker(border_check);
        border_check->setChecked(UISettings::values.game_icon_borders.GetValue());
    }

    RefreshRecents();
    drop_zone->update();

    syncing = false;
}

void HomeCustomizePanel::Browse() {
    const QString current = QString::fromStdString(UISettings::values.home_background.GetValue());
    const QString dir = current.isEmpty() ? QString{} : QFileInfo(current).absolutePath();
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose Home Screen Background"),
                                                      dir, HomeBackground::FileDialogFilter());
    if (!path.isEmpty()) {
        emit BackgroundChosen(path);
    }
}

QPixmap HomeCustomizePanel::Thumbnail(const QString& path) {
    if (const auto it = thumbnail_cache.constFind(path); it != thumbnail_cache.cend()) {
        return *it;
    }

    const qreal dpr = devicePixelRatioF();
    QPixmap pixmap(RecentThumbSize * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRect rect(QPoint{}, RecentThumbSize);
    QPainterPath clip;
    clip.addRoundedRect(QRectF(rect), 6, 6);
    p.setClipPath(clip);
    p.fillRect(rect, palette().color(QPalette::Base));

    const QPalette& pal = palette();
    if (path.isEmpty()) {
        p.setPen(pal.color(QPalette::PlaceholderText));
        p.drawText(rect, Qt::AlignCenter, tr("None"));
    } else if (const QImage still = HomeBackground::LoadThumbnail(path, RecentThumbSize);
               !still.isNull()) {
        HomeBackground::PaintCover(p, rect, still);
    } else {
        // Videos have no cheap still: show a play glyph and the file name.
        p.setPen(Qt::NoPen);
        p.setBrush(pal.color(QPalette::Text));
        const QPointF c(rect.center().x() + 1, rect.center().y() - 6);
        p.drawPolygon(QPolygonF{c + QPointF(-6, -8), c + QPointF(-6, 8), c + QPointF(8, 0)});
        p.setPen(pal.color(QPalette::Text));
        QFont font = p.font();
        font.setPointSizeF(font.pointSizeF() * 0.8);
        p.setFont(font);
        const QRect text_rect = rect.adjusted(4, rect.height() - 18, -4, -2);
        p.drawText(text_rect, Qt::AlignCenter,
                   QFontMetrics(font).elidedText(QFileInfo(path).fileName(), Qt::ElideMiddle,
                                                 text_rect.width()));
    }
    p.end();

    thumbnail_cache.insert(path, pixmap);
    return pixmap;
}

void HomeCustomizePanel::RefreshRecents() {
    const QString current = QString::fromStdString(UISettings::values.home_background.GetValue());

    QStringList paths{QString{}};
    for (const QString& path : std::as_const(UISettings::values.recent_home_backgrounds)) {
        if (QFileInfo::exists(path)) {
            paths << path;
        }
    }

    // Only offer recents once a background has been used, and only while a picture can use them.
    const bool show_recents = paths.size() > 1 || !current.isEmpty();
    const bool picture =
        HomeBackground::KindFromInt(UISettings::values.home_background_kind.GetValue()) ==
        HomeBackground::Kind::Picture;
    recent_label->setVisible(show_recents && picture);
    recent_list->setVisible(show_recents && picture);

    recent_list->clear();
    for (const QString& path : paths) {
        auto* item = new QListWidgetItem(QIcon(Thumbnail(path)), QString{}, recent_list);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path.isEmpty() ? tr("No background") : path);
        item->setSizeHint(RecentThumbSize + QSize(8, 8));
        if (path == current) {
            item->setSelected(true);
            recent_list->scrollToItem(item);
        }
    }
}

void HomeCustomizePanel::OnRecentContextMenu(const QPoint& pos) {
    QListWidgetItem* item = recent_list->itemAt(pos);
    if (!item) {
        return;
    }
    const QString path = item->data(Qt::UserRole).toString();
    if (path.isEmpty()) {
        return;
    }
    QMenu menu;
    QAction* remove = menu.addAction(tr("Remove from Recent"));
    connect(remove, &QAction::triggered, this, [this, path] {
        thumbnail_cache.remove(path);
        emit RecentBackgroundRemoved(path);
    });
    menu.exec(recent_list->viewport()->mapToGlobal(pos));
}

void HomeCustomizePanel::RefreshGlyphs() {
    const QPalette& pal = palette();
    const qreal dpr = devicePixelRatioF();
    const QColor color = pal.color(QPalette::ButtonText);
    for (size_t i = 0; i < Modes.size(); ++i) {
        mode_buttons[i]->setIcon(MakeModeIcon(Modes[i], color, dpr));
    }
    for (size_t i = 0; i < Kinds.size(); ++i) {
        kind_buttons[i]->setIcon(MakeKindIcon(Kinds[i], color, dpr));
    }
    close_button->setIcon(HomeGlyphs::Close(color, dpr));
    title_icon->setPixmap(
        HomeGlyphs::Brush(pal.color(QPalette::Highlight), dpr, 22).pixmap(QSize(22, 22), dpr));
}

void HomeCustomizePanel::UpdateIconSizeLabel() {
    static constexpr std::array<const char*, 4> names{
        QT_TR_NOOP("Small"), QT_TR_NOOP("Standard"), QT_TR_NOOP("Large"), QT_TR_NOOP("Full Size")};
    const int step = icon_size_slider->value();
    icon_size_value->setText(QStringLiteral("%1 (%2px)")
                                 .arg(tr(names[step - 1]))
                                 .arg(IconSizeSteps[step - 1]));
}

void HomeCustomizePanel::RetranslateUI() {
    title_label->setText(tr("Customization"));
    background_label->setText(tr("Background"));
    close_button->setToolTip(tr("Close"));
    kind_buttons[0]->setText(tr("Picture"));
    kind_buttons[1]->setText(tr("Solid"));
    kind_buttons[2]->setText(tr("Gradient"));
    solid_label->setText(tr("Color"));
    gradient_label->setText(tr("Colors"));
    solid_swatch->setToolTip(tr("Choose the solid background color"));
    gradient_swatch_start->setToolTip(tr("Choose the gradient start color"));
    gradient_swatch_end->setToolTip(tr("Choose the gradient end color"));
    drop_zone->setToolTip(tr("Click to choose a background, or drop an image or video here"));
    recent_label->setText(tr("Recent backgrounds"));
    mode_label->setText(tr("Display Mode"));
    mode_buttons[0]->setText(tr("Tree"));
    mode_buttons[1]->setText(tr("Grid"));
    mode_buttons[2]->setText(tr("Carousel"));
    border_check->setText(tr("Game icon borders"));
    border_check->setToolTip(tr("Draw the console frame around game icons in the grid and "
                                 "carousel views"));
    cartridge_label->setText(tr("Cartridge type"));
    cartridge_label->setToolTip(tr("The frame game cards use when icon borders are on"));
    cartridge_buttons[0]->setText(tr("Border"));
    cartridge_buttons[1]->setText(tr("SVG"));
    card_size_label->setText(tr("Card size"));
    card_size_label->setToolTip(tr("Size of the centered game card in the carousel view"));
    card_size_buttons[0]->setText(tr("Full"));
    card_size_buttons[1]->setText(tr("3/4"));
    card_size_buttons[2]->setText(tr("2/3"));
    card_size_buttons[3]->setText(tr("1/2"));
    card_size_buttons[4]->setText(tr("1/3"));
    side_count_label->setText(tr("Cards per side"));
    side_count_label->setToolTip(tr("How many game cards the carousel stacks up on each side "
                                    "of the centered one"));
    for (size_t i = 0; i < side_count_buttons.size(); ++i) {
        side_count_buttons[i]->setText(QString::number(SideCounts[i]));
    }
    banner_check->setText(tr("Show game banner as background"));
    banner_check->setToolTip(tr("Behind the carousel, show the centered game's banner, softly "
                                "blurred, when the game has one"));
    icon_size_title->setText(tr("Icon size"));
    UpdateIconSizeLabel();
}

void HomeCustomizePanel::OnLanguageChanged() {
    RetranslateUI();
}

void HomeCustomizePanel::OnPaletteChanged() {
    RefreshGlyphs();
    thumbnail_cache.clear();
    RefreshRecents();
}
