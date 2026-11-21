// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <numbers>

#include <QBuffer>
#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include "qt_common/config/uisettings.h"
#include "qt_common/util/game_art.h"
#include "yuzu/game/game_details_panel.h"
#include "yuzu/game/home_background.h"
#include "yuzu/util/icon_search.h"

namespace {

constexpr int IconSize = 84;
constexpr int SideMargin = 16;
constexpr int HeroMaxWidth = 1280;
constexpr int StoredIconSize = 256;
constexpr int OverlayButtonSize = 28;
/// Height of the action bar buttons. Buttons in the panel body match it so the panel reads as
/// one set of controls.
constexpr int ActionButtonHeight = 38;

enum class Glyph { Play, Configure, StarOutline, StarFilled, More };

QIcon MakeGlyph(Glyph glyph, const QColor& color, qreal dpr, int size = 20) {
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);

    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 20.0, size / 20.0);
    p.setPen(Qt::NoPen);
    p.setBrush(color);

    const auto star = [] {
        QPolygonF poly;
        for (int i = 0; i < 10; ++i) {
            const qreal radius = i % 2 == 0 ? 8.5 : 3.6;
            const qreal angle = -std::numbers::pi / 2 + i * std::numbers::pi / 5;
            poly << QPointF(10 + radius * std::cos(angle), 10.6 + radius * std::sin(angle));
        }
        return poly;
    };

    switch (glyph) {
    case Glyph::Play:
        p.drawPolygon(QPolygonF{QPointF(6, 3.5), QPointF(6, 16.5), QPointF(16.5, 10)});
        break;
    case Glyph::Configure:
        // Three sliders.
        for (int i = 0; i < 3; ++i) {
            const qreal y = 4.5 + i * 5.5;
            const qreal knob = i == 1 ? 13 : (i == 0 ? 6.5 : 9);
            p.drawRoundedRect(QRectF(2.5, y - 0.9, 15, 1.8), 0.9, 0.9);
            p.drawEllipse(QPointF(knob, y), 2.4, 2.4);
        }
        break;
    case Glyph::StarFilled:
        p.drawPolygon(star());
        break;
    case Glyph::StarOutline:
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolygon(star());
        break;
    case Glyph::More:
        for (int i = 0; i < 3; ++i) {
            p.drawEllipse(QPointF(4 + i * 6, 10), 1.8, 1.8);
        }
        break;
    }
    return QIcon(pixmap);
}

QString Rgba(QColor color) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QColor WithAlpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

/// Steps a surface colour up for hover states, whichever direction the palette goes.
QColor Emphasized(const QColor& color, int amount) {
    return color.lightness() < 128 ? color.lighter(amount) : color.darker(amount);
}

void SetMuted(QWidget* widget, int alpha = 150) {
    QPalette pal = widget->palette();
    pal.setColor(QPalette::WindowText, WithAlpha(pal.color(QPalette::WindowText), alpha));
    widget->setPalette(pal);
}

void ClearLayout(QLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

QByteArray EncodeHeroImage(QImage image) {
    if (image.isNull()) {
        return {};
    }
    image = image.convertToFormat(QImage::Format_RGB888);
    if (image.width() > HeroMaxWidth) {
        image = image.scaledToWidth(HeroMaxWidth, Qt::SmoothTransformation);
    }
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "JPEG", 90);
    return out;
}

QByteArray EncodeIconImage(const QImage& image) {
    if (image.isNull()) {
        return {};
    }
    const int side = std::min(image.width(), image.height());
    const QImage square =
        image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
            .scaled(StoredIconSize, StoredIconSize, Qt::IgnoreAspectRatio,
                    Qt::SmoothTransformation);
    QByteArray out;
    QBuffer buffer(&out);
    buffer.open(QIODevice::WriteOnly);
    square.save(&buffer, "PNG");
    return out;
}

/// Lays chips out left to right and wraps them onto further lines when they don't fit.
class FlowLayout : public QLayout {
public:
    FlowLayout(int spacing_) : spacing{spacing_} {
        setContentsMargins(0, 0, 0, 0);
    }
    ~FlowLayout() override {
        while (QLayoutItem* item = takeAt(0)) {
            delete item;
        }
    }

    void addItem(QLayoutItem* item) override {
        items.append(item);
    }
    int count() const override {
        return static_cast<int>(items.size());
    }
    QLayoutItem* itemAt(int index) const override {
        return items.value(index);
    }
    QLayoutItem* takeAt(int index) override {
        return index >= 0 && index < items.size() ? items.takeAt(index) : nullptr;
    }
    Qt::Orientations expandingDirections() const override {
        return {};
    }
    bool hasHeightForWidth() const override {
        return true;
    }
    int heightForWidth(int width) const override {
        return Arrange(QRect(0, 0, width, 0), false);
    }
    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        Arrange(rect, true);
    }
    QSize sizeHint() const override {
        return minimumSize();
    }
    QSize minimumSize() const override {
        QSize size;
        for (const QLayoutItem* item : items) {
            size = size.expandedTo(item->minimumSize());
        }
        return size;
    }

private:
    int Arrange(const QRect& rect, bool apply) const {
        int x = rect.x();
        int y = rect.y();
        int line_height = 0;
        for (QLayoutItem* item : items) {
            const QSize hint = item->sizeHint().boundedTo(QSize(rect.width(), QWIDGETSIZE_MAX));
            if (x > rect.x() && x + hint.width() > rect.right() + 1) {
                x = rect.x();
                y += line_height + spacing;
                line_height = 0;
            }
            if (apply) {
                item->setGeometry(QRect(QPoint(x, y), hint));
            }
            x += hint.width() + spacing;
            line_height = std::max(line_height, hint.height());
        }
        return y + line_height - rect.y();
    }

    QList<QLayoutItem*> items;
    int spacing;
};

/// Check box that elides its label to the width it gets instead of demanding the full width.
class ElidedCheckBox : public QCheckBox {
public:
    ElidedCheckBox(const QString& text, QWidget* parent) : QCheckBox{parent}, full_text{text} {
        // QCheckBox never shrinks below its full label by default.
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setToolTip(text);
        setText(text);
    }

    QSize sizeHint() const override {
        return {fontMetrics().horizontalAdvance(full_text) + LabelOffset(),
                QCheckBox::sizeHint().height()};
    }
    QSize minimumSizeHint() const override {
        return {LabelOffset() + fontMetrics().horizontalAdvance(QStringLiteral("M...")),
                QCheckBox::minimumSizeHint().height()};
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QCheckBox::resizeEvent(event);
        setText(fontMetrics().elidedText(full_text, Qt::ElideRight,
                                         std::max(0, width() - LabelOffset())));
    }

private:
    int LabelOffset() const {
        return style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, this) +
               style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, this) + 4;
    }

    QString full_text;
};

/// Icon-only button that paints its pill and glyph itself. A styled QToolButton has its frame
/// and icon laid out by the platform style, which draws the surface smaller than the widget and
/// scales the glyph down to fit: at 28 px the icon came out at roughly half its size, which read
/// as broken artwork buttons. Painting both keeps the geometry exact on every style and theme,
/// and lets the panel own the surface instead of a translucent scrim over the artwork.
class IconButton : public QToolButton {
public:
    enum class Kind {
        /// Sits on the banner and the game icon.
        Overlay,
        /// Secondary control in the action bar.
        Outline,
    };

    IconButton(Kind kind_, int size, int icon_size, QWidget* parent)
        : QToolButton{parent}, kind{kind_} {
        setFixedSize(size, size);
        setIconSize(QSize(icon_size, icon_size));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setToolButtonStyle(Qt::ToolButtonIconOnly);
        setAttribute(Qt::WA_Hover);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QPalette& pal = palette();
        const QColor text = pal.color(QPalette::WindowText);
        const QColor surface = pal.color(QPalette::Window);
        const bool hovered = underMouse();
        const bool pressed = isDown();

        QColor fill;
        QColor border;
        if (kind == Kind::Overlay) {
            if (pressed) {
                fill = WithAlpha(Emphasized(surface, 115), 245);
            } else if (hovered) {
                fill = WithAlpha(Emphasized(surface, 135), 245);
            } else {
                fill = WithAlpha(surface, 235);
            }
            border = WithAlpha(text, hovered ? 110 : 55);
        } else {
            fill = pressed ? WithAlpha(text, 45)
                           : (hovered ? WithAlpha(text, 25) : QColor(Qt::transparent));
            border = WithAlpha(text, 60);
        }

        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(box, 8, 8);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(hasFocus() ? pal.color(QPalette::Highlight) : border, 1));
        p.drawRoundedRect(box, 8, 8);

        const QSize glyph = iconSize();
        const QRect glyph_rect(QPoint((width() - glyph.width()) / 2, (height() - glyph.height()) / 2),
                               glyph);
        const QIcon::Mode mode = !isEnabled() ? QIcon::Disabled
                                              : (hovered ? QIcon::Active : QIcon::Normal);
        icon().paint(&p, glyph_rect, Qt::AlignCenter, mode, QIcon::Off);
    }

    void enterEvent(QEnterEvent* event) override {
        update();
        QToolButton::enterEvent(event);
    }
    void leaveEvent(QEvent* event) override {
        update();
        QToolButton::leaveEvent(event);
    }

private:
    Kind kind;
};

IconButton* MakeOverlayButton(QWidget* parent) {
    // At 16 px the brush's handle and bristles collapse into one diagonal stroke.
    return new IconButton(IconButton::Kind::Overlay, OverlayButtonSize, 20, parent);
}

/// Secondary button in the panel body. Outlined like the action bar's tool buttons and derived
/// from the palette, so every control on the panel reads as one family and stays legible on
/// light and dark themes alike. The height is stated in the style sheet as well because QSS,
/// not the widget, decides the size of a styled button.
QString BodyButtonStyle(const QPalette& pal) {
    const QColor text = pal.color(QPalette::ButtonText);
    const QColor accent = pal.color(QPalette::Highlight);
    return QStringLiteral(
        "QPushButton { border: 1px solid %1; border-radius: 8px; padding: 0 12px; "
        "min-height: %2px; max-height: %2px; background: transparent; "
        "color: %3; } "
        "QPushButton:hover { background: %4; border-color: %5; } "
        "QPushButton:pressed { background: %6; } "
        "QPushButton:disabled { border-color: %7; color: %8; }")
        // The 1 px border is part of the box, so the content height is the bar height minus it.
        .arg(Rgba(WithAlpha(text, 60)), QString::number(ActionButtonHeight - 2),
             Rgba(text), Rgba(WithAlpha(accent, 45)), Rgba(WithAlpha(accent, 120)),
             Rgba(WithAlpha(accent, 75)), Rgba(WithAlpha(text, 35)), Rgba(WithAlpha(text, 90)));
}

/// Collapsible section header. A full-width row with a faint surface of its own: it says
/// "click me to unfold" instead of looking like the plain headings around it.
QString SectionToggleStyle(const QPalette& pal) {
    const QColor text = pal.color(QPalette::ButtonText);
    return QStringLiteral(
        "QToolButton { border: 1px solid transparent; border-radius: 8px; padding: 0 8px; "
        "min-height: %1px; max-height: %1px; background: %2; text-align: left; color: %3; } "
        "QToolButton:hover { background: %4; border-color: %5; } "
        "QToolButton:pressed { background: %6; }")
        .arg(QString::number(ActionButtonHeight - 2), Rgba(WithAlpha(text, 14)), Rgba(text),
             Rgba(WithAlpha(text, 28)), Rgba(WithAlpha(text, 55)),
             Rgba(WithAlpha(text, 20)));
}

} // namespace

GameDetailsHeader::GameDetailsHeader(QWidget* parent) : QWidget{parent} {
    // Not Fixed: a fixed policy caps the height at sizeHint(), so the layout centres the header
    // in its taller height-for-width slot and the title is clipped.
    QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    title = new QLabel(this);
    title->setWordWrap(true);
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont title_font = title->font();
    title_font.setBold(true);
    title_font.setPointSizeF(title_font.pointSizeF() * 1.3);
    title->setFont(title_font);

    developer = new QLabel(this);
    developer->setWordWrap(true);

    close_button = MakeOverlayButton(this);
    connect(close_button, &QToolButton::clicked, this, &GameDetailsHeader::CloseRequested);

    banner_edit = MakeOverlayButton(this);
    banner_edit->setPopupMode(QToolButton::InstantPopup);
    icon_edit = MakeOverlayButton(this);
    icon_edit->setPopupMode(QToolButton::InstantPopup);
    banner_edit->hide();
    icon_edit->hide();

    RefreshGlyphs();
    RefreshDeveloperPalette();
}

void GameDetailsHeader::RefreshDeveloperPalette() {
    QPalette pal = developer->palette();
    pal.setColor(QPalette::WindowText, WithAlpha(palette().color(QPalette::WindowText), 170));
    developer->setPalette(pal);
}

void GameDetailsHeader::SetGame(const GameDetailsInfo& info, const QImage& hero_) {
    hero = hero_;
    title->setText(info.title);
    developer->setText(info.developer);
    developer->setVisible(!info.developer.isEmpty());
    SetIcon(info.icon);
}

void GameDetailsHeader::SetIcon(const QPixmap& icon_) {
    icon = icon_;
    // Without a banner, a heavily downscaled icon stretched across makes a soft colour wash.
    fallback_hero = icon.isNull() ? QImage{}
                                  : icon.toImage().scaled(12, 12, Qt::IgnoreAspectRatio,
                                                          Qt::SmoothTransformation);
    LayoutLabels();
    update();
}

void GameDetailsHeader::SetEditable(bool editable_) {
    editable = editable_;
    UpdateEditButtons();
}

void GameDetailsHeader::SetBusy(bool busy_) {
    busy = busy_;
    UpdateEditButtons();
    update();
}

void GameDetailsHeader::UpdateEditButtons() {
    // Always visible while the artwork can be changed: a control that only exists once the
    // pointer is already over the artwork is one nobody finds.
    const bool show = editable && !busy;
    banner_edit->setVisible(show);
    icon_edit->setVisible(show);
}

int GameDetailsHeader::HeroHeight(int width) const {
    return width / 2;
}

QRect GameDetailsHeader::IconRect() const {
    return {SideMargin, HeroHeight(width()) + 1 - IconSize / 2, IconSize, IconSize};
}

bool GameDetailsHeader::hasHeightForWidth() const {
    return true;
}

int GameDetailsHeader::heightForWidth(int width) const {
    const int hero_height = HeroHeight(width);
    const int text_x = SideMargin + IconSize + 12;
    const int text_width = std::max(1, width - text_x - SideMargin);
    int text_height = title->heightForWidth(text_width);
    if (!developer->text().isEmpty()) {
        text_height += 2 + developer->heightForWidth(text_width);
    }
    return std::max(hero_height + IconSize / 2, hero_height + 8 + text_height) + 4;
}

QSize GameDetailsHeader::sizeHint() const {
    const int hint_width = width() > 0 ? width() : GameDetailsPanel::PanelWidth;
    return {GameDetailsPanel::PanelWidth, heightForWidth(hint_width)};
}

void GameDetailsHeader::LayoutLabels() {
    const int hero_height = HeroHeight(width());
    const int text_x = SideMargin + IconSize + 12;
    const int text_width = std::max(1, width() - text_x - SideMargin);
    const int title_height = title->heightForWidth(text_width);
    title->setGeometry(text_x, hero_height + 8, text_width, title_height);
    developer->setGeometry(text_x, hero_height + 10 + title_height, text_width,
                           developer->heightForWidth(text_width));

    close_button->move(width() - OverlayButtonSize - SideMargin, SideMargin / 2);
    banner_edit->move(close_button->x() - OverlayButtonSize - 8, close_button->y());

    // Sits inside the icon's bottom-right corner. Hung off the corner itself it ended up past
    // the header's bottom edge, which cut the button in half.
    const QRect icon_rect = IconRect();
    icon_edit->move(icon_rect.right() + 1 - OverlayButtonSize - 4,
                    icon_rect.bottom() + 1 - OverlayButtonSize - 4);
    updateGeometry();
}

void GameDetailsHeader::resizeEvent(QResizeEvent* event) {
    LayoutLabels();
    QWidget::resizeEvent(event);
}

void GameDetailsHeader::RefreshGlyphs() {
    const qreal dpr = devicePixelRatioF();
    const QColor color = palette().color(QPalette::WindowText);

    close_button->setIcon(HomeGlyphs::Close(color, dpr, 20));
    // The brush the home screen already uses to mean "change the artwork", so both panels ask
    // for it with the same symbol.
    banner_edit->setIcon(HomeGlyphs::Brush(color, dpr, 20));
    icon_edit->setIcon(HomeGlyphs::Brush(color, dpr, 20));
    // The glyphs are palette-derived: repaint the buttons that paint them.
    for (QToolButton* button : {close_button, banner_edit, icon_edit}) {
        button->update();
    }
}

void GameDetailsHeader::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange && developer) {
        RefreshDeveloperPalette();
        // Setting child style sheets while Qt is still applying the change crashes it, so the
        // glyphs and button surfaces follow on the next event loop turn.
        if (!glyph_refresh_queued) {
            glyph_refresh_queued = true;
            QMetaObject::invokeMethod(
                this,
                [this] {
                    glyph_refresh_queued = false;
                    RefreshGlyphs();
                },
                Qt::QueuedConnection);
        }
    }
    QWidget::changeEvent(event);
}

void GameDetailsHeader::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const QPalette& pal = palette();
    const QColor window = pal.color(QPalette::Window);
    const QRect hero_rect(0, 0, width(), HeroHeight(width()));

    if (!hero.isNull()) {
        HomeBackground::PaintCover(p, hero_rect, hero);
    } else if (!fallback_hero.isNull()) {
        HomeBackground::PaintCover(p, hero_rect, fallback_hero);
        p.fillRect(hero_rect, WithAlpha(window, 70));
    } else {
        QLinearGradient wash(hero_rect.topLeft(), hero_rect.bottomRight());
        wash.setColorAt(0, WithAlpha(pal.color(QPalette::Highlight), 150));
        wash.setColorAt(1, WithAlpha(pal.color(QPalette::Highlight), 40));
        p.fillRect(hero_rect, window);
        p.fillRect(hero_rect, wash);
    }

    // Fade the banner into the panel so the title area reads as one surface.
    QLinearGradient fade(0, hero_rect.top(), 0, hero_rect.bottom() + 1);
    fade.setColorAt(0.55, WithAlpha(window, 0));
    fade.setColorAt(1.0, window);
    p.fillRect(hero_rect, fade);

    if (busy) {
        const QString text = tr("Searching SteamGridDB...");
        const QFontMetrics fm(font());
        const QSize pill(fm.horizontalAdvance(text) + 24, fm.height() + 10);
        const QRectF pill_rect(QPointF((width() - pill.width()) / 2.0,
                                       (hero_rect.height() - pill.height()) / 2.0),
                               pill);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 160));
        p.drawRoundedRect(pill_rect, pill_rect.height() / 2, pill_rect.height() / 2);
        p.setPen(Qt::white);
        p.drawText(pill_rect, Qt::AlignCenter, text);
    }

    const QRectF icon_rect(IconRect());
    QPainterPath clip;
    clip.addRoundedRect(icon_rect, 14, 14);
    p.save();
    p.setClipPath(clip);
    p.fillRect(icon_rect, pal.color(QPalette::Base));
    if (!icon.isNull()) {
        p.drawPixmap(icon_rect.toRect(), icon);
    }
    p.restore();
    p.setPen(QPen(window, 3));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(icon_rect.adjusted(-1.5, -1.5, 1.5, 1.5), 15.5, 15.5);
}

PathLinkLabel::PathLinkLabel(QWidget* parent) : QLabel{parent} {
    setTextFormat(Qt::RichText);
    setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setCursor(Qt::PointingHandCursor);
}

void PathLinkLabel::SetPath(const QString& path_) {
    path = path_;
    setToolTip(QDir::toNativeSeparators(path));
    Elide();
}

QSize PathLinkLabel::minimumSizeHint() const {
    return {0, fontMetrics().height()};
}

void PathLinkLabel::resizeEvent(QResizeEvent* event) {
    QLabel::resizeEvent(event);
    Elide();
}

void PathLinkLabel::Elide() {
    const QString shown =
        fontMetrics().elidedText(QDir::toNativeSeparators(path), Qt::ElideMiddle, width());
    setText(QStringLiteral("<a href=\"open\">%1</a>").arg(shown.toHtmlEscaped()));
}

GameDetailsPanel::GameDetailsPanel(QWidget* parent) : HomeSidePanel{parent} {
    QWidget* scroll_content = Content();
    auto* content_layout = new QVBoxLayout(scroll_content);
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);

    header = new GameDetailsHeader(scroll_content);
    connect(header, &GameDetailsHeader::CloseRequested, this, &GameDetailsPanel::CloseRequested);
    content_layout->addWidget(header);

    auto* body = new QVBoxLayout;
    body->setContentsMargins(SideMargin, 10, SideMargin, SideMargin);
    body->setSpacing(8);
    content_layout->addLayout(body);

    // Chips
    chip_row = new FlowLayout(6);
    body->addLayout(chip_row);
    body->addSpacing(10);

    // Updates and DLC
    add_ons_heading = MakeHeading(scroll_content);
    body->addWidget(add_ons_heading);
    add_ons_box = new QWidget(scroll_content);
    add_ons_layout = new QVBoxLayout(add_ons_box);
    add_ons_layout->setContentsMargins(0, 0, 0, 0);
    add_ons_layout->setSpacing(4);
    body->addWidget(add_ons_box);
    body->addSpacing(10);

    // Shaders
    shaders_heading = MakeHeading(scroll_content);
    body->addWidget(shaders_heading);
    shaders_box = new QWidget(scroll_content);
    auto* shaders_grid = new QGridLayout(shaders_box);
    shaders_grid->setContentsMargins(0, 0, 0, 0);
    shaders_grid->setHorizontalSpacing(12);
    shaders_grid->setVerticalSpacing(6);
    shaders_grid->setColumnStretch(1, 1);
    pipeline_cache_key = new QLabel(shaders_box);
    pipeline_cache_value = new QLabel(shaders_box);
    precompiled_key = new QLabel(shaders_box);
    precompiled_value = new QLabel(shaders_box);
    precompile_button = new QPushButton(shaders_box);
    precompile_button->setCursor(Qt::PointingHandCursor);
    connect(precompile_button, &QPushButton::clicked, this,
            &GameDetailsPanel::PrecompileShadersRequested);
    shaders_grid->addWidget(pipeline_cache_key, 0, 0);
    shaders_grid->addWidget(pipeline_cache_value, 0, 1);
    shaders_grid->addWidget(precompiled_key, 1, 0);
    shaders_grid->addWidget(precompiled_value, 1, 1);
    shaders_grid->addWidget(precompile_button, 2, 0, 1, 2, Qt::AlignLeft);
    body->addWidget(shaders_box);
    body->addSpacing(10);

    // Technical details, collapsed by default
    technical_toggle = new QToolButton(scroll_content);
    technical_toggle->setCheckable(true);
    technical_toggle->setAutoRaise(true);
    technical_toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    technical_toggle->setArrowType(Qt::RightArrow);
    technical_toggle->setCursor(Qt::PointingHandCursor);
    // Full width so the header is one row across the panel rather than a label plus a stray arrow.
    technical_toggle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFont toggle_font = technical_toggle->font();
    toggle_font.setBold(true);
    toggle_font.setPointSizeF(toggle_font.pointSizeF() * 1.15);
    technical_toggle->setFont(toggle_font);
    body->addWidget(technical_toggle);

    technical_box = new QWidget(scroll_content);
    info_grid = new QGridLayout(technical_box);
    info_grid->setContentsMargins(0, 0, 0, 0);
    info_grid->setHorizontalSpacing(12);
    info_grid->setVerticalSpacing(6);
    info_grid->setColumnStretch(1, 1);
    technical_box->hide();
    body->addWidget(technical_box);
    connect(technical_toggle, &QToolButton::toggled, this, [this](bool expanded) {
        technical_toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        technical_box->setVisible(expanded);
    });

    body->addStretch(1);

    // Action bar, pinned below the scrolled content
    auto* action_bar = new QWidget(this);
    auto* actions = new QHBoxLayout(action_bar);
    actions->setContentsMargins(SideMargin, 10, SideMargin, 12);
    actions->setSpacing(8);
    play_button = new QPushButton(action_bar);
    play_button->setFixedHeight(ActionButtonHeight);
    play_button->setIconSize(QSize(18, 18));
    play_button->setCursor(Qt::PointingHandCursor);
    QFont play_font = play_button->font();
    play_font.setBold(true);
    play_button->setFont(play_font);
    connect(play_button, &QPushButton::clicked, this, &GameDetailsPanel::PlayRequested);
    actions->addWidget(play_button, 1);

    const auto make_action = [&] {
        auto* button = new IconButton(IconButton::Kind::Outline, ActionButtonHeight, 18, action_bar);
        actions->addWidget(button);
        return button;
    };
    configure_button = make_action();
    connect(configure_button, &QToolButton::clicked, this, &GameDetailsPanel::ConfigureRequested);
    favorite_button = make_action();
    connect(favorite_button, &QToolButton::clicked, this, &GameDetailsPanel::FavoriteToggled);
    more_button = make_action();
    connect(more_button, &QToolButton::clicked, this, [this] {
        emit MoreRequested(more_button->mapToGlobal(QPoint(0, more_button->height())));
    });
    SetFooter(action_bar);

    // Artwork menus on the header's edit buttons
    banner_menu = new QMenu(this);
    icon_menu = new QMenu(this);
    for (QMenu* menu : {banner_menu, icon_menu}) {
        const bool hero = menu == banner_menu;
        menu->setToolTipsVisible(true);
        connect(menu, &QMenu::aboutToShow, this, [this, menu, hero] {
            menu->clear();
            QAction* find = menu->addAction(tr("Find on SteamGridDB..."));
            if (UISettings::values.steamgriddb_api_key.GetValue().empty()) {
                find->setToolTip(tr("Needs a free SteamGridDB API key. You will be asked for it, "
                                    "or set it in Settings > General > Web."));
            }
            connect(find, &QAction::triggered, this, [this, hero] { FindArt(hero); });
            connect(menu->addAction(tr("Choose File...")), &QAction::triggered, this,
                    [this, hero] { ChooseArtFile(hero); });
            const QString path = hero ? QtCommon::GameArt::HeroPath(game.program_id)
                                      : QtCommon::GameArt::IconPath(game.program_id);
            QAction* reset = menu->addAction(hero ? tr("Remove Banner") : tr("Reset Icon"));
            reset->setEnabled(QFileInfo::exists(path));
            connect(reset, &QAction::triggered, this, [this, hero] { ResetArt(hero); });
        });
    }
    header->BannerEditButton()->setMenu(banner_menu);
    header->IconEditButton()->setMenu(icon_menu);

    icon_search = new IconSearch(this);
    connect(icon_search, &IconSearch::RunningChanged, header, &GameDetailsHeader::SetBusy);
    connect(icon_search, &IconSearch::IconSelected, this,
            [this](const QByteArray& png, const QString&) { SaveIcon(search_program_id, png); });
    connect(icon_search, &IconSearch::HeroSelected, this,
            [this](const QByteArray& image) { SaveHero(search_program_id, image); });

    RetranslateUI();
    RefreshGlyphs();

}

GameDetailsPanel::~GameDetailsPanel() = default;

void GameDetailsPanel::SetGame(const GameDetailsInfo& info) {
    game = info;
    extras = GameDetailsExtras{.program_id = info.program_id, .path = info.path};
    extras_loaded = false;

    const bool has_id = game.program_id != 0;
    header->SetEditable(has_id);
    add_ons_heading->setVisible(has_id);
    add_ons_box->setVisible(has_id);
    shaders_heading->setVisible(has_id);
    shaders_box->setVisible(has_id);

    ReloadHero();
    SetFavorite(info.favorite);
    RefreshChips();
    RefreshInfo();
    RefreshAddOns();
    RefreshShaders();
}

void GameDetailsPanel::SetExtras(const GameDetailsExtras& extras_) {
    if (extras_.program_id != game.program_id || extras_.path != game.path) {
        return;
    }
    extras = extras_;
    extras_loaded = true;
    RefreshChips();
    RefreshAddOns();
    RefreshShaders();
}

void GameDetailsPanel::SetFavorite(bool favorite) {
    game.favorite = favorite;
    favorite_button->setToolTip(favorite ? tr("Remove from Favorites") : tr("Add to Favorites"));
    RefreshGlyphs();
}

void GameDetailsPanel::SetIcon(const QPixmap& icon) {
    game.icon = icon;
    header->SetIcon(icon);
}

QImage GameDetailsPanel::LoadHero(u64 program_id) {
    if (program_id == 0) {
        return {};
    }
    if (const QImage* cached = hero_cache.object(program_id)) {
        return *cached;
    }
    QImage image;
    const QString path = QtCommon::GameArt::HeroPath(program_id);
    if (QFileInfo::exists(path)) {
        // Decode at the size the banner is shown at, not the stored size.
        QImageReader reader(path);
        const QSize full = reader.size();
        const int target = static_cast<int>(PanelWidth * devicePixelRatioF() * 1.25);
        if (full.isValid() && full.width() > target) {
            reader.setScaledSize(QSize(target, full.height() * target / full.width()));
        }
        image = reader.read();
    }
    hero_cache.insert(program_id, new QImage(image),
                      std::max<qsizetype>(1, image.sizeInBytes() / 1024));
    return image;
}

void GameDetailsPanel::ReloadHero() {
    header->SetGame(game, LoadHero(game.program_id));
}

void GameDetailsPanel::RefreshChips() {
    ClearLayout(chip_row);

    const QColor text = palette().color(QPalette::WindowText);
    const QColor accent = palette().color(QPalette::Highlight);
    const auto add_chip = [&](const QString& label, bool highlighted, const QString& tooltip) {
        auto* chip = new QLabel(label, Content());
        chip->setToolTip(tooltip);
        chip->setStyleSheet(
            QStringLiteral("QLabel { border-radius: 10px; padding: 3px 10px; background: %1; }")
                .arg(Rgba(highlighted ? WithAlpha(accent, 70) : WithAlpha(text, 22))));
        chip_row->addWidget(chip);
    };

    add_chip(game.play_time.isEmpty() ? tr("Never played") : tr("%1 played").arg(game.play_time),
             false, tr("Play time"));
    if (!game.size.isEmpty()) {
        add_chip(game.size, false, tr("Size on disk"));
    }
    if (extras_loaded && extras.custom_config) {
        add_chip(tr("Custom settings"), true, tr("This game has its own configuration"));
    }
}

void GameDetailsPanel::RefreshInfo() {
    ClearLayout(info_grid);
    location_label = nullptr;

    int row = 0;
    const auto add_row = [&](const QString& key, QWidget* value) {
        auto* key_label = new QLabel(key, technical_box);
        SetMuted(key_label);
        key_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        info_grid->addWidget(key_label, row, 0);
        info_grid->addWidget(value, row, 1);
        ++row;
    };
    const auto add_text = [&](const QString& key, const QString& value) {
        if (value.trimmed().isEmpty()) {
            return;
        }
        auto* value_label = new QLabel(value, technical_box);
        value_label->setWordWrap(true);
        value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        add_row(key, value_label);
    };

    add_text(tr("Version"), game.version);
    if (game.program_id != 0) {
        add_text(tr("Title ID"),
                 QStringLiteral("%1").arg(game.program_id, 16, 16, QLatin1Char('0')).toUpper());
    }
    add_text(tr("Format"), game.file_type);
    if (!game.path.isEmpty()) {
        location_label = new PathLinkLabel(technical_box);
        location_label->SetPath(QFileInfo(game.path).absolutePath());
        location_label->setToolTip(tr("Open %1").arg(QDir::toNativeSeparators(game.path)));
        connect(location_label, &QLabel::linkActivated, this,
                &GameDetailsPanel::OpenLocationRequested);
        add_row(tr("Location"), location_label);
    }
}

void GameDetailsPanel::RefreshAddOns() {
    ClearLayout(add_ons_layout);
    add_on_checks.clear();

    const auto add_note = [&](const QString& text) {
        auto* note = new QLabel(text, add_ons_box);
        note->setWordWrap(true);
        SetMuted(note);
        add_ons_layout->addWidget(note);
    };
    if (!extras_loaded) {
        add_note(tr("Loading..."));
        return;
    }
    if (extras.add_ons.empty()) {
        add_note(tr("No updates, DLC or mods installed."));
        return;
    }

    for (size_t i = 0; i < extras.add_ons.size(); ++i) {
        const GameAddOn& add_on = extras.add_ons[i];
        auto* row = new QWidget(add_ons_box);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(8);

        auto* version = new QLabel(row);
        version->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        version->setText(version->fontMetrics().elidedText(add_on.version, Qt::ElideRight,
                                                           (PanelWidth - 2 * SideMargin) * 2 / 5));
        version->setToolTip(add_on.version);
        SetMuted(version);

        auto* check = new ElidedCheckBox(add_on.name, row);
        check->setChecked(add_on.enabled);
        check->setCursor(Qt::PointingHandCursor);
        connect(check, &QCheckBox::toggled, this,
                [this, i](bool enabled) { OnAddOnToggled(i, enabled); });
        add_on_checks.push_back(check);

        row_layout->addWidget(check, 1);
        row_layout->addWidget(version);
        add_ons_layout->addWidget(row);
    }
}

void GameDetailsPanel::OnAddOnToggled(size_t index, bool enabled) {
    if (index >= extras.add_ons.size()) {
        return;
    }
    extras.add_ons[index].enabled = enabled;
    // Only one update can be active at a time.
    if (enabled && extras.add_ons[index].is_update) {
        for (size_t i = 0; i < extras.add_ons.size(); ++i) {
            if (i != index && extras.add_ons[i].is_update && extras.add_ons[i].enabled) {
                extras.add_ons[i].enabled = false;
                const QSignalBlocker blocker(add_on_checks[i]);
                add_on_checks[i]->setChecked(false);
            }
        }
    }

    QStringList disabled;
    for (const GameAddOn& add_on : extras.add_ons) {
        if (!add_on.enabled) {
            disabled << add_on.key;
        }
    }
    emit DisabledAddOnsChanged(disabled);
}

void GameDetailsPanel::RefreshShaders() {
    if (!extras_loaded) {
        pipeline_cache_value->setText(tr("..."));
        precompiled_value->setText(tr("..."));
        precompile_button->setText(tr("Pre-compile Shaders"));
        return;
    }
    pipeline_cache_value->setText(
        extras.pipeline_cache_bytes == 0
            ? tr("None yet")
            : QLocale().formattedDataSize(static_cast<qint64>(extras.pipeline_cache_bytes)));
    precompiled_value->setText(extras.shaders_precompiled ? tr("Yes") : tr("No"));
    precompile_button->setText(extras.shaders_precompiled ? tr("Pre-compile Again")
                                                          : tr("Pre-compile Shaders"));
}

void GameDetailsPanel::FindArt(bool hero) {
    if (icon_search->IsRunning()) {
        return;
    }
    searching_hero = hero;
    search_program_id = game.program_id;
    icon_search->StartSteamGridDb(game.title, game.program_id,
                                  hero ? SgdbArtKind::Hero : SgdbArtKind::Icon);
}

void GameDetailsPanel::ChooseArtFile(bool hero) {
    const QString path =
        QFileDialog::getOpenFileName(this, hero ? tr("Choose Banner") : tr("Choose Icon"), {},
                                     tr("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (path.isEmpty()) {
        return;
    }
    const QImage image(path);
    if (image.isNull()) {
        QMessageBox::warning(
            this, tr("Artwork"),
            tr("\"%1\" could not be loaded as an image.").arg(QFileInfo(path).fileName()));
        return;
    }
    if (hero) {
        SaveHero(game.program_id, EncodeHeroImage(image));
    } else {
        SaveIcon(game.program_id, EncodeIconImage(image));
    }
}

void GameDetailsPanel::ResetArt(bool hero) {
    if (hero) {
        QtCommon::GameArt::RemoveHero(game.program_id);
        hero_cache.remove(game.program_id);
        ReloadHero();
        emit HeroChanged(game.program_id);
    } else {
        QtCommon::GameArt::RemoveIcon(game.program_id);
        emit IconChanged(game.program_id);
    }
}

void GameDetailsPanel::SaveHero(u64 program_id, const QByteArray& image) {
    if (!QtCommon::GameArt::SaveHero(program_id, image)) {
        QMessageBox::warning(this, tr("Artwork"), tr("The banner could not be saved."));
        return;
    }
    hero_cache.remove(program_id);
    if (program_id == game.program_id) {
        ReloadHero();
    }
    emit HeroChanged(program_id);
}

void GameDetailsPanel::SaveIcon(u64 program_id, const QByteArray& png) {
    if (!QtCommon::GameArt::SaveIcon(program_id, png)) {
        QMessageBox::warning(this, tr("Artwork"), tr("The icon could not be saved."));
        return;
    }
    emit IconChanged(program_id);
}

void GameDetailsPanel::RefreshGlyphs() {
    const QPalette& pal = palette();
    const qreal dpr = devicePixelRatioF();
    const QColor text = pal.color(QPalette::ButtonText);
    const QColor accent = pal.color(QPalette::Highlight);

    // The style sheet, not the widget, decides the height of a styled button, so the action bar
    // height has to be stated here as well or Play shrinks to its text.
    play_button->setIcon(MakeGlyph(Glyph::Play, pal.color(QPalette::HighlightedText), dpr));
    play_button->setStyleSheet(
        QStringLiteral("QPushButton { border: none; border-radius: 8px; padding: 0 14px; "
                       "min-height: %1px; max-height: %1px; background: palette(highlight); "
                       "color: palette(highlighted-text); } "
                       "QPushButton:hover { background: %2; } "
                       "QPushButton:pressed { background: %3; }")
            .arg(QString::number(ActionButtonHeight), Rgba(accent.lighter(112)),
                 Rgba(accent.darker(112))));

    configure_button->setIcon(MakeGlyph(Glyph::Configure, text, dpr));
    favorite_button->setIcon(game.favorite ? MakeGlyph(Glyph::StarFilled, accent, dpr)
                                           : MakeGlyph(Glyph::StarOutline, text, dpr));
    more_button->setIcon(MakeGlyph(Glyph::More, text, dpr));

    for (QLabel* key : {pipeline_cache_key, precompiled_key}) {
        SetMuted(key);
    }

    // Like the action bar, the styles are palette-derived and re-applied here so a theme switch
    // repaints them instead of leaving colours from the previous palette behind.
    precompile_button->setStyleSheet(BodyButtonStyle(pal));
    technical_toggle->setStyleSheet(SectionToggleStyle(pal));
}

void GameDetailsPanel::RetranslateUI() {
    play_button->setText(tr("Play"));
    play_button->setToolTip(tr("Start the game (Enter)"));
    configure_button->setToolTip(tr("Configure Game"));
    more_button->setToolTip(tr("More Actions"));
    favorite_button->setToolTip(game.favorite ? tr("Remove from Favorites")
                                              : tr("Add to Favorites"));
    header->BannerEditButton()->setToolTip(tr("Change Banner"));
    header->IconEditButton()->setToolTip(tr("Change Icon"));
    add_ons_heading->setText(tr("Updates & DLC"));
    shaders_heading->setText(tr("Shaders"));
    pipeline_cache_key->setText(tr("Pipeline cache"));
    precompiled_key->setText(tr("Pre-compiled"));
    precompile_button->setToolTip(
        tr("Compile this game's shaders ahead of time to reduce stutter while playing"));
    technical_toggle->setText(tr("Technical Details"));
    RefreshInfo();
    RefreshChips();
    RefreshAddOns();
    RefreshShaders();
}

void GameDetailsPanel::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        emit PlayRequested();
        return;
    }
    HomeSidePanel::keyPressEvent(event);
}

void GameDetailsPanel::OnLanguageChanged() {
    RetranslateUI();
}

void GameDetailsPanel::OnPaletteChanged() {
    RefreshGlyphs();
    RefreshChips();
    RefreshInfo();
    RefreshAddOns();
}
