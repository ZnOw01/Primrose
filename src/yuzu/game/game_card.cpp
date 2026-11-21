// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QTextLayout>
#include "game_card.h"
#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"

GameCard::GameCard(QObject* parent) : QStyledItemDelegate{parent} {
    setObjectName("GameCard");
}

namespace {

constexpr int MaxTitleLines = 2;
/// How far the icon's drop shadow extends past the icon on each side.
constexpr int ShadowSpread = 6;

} // namespace

/// The platform border for a game icon, scaled to `size` logical pixels and loaded once per
/// process. Scaling a 1024x1024 pixmap on every card paint is far too slow to scroll at 60 fps,
/// so the result is cached per size and device pixel ratio.
QPixmap GameCard::PlatformBorderPixmap(bool is_switch2, int size, qreal dpr) {
    const QString key = QStringLiteral("eglantine-border-%1-%2-%3")
                            .arg(is_switch2 ? QStringLiteral("switch2") : QStringLiteral("switch1"))
                            .arg(size)
                            .arg(dpr);

    QPixmap border;
    if (QPixmapCache::find(key, &border)) {
        return border;
    }

    border.load(is_switch2 ? QStringLiteral(":/game_borders/switch2.png")
                           : QStringLiteral(":/game_borders/switch1.png"));
    if (border.isNull()) {
        return border;
    }

    const int pixels = std::max(1, qRound(size * dpr));
    border = border.scaled(pixels, pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    border.setDevicePixelRatio(dpr);
    QPixmapCache::insert(key, border);
    return border;
}

QPixmap GameCard::CartridgePixmap(int height_px, qreal dpr) {
    // Quantized to 32-pixel buckets so a live slide hits the cache instead of re-rendering.
    const int bucket = std::max(32, (std::max(1, height_px) + 31) / 32 * 32);
    const QString key =
        QStringLiteral("eglantine-cartridge-%1-%2").arg(bucket).arg(dpr);

    QPixmap cartridge;
    if (QPixmapCache::find(key, &cartridge)) {
        return cartridge;
    }

    QImageReader reader(QStringLiteral(":/game_borders/game_cartridge.svg"));
    reader.setScaledSize(
        QSize(qRound(bucket * CartridgeAspect * dpr), qRound(bucket * dpr)));
    const QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    // Kept at raw device pixels: callers scale it to the exact card rect themselves, which
    // keeps the bucket quantization out of the draw path at any device pixel ratio.
    cartridge = QPixmap::fromImage(image);
    QPixmapCache::insert(key, cartridge);
    return cartridge;
}

QRectF GameCard::CartridgeLabelRect(const QRectF& cartridge_rect) {
    const qreal sx = cartridge_rect.width() / CartridgeBase.width();
    const qreal sy = cartridge_rect.height() / CartridgeBase.height();
    return {cartridge_rect.left() + CartridgeLabel.x() * sx,
            cartridge_rect.top() + CartridgeLabel.y() * sy,
            CartridgeLabel.width() * sx, CartridgeLabel.height() * sy};
}

QRectF GameCard::CartridgeBodyRect(const QRectF& cartridge_rect) {
    const qreal sx = cartridge_rect.width() / CartridgeBase.width();
    const qreal sy = cartridge_rect.height() / CartridgeBase.height();
    return {cartridge_rect.left() + CartridgeBody.x() * sx,
            cartridge_rect.top() + CartridgeBody.y() * sy,
            CartridgeBody.width() * sx, CartridgeBody.height() * sy};
}

void GameCard::DrawCoverCropped(QPainter& painter, const QPixmap& pixmap,
                                const QRectF& target) {
    if (pixmap.isNull() || target.isEmpty()) {
        return;
    }
    // Center-crop to the target's aspect: the source region keeps the target's proportions.
    const qreal scale =
        std::max(target.width() / pixmap.width(), target.height() / pixmap.height());
    const qreal src_w = target.width() / scale;
    const qreal src_h = target.height() / scale;
    const QRectF source((pixmap.width() - src_w) / 2, (pixmap.height() - src_h) / 2, src_w,
                       src_h);
    painter.drawPixmap(target, pixmap, source);
}

namespace {

/// The soft drop shadow drawn under a game icon: six stacked translucent rounded rects, rendered
/// once per icon size and device pixel ratio. Drawing them per card per frame costs about as much
/// as the whole rest of the card, which is enough to drop scrolling below 60 fps.
QPixmap GetIconShadow(const QSize& icon_size, qreal dpr) {
    constexpr int ShadowShift = 3;

    const QString key = QStringLiteral("eglantine-icon-shadow-%1x%2-%3")
                            .arg(icon_size.width())
                            .arg(icon_size.height())
                            .arg(dpr);

    QPixmap shadow;
    if (QPixmapCache::find(key, &shadow)) {
        return shadow;
    }

    const QSize extent(icon_size.width() + 2 * ShadowSpread,
                       icon_size.height() + 2 * ShadowSpread + ShadowShift);
    shadow = QPixmap(extent * dpr);
    shadow.setDevicePixelRatio(dpr);
    shadow.fill(Qt::transparent);

    QPainter painter(&shadow);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 14));

    const qreal icon_radius = icon_size.width() * 0.08;
    for (int spread = 1; spread <= ShadowSpread; ++spread) {
        const QRectF rect(ShadowSpread - spread, ShadowSpread - spread + ShadowShift,
                          icon_size.width() + spread * 2, icon_size.height() + spread * 2);
        painter.drawRoundedRect(rect, icon_radius + spread, icon_radius + spread);
    }
    painter.end();

    QPixmapCache::insert(key, shadow);
    return shadow;
}

/// Wraps `text` into at most `max_lines` centred lines, eliding the last one if it overflows.
void DrawTitle(QPainter* painter, const QRect& rect, const QString& text, const QFont& font,
               int max_lines) {
    const QFontMetrics metrics(font);
    QTextLayout layout(text, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);

    QStringList lines;
    layout.beginLayout();
    while (lines.size() < max_lines) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(rect.width());
        if (lines.size() == max_lines - 1) {
            lines << metrics.elidedText(text.mid(line.textStart()).trimmed(), Qt::ElideRight,
                                        rect.width());
        } else {
            lines << text.mid(line.textStart(), line.textLength()).trimmed();
        }
    }
    layout.endLayout();

    int y = rect.top();
    for (const QString& line : std::as_const(lines)) {
        if (y + metrics.height() > rect.bottom() + 1) {
            break;
        }
        painter->drawText(QRect(rect.left(), y, rect.width(), metrics.height()),
                          Qt::AlignHCenter | Qt::AlignTop, line);
        y += metrics.lineSpacing();
    }
}

} // namespace

void GameCard::paint(QPainter* painter, const QStyleOptionViewItem& option,
                     const QModelIndex& index) const {
    if (!index.isValid())
        return;

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    constexpr int cardCornerRadius = 18;

    const QRect cardRect = getCardRect(option, index);
    const QPalette& palette = option.palette;
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;

    // No card frame: only hover and selection get a soft backdrop.
    if (selected || hovered) {
        QColor backdrop = palette.highlight().color();
        backdrop.setAlpha(selected ? 110 : 45);
        painter->setPen(Qt::NoPen);
        painter->setBrush(backdrop);
        painter->drawRoundedRect(cardRect, cardCornerRadius, cardCornerRadius);
    }

    const u32 icon_size = UISettings::values.game_icon_size.GetValue();
    QPixmap icon_pixmap = index.data(Qt::DecorationRole).value<QPixmap>();

    QRect iconRect;
    qreal icon_radius = 0.0;
    if (!icon_pixmap.isNull() && icon_size > 0) {
        const qreal dpr = painter->device() ? painter->device()->devicePixelRatio() : 1.0;

        if (UISettings::values.game_icon_borders.GetValue() &&
            UISettings::values.game_cartridge_kind.GetValue() == 1) {
            // The cartridge frame is portrait and replaces the square icon block: the game
            // art sits in its label area, cover-cropped, and the frame goes on top.
            const int cart_w = qRound(icon_size * CartridgeAspect);
            iconRect = {cardRect.left() + (cardRect.width() - cart_w) / 2,
                        cardRect.top() + cardMargin - 1, cart_w, static_cast<int>(icon_size)};
            icon_radius = cart_w * 0.06;

            const QPixmap frame = CartridgePixmap(static_cast<int>(icon_size), dpr);
            if (!frame.isNull()) {
                painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

                const QPixmap shadow = GetIconShadow(iconRect.size(), dpr);
                painter->drawPixmap(iconRect.topLeft() - QPoint(ShadowSpread, ShadowSpread),
                                    shadow);

                // The frame goes down first: its label area carries the label design, and
                // the game art sits on top of it, cover-cropped.
                painter->drawPixmap(iconRect, frame);

                const QRectF label = CartridgeLabelRect(QRectF(iconRect));
                painter->save();
                QPainterPath label_clip;
                label_clip.addRoundedRect(label, label.width() * 0.04, label.width() * 0.04);
                painter->setClipPath(label_clip);
                DrawCoverCropped(*painter, icon_pixmap, label);
                painter->restore();
            }
        } else {
            QSize scaled = icon_pixmap.size();
            scaled.scale(icon_size, icon_size, Qt::KeepAspectRatio);

            iconRect = {cardRect.left() + (cardRect.width() - scaled.width()) / 2,
                        cardRect.top() + cardMargin - 1, scaled.width(), scaled.height()};
            icon_radius = iconRect.width() * 0.08;

            painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

            // Soft drop shadow, pre-rendered: darkest under the icon's centre.
            const QPixmap shadow = GetIconShadow(iconRect.size(), dpr);
            painter->drawPixmap(iconRect.topLeft() - QPoint(ShadowSpread, ShadowSpread), shadow);

            painter->save();
            QPainterPath clip_path;
            clip_path.addRoundedRect(iconRect, icon_radius, icon_radius);
            painter->setClipPath(clip_path);
            painter->drawPixmap(iconRect, icon_pixmap);
            painter->restore();

            // The platform border sits on top of the icon; its centre is transparent.
            if (UISettings::values.game_icon_borders.GetValue()) {
                const QPixmap border = GameCard::PlatformBorderPixmap(
                    index.data(GameListItemPath::Switch2Role).toBool(), iconRect.width(), dpr);
                if (!border.isNull()) {
                    painter->drawPixmap(iconRect, border);
                }
            }
        }

        if (selected) {
            painter->setPen(QPen(palette.highlight().color(), 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(QRectF(iconRect).adjusted(-1, -1, 1, 1), icon_radius,
                                     icon_radius);
        }
    } else {
        iconRect = {cardRect.left() + cardMargin, cardRect.top() + cardMargin,
                    static_cast<int>(icon_size), static_cast<int>(icon_size)};
    }

    if (UISettings::values.show_game_name.GetValue()) {
        QRect textRect = cardRect;
        textRect.setTop(iconRect.bottom() + cardMargin);
        textRect.adjust(cardMargin / 2, 0, -cardMargin / 2, -cardMargin / 2);

        const QString title =
            index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n')).first();

        QFont font = option.font;
        font.setBold(true);
        font.setPixelSize(std::max(11.0, std::sqrt(static_cast<double>(icon_size))));
        painter->setFont(font);
        painter->setPen(palette.text().color());

        DrawTitle(painter, textRect, title, font, MaxTitleLines);
    }

    painter->restore();
}

QRect GameCard::getCardRect(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const int cell_width = option.rect.width();

    const int card_width = cell_width - m_padding;

    int card_left, card_top, card_height;

    if (m_columns >= 1) {
        // grid mode

        // center everything in-line, such that the leftmost and rightmost cards
        // have ~ equal padding to the edge of the viewport
        // spacing between each card is larger, but equal to each other
        const int column = index.row() % m_columns;

        const int row_width = m_columns * cell_width;
        const int total_gap = row_width - cardMargin * 2 - m_columns * card_width;
        const int gap = (m_columns > 1) ? (total_gap / (m_columns - 1)) : 0;
        card_left =
            option.rect.left() - column * cell_width + cardMargin + column * (card_width + gap) + 4;

        // fill cell vertically
        card_top = option.rect.top() + cardMargin;
        card_height = option.rect.height() - cardMargin - 1;
    } else {
        // carousel mode
        card_left = option.rect.left() + cardMargin + 4;

        // the delegate itself takes up the full height, but the card itself
        // gets centered
        const int content_height = m_contentSize.height() - cardMargin;
        const int cell_height = option.rect.height();

        card_height = std::min(content_height, cell_height - cardMargin * 2) - 1;
        card_top = option.rect.top() + (cell_height - card_height) / 2;
    }

    return QRect(card_left, card_top, card_width - cardMargin, card_height);
}

bool GameCard::hitTest(const QPoint& point, const QModelIndex& index, const QWidget* widget,
                       const QRect& cellRect) const {
    QStyleOptionViewItem option;
    option.initFrom(widget);
    option.rect = cellRect;
    return getCardRect(option, index).contains(point);
}

QSize GameCard::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const {
    return m_size;
}

void GameCard::setSize(const QSize& newSize, const QSize& contentSize, const int padding,
                       const int columns) {
    m_size = newSize;
    m_contentSize = contentSize;
    m_padding = padding;
    m_columns = columns;
}
