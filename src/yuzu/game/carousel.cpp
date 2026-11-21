// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/game/carousel.h"

#include <QEasingCurve>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPixmapCache>
#include <QPolygonF>
#include <QTransform>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <vector>

#include "qt_common/config/uisettings.h"
#include "qt_common/game_list/game_list_p.h"
#include "qt_common/game_list/model.h"
#include "qt_common/util/game_art.h"
#include "yuzu/game/common.h"
#include "yuzu/game/game_card.h"

namespace {

constexpr int SideGap = 16;
/// Room under the centered card for its title row, when game names are shown.
constexpr int TitleReserve = 44;
constexpr int MinCenterSize = 160;
/// How far each stacked card peeks past the card in front of it, as a share of the center card.
constexpr qreal PeekFraction = 0.12;
/// Size falloff per step into the stack, so the strip reads as receding.
constexpr qreal ScaleStep = 0.90;
constexpr qreal MinScale = 0.40;
/// Trapezoid tilt of the stacked cards: their far edge is this much narrower, and this much
/// shorter, than the edge facing the center.
constexpr qreal TiltSqueeze = 0.14;
constexpr qreal TiltHeight = 0.95;
/// Depth fog: the panel's Window colour at this alpha per step into the stack, the same wash the
/// details panel uses to fade banners into the surface.
constexpr int FogPerStep = 55;
constexpr int FogMax = 170;
/// The deepest stack the setting may ask for, whatever ends up in the config file.
constexpr int MaxSideCount = 12;

/// How opaque a stacked card is at `depth` with `count` cards a side shown: solid up close,
/// dissolving to nothing at the stack's edge. Cards fade out of existence as they recede and
/// fade back in at the back of the stack as they return.
qreal StackAlpha(qreal depth, int count) {
    const qreal d = std::abs(depth);
    const qreal solid = std::max<qreal>(1.0, count - 2.0);
    if (d <= solid) {
        return 1.0;
    }
    const qreal t = std::min<qreal>(1.0, (d - solid) / std::max<qreal>(1.0, count - solid));
    return 1.0 - t * t * (3.0 - 2.0 * t);
}
constexpr int FocusRingWidth = 2;
constexpr int AnimationMs = 220;
/// Icon sizes are quantized to this many pixels so a live window resize hits the scaling cache
/// instead of re-filtering every card at every pixel step.
constexpr int SizeBucket = 32;
/// Cached icon sources are capped in physical pixels: full-card-height entries would crowd Qt's
/// small default pixmap cache, and every eviction re-filters mid-animation. A card never needs
/// more detail than this; the last stretch to the card's size is a cheap per-frame smooth blit.
constexpr int MaxCenterSourcePx = 768;
constexpr int MaxStackSourcePx = 512;

QColor WithAlpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

int SizeBucketOf(qreal side) {
    return std::max(SizeBucket * 2, int(side) / SizeBucket * SizeBucket);
}

/// Mitchell-Netravali bicubic filter: crisper than the painter's bilinear when a small icon is
/// enlarged onto a full-height card, without the ringing sharper windows produce on hard edges.
qreal MitchellWeight(qreal x) {
    constexpr qreal b = 1.0 / 3.0;
    constexpr qreal c = 1.0 / 3.0;
    x = std::abs(x);
    if (x < 1.0) {
        return ((12 - 9 * b - 6 * c) * x * x * x + (-18 + 12 * b + 6 * c) * x * x +
                (6 - 2 * b)) / 6.0;
    }
    if (x < 2.0) {
        return ((-b - 6 * c) * x * x * x + (6 * b + 30 * c) * x * x + (-12 * b - 48 * c) * x +
                (8 * b + 24 * c)) / 6.0;
    }
    return 0.0;
}

struct FilterTaps {
    int start = 0;
    std::vector<qreal> weights;
};

std::vector<FilterTaps> BuildTaps(int in_count, int out_count) {
    const qreal ratio = qreal(in_count) / qreal(out_count);
    const qreal scale = std::max<qreal>(1.0, ratio);
    const int half = int(std::ceil(2.0 * scale));
    std::vector<FilterTaps> taps(out_count);
    for (int out = 0; out < out_count; ++out) {
        const qreal center = (out + 0.5) * ratio - 0.5;
        FilterTaps& t = taps[out];
        t.start = int(std::floor(center)) - half + 1;
        qreal sum = 0.0;
        for (int i = t.start; i < t.start + 2 * half; ++i) {
            // Negative lobes ring around sharp text and logo edges when a small icon is
            // enlarged, so the kernel is kept non-negative: a touch softer, but artifact-free.
            const qreal weight = std::max<qreal>(0.0, MitchellWeight((i - center) / scale));
            t.weights.push_back(weight);
            sum += weight;
        }
        if (sum > 0.0) {
            for (qreal& weight : t.weights) {
                weight /= sum;
            }
        }
    }
    return taps;
}

/// Two separable passes with edge clamping; runs once per icon and size, never per frame.
QImage MitchellResample(const QImage& source, const QSize& target) {
    if (source.size() == target) {
        return source.convertToFormat(QImage::Format_ARGB32);
    }
    const QImage src = source.convertToFormat(QImage::Format_ARGB32);

    QImage temp(target.width(), src.height(), QImage::Format_ARGB32);
    const std::vector<FilterTaps> h_taps = BuildTaps(src.width(), target.width());
    for (int y = 0; y < src.height(); ++y) {
        const auto* row_in = reinterpret_cast<const QRgb*>(src.scanLine(y));
        auto* row_out = reinterpret_cast<QRgb*>(temp.scanLine(y));
        for (int x = 0; x < target.width(); ++x) {
            const FilterTaps& t = h_taps[x];
            qreal r = 0.0, g = 0.0, b = 0.0, a = 0.0;
            for (size_t k = 0; k < t.weights.size(); ++k) {
                const int sx = std::clamp(t.start + int(k), 0, src.width() - 1);
                const qreal w = t.weights[k];
                const QRgb color = row_in[sx];
                r += w * qRed(color);
                g += w * qGreen(color);
                b += w * qBlue(color);
                a += w * qAlpha(color);
            }
            row_out[x] = qRgba(qRound(r), qRound(g), qRound(b), qRound(a));
        }
    }

    QImage out(target.width(), target.height(), QImage::Format_ARGB32);
    const std::vector<FilterTaps> v_taps = BuildTaps(src.height(), target.height());
    for (int y = 0; y < target.height(); ++y) {
        const FilterTaps& t = v_taps[y];
        auto* row_out = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < target.width(); ++x) {
            qreal r = 0.0, g = 0.0, b = 0.0, a = 0.0;
            for (size_t k = 0; k < t.weights.size(); ++k) {
                const int sy = std::clamp(t.start + int(k), 0, temp.height() - 1);
                const qreal w = t.weights[k];
                const QRgb color = reinterpret_cast<const QRgb*>(temp.scanLine(sy))[x];
                r += w * qRed(color);
                g += w * qGreen(color);
                b += w * qBlue(color);
                a += w * qAlpha(color);
            }
            row_out[x] = qRgba(qRound(r), qRound(g), qRound(b), qRound(a));
        }
    }
    return out;
}

/// The card's icon at `bucket` logical pixels square, resampled once and cached: the model's
/// decoration is pre-scaled to the icon-size setting, so enlarging it per frame with the
/// painter's filter both looked soft and cost a smooth stretch per card per frame.
QPixmap ScaledCardIcon(const QModelIndex& index, int bucket, qreal dpr, int cap_px) {
    const int px = std::min(cap_px, std::max(1, qRound(bucket * dpr)));
    // The icon the user chose through the artwork feature overrides the embedded one, as in
    // every other view; otherwise the embedded original beats the pre-scaled decoration.
    QPixmap source;
    if (QtCommon::GameArt::HasIcon(index.data(GameListItemPath::ProgramIdRole).toULongLong())) {
        source = index.data(Qt::DecorationRole).value<QPixmap>();
    }
    if (source.isNull()) {
        source = index.data(GameListItemPath::OriginalIconRole).value<QPixmap>();
    }
    if (source.isNull()) {
        source = index.data(Qt::DecorationRole).value<QPixmap>();
    }
    if (source.isNull()) {
        return {};
    }
    // Keyed by the pixmap itself: two rows of the same title (two dumps of one game) must not
    // share an entry, and neither must a placeholder stand in for a real icon.
    const QString key = QStringLiteral("eglantine-carousel-%1-%2-%3")
                            .arg(source.cacheKey())
                            .arg(px)
                            .arg(dpr);
    QPixmap cached;
    if (QPixmapCache::find(key, &cached)) {
        return cached;
    }
    const QSize physical(px, px);
    const QSize fitted = source.size().scaled(physical, Qt::KeepAspectRatio);
    QImage square(physical, QImage::Format_ARGB32);
    square.fill(Qt::transparent);
    QPainter sq(&square);
    sq.drawImage((physical.width() - fitted.width()) / 2, (physical.height() - fitted.height()) / 2,
                 MitchellResample(source.toImage(), fitted));
    QPixmap out = QPixmap::fromImage(square);
    out.setDevicePixelRatio(dpr);
    QPixmapCache::insert(key, out);
    return out;
}

struct CarouselMetrics {
    QPointF center;
    qreal card_w = 0.0;
    qreal card_h = 0.0;
    qreal peek = 0.0;
    bool names = false;
};

CarouselMetrics MetricsFor(const QSize& area) {
    CarouselMetrics m;
    m.names = UISettings::values.show_game_name.GetValue();
    const qreal title = m.names ? TitleReserve : 0.0;
    // The stored index picks a share of the available height; anything out of range reads as
    // full, so a stale value can never shrink the card away.
    const auto fractions = GameCarousel::CardSizeFractions;
    size_t size_index = UISettings::values.carousel_card_size.GetValue();
    if (size_index >= fractions.size()) {
        size_index = 0;
    }
    const qreal available = std::max<qreal>(0.0, area.height() - 2 * SideGap - title);
    m.card_h = std::max<qreal>(MinCenterSize, available * fractions[size_index]);
    // The cartridge frame is portrait, and its slot is too: the stack, the peeks and the title
    // all space a rectangle instead of a square with a rectangle painted inside it.
    const bool cartridge = UISettings::values.game_icon_borders.GetValue() &&
                           UISettings::values.game_cartridge_kind.GetValue() == 1;
    m.card_w = cartridge ? m.card_h * GameCard::CartridgeAspect : m.card_h;
    // The card floats centered in the available height at every size.
    m.center = QPointF(area.width() / 2.0, SideGap + available / 2.0);
    m.peek = std::clamp(m.card_w * PeekFraction, qreal(40), qreal(96));
    return m;
}

/// The card's untilted rectangle at `depth`: 0 is the centered card, and the card `k` steps away
/// tucks under the one in front of it, peeking `k * peek` past its edge. The tilt foreshortens a
/// stacked card, so its slot is laid out on the foreshortened width and the peek stays exact.
/// Fractional depths interpolate, so the strip slides as one piece while the position animates.
QRectF CardRect(qreal depth, const CarouselMetrics& m) {
    const qreal d = std::abs(depth);
    const qreal fade = std::min(d, 1.0);
    const qreal scale = std::max(MinScale, std::pow(ScaleStep, d));
    const qreal w = m.card_w * scale * (1.0 - TiltSqueeze * fade);
    const qreal h = m.card_h * scale;
    const qreal y = m.center.y() - h / 2.0;
    if (depth > 0.0) {
        const qreal centered = m.center.x() - m.card_w / 2.0;
        const qreal stacked = m.center.x() + m.card_w / 2.0 + m.peek - w;
        const qreal left = depth < 1.0 ? centered + (stacked - centered) * depth
                                       : stacked + (depth - 1.0) * m.peek;
        return {left, y, w, h};
    }
    if (depth < 0.0) {
        const qreal centered = m.center.x() - m.card_w / 2.0;
        const qreal stacked = m.center.x() - m.card_w / 2.0 - m.peek;
        const qreal left = depth > -1.0 ? centered + (stacked - centered) * (-depth)
                                       : stacked + (depth + 1.0) * m.peek;
        return {left, y, w, h};
    }
    return {m.center.x() - w / 2.0, y, w, h};
}

void DrawCardShadow(QPainter& p, const QRectF& rect, int alpha) {
    if (alpha <= 0) {
        return;
    }
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, alpha));
    const qreal radius = rect.width() * 0.08;
    for (int spread = 1; spread <= 5; ++spread) {
        p.drawRoundedRect(rect.adjusted(-spread, -spread + 2, spread, spread + 2),
                          radius + spread, radius + spread);
    }
}

/// The title of the card taking the center slot, fading in as the card settles.
void DrawTitle(QPainter& p, const QModelIndex& index, const CarouselMetrics& m,
               const QPalette& pal) {
    const QString title =
        index.data(Qt::DisplayRole).toString().split(QLatin1Char('\n')).first();
    QFont font = p.font();
    font.setBold(true);
    font.setPixelSize(qRound(std::clamp(m.card_w * 0.045, qreal(13), qreal(26))));
    const int text_width = qRound(m.card_w * 1.4);
    const QString line = QFontMetrics(font).elidedText(title, Qt::ElideRight, text_width);
    p.setFont(font);
    p.setPen(pal.color(QPalette::Text));
    const QRectF slot = CardRect(0.0, m);
    p.drawText(QRectF(slot.center().x() - text_width / 2.0, slot.bottom() + 6, text_width,
                      TitleReserve - 10),
               Qt::AlignHCenter | Qt::AlignTop, line);
}

/// A stacked cartridge card: the label art and the frame under one tilt, drawn full body. The
/// frame is cached per bucket, so the extra blit area over a strip costs nothing.
void DrawStackedCartridge(QPainter& p, const QPixmap& icon, const QRectF& slot, qreal depth,
                          qreal dpr, const QPalette& pal) {
    const qreal d = std::abs(depth);
    const QRectF& art = slot;
    const qreal fade = std::min(d, 1.0);
    const qreal far_h = art.height() * (1.0 + (TiltHeight - 1.0) * fade);
    const qreal yc = art.center().y();
    const qreal aw = art.width();
    const qreal ah = art.height();
    QPolygonF one{QPointF(0, 0), QPointF(aw, 0), QPointF(aw, ah), QPointF(0, ah)};
    QPolygonF target;
    // The whole cartridge maps across its slot: the edge facing the center keeps its height,
    // the far edge is shorter and reads as receding.
    if (depth > 0.0) {
        target << QPointF(art.left(), art.top()) << QPointF(art.right(), yc - far_h / 2)
               << QPointF(art.right(), yc + far_h / 2) << QPointF(art.left(), art.bottom());
    } else {
        target << QPointF(art.left(), yc - far_h / 2) << QPointF(art.right(), art.top())
               << QPointF(art.right(), art.bottom()) << QPointF(art.left(), yc + far_h / 2);
    }
    QTransform tilt;
    if (!QTransform::quadToQuad(one, target, tilt)) {
        return;
    }
    const QPixmap frame = GameCard::CartridgePixmap(qRound(ah), dpr);
    if (frame.isNull()) {
        return;
    }
    p.save();
    p.setTransform(tilt);
    // The frame's slot carries a transparent margin and a faint baked shadow around the
    // solid body. Clip and fog the body itself: washing the whole slot paints a translucent
    // border around every stacked card, and a clip wider than the frame's own ~8%-width
    // corners trims them.
    const QRectF body = GameCard::CartridgeBodyRect(QRectF(0, 0, aw, ah));
    const qreal body_radius =
        GameCard::CartridgeBodyRadius * aw / GameCard::CartridgeBase.width();
    QPainterPath clip;
    clip.addRoundedRect(body, body_radius, body_radius);
    p.setClipPath(clip);
    p.drawPixmap(QRectF(0, 0, aw, ah), frame,
                 QRectF(0, 0, frame.width(), frame.height()));
    const QRectF label = GameCard::CartridgeLabelRect(QRectF(0, 0, aw, ah));
    p.save();
    QPainterPath label_clip;
    label_clip.addRoundedRect(label, label.width() * 0.04, label.width() * 0.04);
    p.setClipPath(label_clip);
    GameCard::DrawCoverCropped(p, icon, label);
    p.restore();
    const int fog = std::min(FogMax, qRound(d * FogPerStep));
    p.fillRect(body, WithAlpha(pal.color(QPalette::Window), fog));
    p.restore();
}

/// One card at `depth`. Every visual parameter is continuous in the depth, so a card sliding out
/// of the center shrinks, tilts and fogs in instead of snapping between two shapes.
void DrawCard(QPainter& p, const QModelIndex& index, const QRectF& rect, qreal depth,
              const QPalette& pal, bool focused, bool borders, qreal dpr, qreal peek,
              int center_bucket, int stack_bucket, qreal alpha) {
    const qreal d = std::abs(depth);
    const bool centered = d < 0.01;
    const QPixmap icon =
        ScaledCardIcon(index, centered ? center_bucket : stack_bucket, dpr,
                       centered ? MaxCenterSourcePx : MaxStackSourcePx);
    if (icon.isNull()) {
        return;
    }
    const bool cartridge = borders && UISettings::values.game_cartridge_kind.GetValue() == 1;
    // The cartridge's own solid frame is its outline: a translucent shadow halo or a
    // selection ring drawn around it reads as an ugly transparent border, so neither is
    // drawn in cartridge mode.
    if (d < 1.0 && !cartridge) {
        // Only the card taking the center slot casts the full shadow.
        DrawCardShadow(p, rect, int((1.0 - d) * 13));
    }

    p.save();
    if (alpha < 1.0) {
        p.setOpacity(alpha);
    }
    const int side = icon.width();

    if (centered) {
        if (cartridge) {
            const QRectF& cart = rect;
            const QPixmap frame = GameCard::CartridgePixmap(qRound(cart.height()), dpr);
            if (!frame.isNull()) {
                // The frame's slot carries a transparent margin and a faint baked shadow
                // around the solid body; clip to the body so the silhouette matches the
                // stacked cards and nothing translucent hugs the card. The frame goes down
                // first and the game art sits on its label, cover-cropped.
                const QRectF body = GameCard::CartridgeBodyRect(cart);
                const qreal body_radius =
                    GameCard::CartridgeBodyRadius * cart.width() /
                    GameCard::CartridgeBase.width();
                QPainterPath body_clip;
                body_clip.addRoundedRect(body, body_radius, body_radius);
                p.setClipPath(body_clip);
                // The frame first, the game art on its label: the label design shows through
                // the art's transparent regions.
                p.drawPixmap(cart, frame, QRectF(0, 0, frame.width(), frame.height()));
                const QRectF label = GameCard::CartridgeLabelRect(cart);
                p.save();
                QPainterPath label_clip;
                label_clip.addRoundedRect(label, label.width() * 0.04, label.width() * 0.04);
                p.setClipPath(label_clip);
                GameCard::DrawCoverCropped(p, icon, label);
                p.restore();
            }
        } else {
            QPainterPath clip;
            clip.addRoundedRect(rect, rect.width() * 0.08, rect.width() * 0.08);
            p.setClipPath(clip);
            p.drawPixmap(rect.toRect(), icon);
            if (borders) {
                p.drawPixmap(rect.toRect(),
                             GameCard::PlatformBorderPixmap(
                                 index.data(GameListItemPath::Switch2Role).toBool(),
                                 qRound(rect.width()), dpr));
            }
        }
    } else if (cartridge) {
        DrawStackedCartridge(p, icon, rect, depth, dpr, pal);
    } else {
        const qreal fade = std::min(d, 1.0);
        const qreal far_h = rect.height() * (1.0 + (TiltHeight - 1.0) * fade);
        const qreal yc = rect.center().y();
        QPolygonF one{QPointF(0, 0), QPointF(side, 0), QPointF(side, side), QPointF(0, side)};
        QPolygonF target;
        // The whole icon maps across the card's slot: the edge facing the center keeps its
        // height, the far edge is shorter and reads as receding.
        if (depth > 0.0) {
            target << QPointF(rect.left(), rect.top()) << QPointF(rect.right(), yc - far_h / 2)
                   << QPointF(rect.right(), yc + far_h / 2) << QPointF(rect.left(), rect.bottom());
        } else {
            target << QPointF(rect.left(), yc - far_h / 2) << QPointF(rect.right(), rect.top())
                   << QPointF(rect.right(), rect.bottom())
                   << QPointF(rect.left(), yc + far_h / 2);
        }
        QTransform tilt;
        if (QTransform::quadToQuad(one, target, tilt)) {
            p.setTransform(tilt);
            QPainterPath clip;
            clip.addRoundedRect(QRectF(0, 0, side, side), side * 0.08, side * 0.08);
            p.setClipPath(clip);
            // Only the far-end sliver of a stacked card can be seen; the rest sits under the
            // card in front, so drawing that strip alone keeps the resampling cheap. A card
            // crossing the center slot has nothing in front of it and needs its whole body;
            // a settled card needs just its peek, with the wider margin covering the gap a
            // slide opens between stack neighbours.
            qreal margin;
            if (d < 1.0) {
                margin = rect.width();
            } else {
                const qreal frac = std::abs(d - std::round(d));
                margin = frac < 0.02 ? peek + 4.0 : 2.5 * peek + 8.0;
            }
            const qreal strip = std::min(qreal(side), margin / rect.width() * side);
            const QRectF src = depth > 0.0 ? QRectF(side - strip, 0.0, strip, side)
                                           : QRectF(0.0, 0.0, strip, side);
            p.drawPixmap(src, icon, src);
            if (borders) {
                p.drawPixmap(src,
                             GameCard::PlatformBorderPixmap(
                                 index.data(GameListItemPath::Switch2Role).toBool(), side, dpr),
                             src);
            }
            const int fog = std::min(FogMax, qRound(d * FogPerStep));
            p.fillRect(src, WithAlpha(pal.color(QPalette::Window), fog));
        }
    }
    p.restore();

    if (centered && focused && !cartridge) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(pal.color(QPalette::Highlight), FocusRingWidth));
        p.drawRoundedRect(rect.adjusted(-1, -1, 1, 1), rect.width() * 0.08 + 1,
                          rect.width() * 0.08 + 1);
    }
}

} // namespace

GameCarousel::GameCarousel(QWidget* parent) : QListView{parent} {
    setSelectionMode(QAbstractItemView::SingleSelection);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setContextMenuPolicy(Qt::CustomContextMenu);
    setFocusPolicy(Qt::StrongFocus);

    // Full-height cards mean large icon entries; Qt's small default pixmap cache would evict
    // them mid-animation and re-filter every frame. Raise it once, never lower it.
    if (QPixmapCache::cacheLimit() < 65536) {
        QPixmapCache::setCacheLimit(65536);
    }

    m_position_anim = new QVariantAnimation(this);
    m_position_anim->setDuration(AnimationMs);
    m_position_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_position_anim, &QVariantAnimation::valueChanged, this,
            [this](const QVariant& value) {
                m_position = value.toReal();
                viewport()->update();
            });

}

void GameCarousel::SetModel(GameListModel* model) {
    QListView::setModel(model);
    // A repopulate replaces the rows, so the stored filter has to run over the new ones.
    connect(model, &QAbstractItemModel::rowsInserted, this, [this] { ScheduleRelayout(); });
    connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { ScheduleRelayout(); });
    connect(model, &QAbstractItemModel::modelReset, this, [this] { ScheduleRelayout(); });
    ApplyFilter({}, model);
}

void GameCarousel::ApplyFilter(const QString& edit_filter_text, GameListModel* model) {
    m_filter_text = edit_filter_text;
    m_visible.clear();
    for (int row = 0; row < model->rowCount(); ++row) {
        QStandardItem* item = model->item(row, 0);
        if (!item || Yuzu::FilterMatches(edit_filter_text, item)) {
            m_visible.push_back(row);
        }
    }
    if (m_position >= m_visible.size()) {
        m_position = std::max<qreal>(0.0, qreal(m_visible.size()) - 1.0);
    }
    viewport()->update();
    emit CenteredGameChanged(CenteredProgramId());
}

u64 GameCarousel::CenteredProgramId() const {
    if (m_visible.empty() || !model()) {
        return 0;
    }
    const int index = m_pending_center >= 0 && m_pending_center < int(m_visible.size())
                          ? m_pending_center
                          : std::clamp(int(std::lround(m_position)), 0,
                                       int(m_visible.size()) - 1);
    return model()->index(m_visible[index], 0)
        .data(GameListItemPath::ProgramIdRole)
        .toULongLong();
}

void GameCarousel::UpdateIconSize() {
    // The card sizes follow the viewport, so there is nothing to recompute.
    viewport()->update();
}

int GameCarousel::PositionOf(int row) const {
    const auto it = std::find(m_visible.begin(), m_visible.end(), row);
    return it == m_visible.end() ? -1 : int(it - m_visible.begin());
}

void GameCarousel::MoveTo(int position) {
    if (m_visible.empty()) {
        m_position = 0.0;
        viewport()->update();
        return;
    }
    position = std::clamp(position, 0, int(m_visible.size()) - 1);
    if (qFuzzyCompare(m_position, qreal(position))) {
        m_position = position;
        viewport()->update();
        return;
    }
    m_position_anim->stop();
    m_position_anim->setStartValue(m_position);
    m_position_anim->setEndValue(qreal(position));
    // The backdrop follows the slide's destination, not the games it passes.
    m_pending_center = position;
    emit CenteredGameChanged(CenteredProgramId());
    // The card settling into the center slot swaps to the center-sized icon; warm it now so
    // the last frame of the slide does not hitch on its first resample.
    if (model() && position >= 0 && position < int(m_visible.size())) {
        const CarouselMetrics metrics = MetricsFor(viewport()->size());
        ScaledCardIcon(model()->index(m_visible[position], 0), SizeBucketOf(metrics.card_w),
                       devicePixelRatioF(), MaxCenterSourcePx);
    }
    m_position_anim->start();
}

void GameCarousel::ScheduleRelayout() {
    if (m_relayout_queued) {
        return;
    }
    m_relayout_queued = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_relayout_queued = false;
            m_pending_center = -1;
            if (auto* model = qobject_cast<GameListModel*>(this->model())) {
                ApplyFilter(m_filter_text, model);
            }
        },
        Qt::QueuedConnection);
}

void GameCarousel::paintEvent(QPaintEvent*) {
    QPainter p(viewport());
    if (m_visible.empty() || !model()) {
        return;
    }
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const CarouselMetrics m = MetricsFor(viewport()->size());
    const bool borders = UISettings::values.game_icon_borders.GetValue();
    const bool focused = hasFocus();
    const qreal dpr = p.device() ? p.device()->devicePixelRatio() : 1.0;
    const int center_bucket = SizeBucketOf(m.card_w);
    const int stack_bucket = SizeBucketOf(m.card_w * ScaleStep);

    // Draw the farthest cards first so the centered card sits on top of the stack.
    struct Card {
        int position;
        qreal depth;
    };
    std::vector<Card> cards;
    const int max_depth =
        std::clamp(int(UISettings::values.carousel_side_count.GetValue()), 1, MaxSideCount);
    const int base = int(std::floor(m_position));
    for (int position = std::max(0, base - max_depth - 1);
         position <= std::min<int>(int(m_visible.size()) - 1, base + max_depth + 1); ++position) {
        cards.push_back({position, position - m_position});
    }
    std::sort(cards.begin(), cards.end(), [](const Card& a, const Card& b) {
        if (!qFuzzyCompare(std::abs(a.depth), std::abs(b.depth))) {
            return std::abs(a.depth) > std::abs(b.depth);
        }
        // While sliding, the outgoing card passes under the incoming one.
        return a.depth > b.depth;
    });
    for (const Card& card : cards) {
        const QRectF rect = CardRect(card.depth, m);
        // A sliver past the viewport edge cannot be seen; skipping it also skips its icon fetch.
        if (rect.right() < 0.0 || rect.left() >= viewport()->width()) {
            continue;
        }
        const qreal alpha = StackAlpha(card.depth, max_depth);
        if (alpha <= 0.0) {
            continue;
        }
        const QModelIndex index = model()->index(m_visible[card.position], 0);
        DrawCard(p, index, rect, card.depth, palette(), focused, borders, dpr, m.peek,
                 center_bucket, stack_bucket, alpha);
    }

    if (m.names) {
        // The title belongs to the card settling into the center slot and fades with it.
        const auto nearest = std::min_element(
            cards.begin(), cards.end(), [](const Card& a, const Card& b) {
                return std::abs(a.depth) < std::abs(b.depth);
            });
        if (nearest != cards.end() && std::abs(nearest->depth) < 0.5) {
            p.setOpacity(std::max<qreal>(0.0, 1.0 - 2.0 * std::abs(nearest->depth)));
            DrawTitle(p, model()->index(m_visible[nearest->position], 0), m, palette());
            p.setOpacity(1.0);
        }
    }
}

QModelIndex GameCarousel::indexAt(const QPoint& point) const {
    if (m_visible.empty() || !model()) {
        return {};
    }
    const CarouselMetrics m = MetricsFor(viewport()->size());
    // The cartridge slot carries a transparent margin around the solid body, and the next
    // card's art shows through it: hit-test the body, or a click on what looks like the
    // neighbour's cartridge lands on the centered card's empty margin and selects nothing.
    const bool cartridge = UISettings::values.game_icon_borders.GetValue() &&
                           UISettings::values.game_cartridge_kind.GetValue() == 1;
    const int max_depth =
        std::clamp(int(UISettings::values.carousel_side_count.GetValue()), 1, MaxSideCount) + 1;
    int best = -1;
    qreal best_depth = 0.0;
    for (int position = 0; position < int(m_visible.size()); ++position) {
        const qreal depth = position - m_position;
        if (std::abs(depth) > max_depth) {
            continue;
        }
        const QRectF hit = cartridge ? GameCard::CartridgeBodyRect(CardRect(depth, m))
                                     : CardRect(depth, m);
        if (hit.contains(QPointF(point)) &&
            (best < 0 || std::abs(depth) < std::abs(best_depth))) {
            best = position;
            best_depth = depth;
        }
    }
    return best < 0 ? QModelIndex{} : model()->index(m_visible[best], 0);
}

QRect GameCarousel::visualRect(const QModelIndex& index) const {
    if (!index.isValid()) {
        return {};
    }
    const int position = PositionOf(index.row());
    if (position < 0) {
        return {};
    }
    const CarouselMetrics m = MetricsFor(viewport()->size());
    return CardRect(position - m_position, m).toAlignedRect();
}

bool GameCarousel::isIndexHidden(const QModelIndex& index) const {
    return PositionOf(index.row()) < 0;
}

void GameCarousel::scrollTo(const QModelIndex& index, ScrollHint) {
    if (!index.isValid()) {
        return;
    }
    const int position = PositionOf(index.row());
    if (position >= 0) {
        MoveTo(position);
    }
}

QModelIndex GameCarousel::moveCursor(CursorAction cursor_action, Qt::KeyboardModifiers) {
    if (m_visible.empty() || !model()) {
        return {};
    }
    const QModelIndex current = currentIndex();
    int position = current.isValid() ? std::max(0, PositionOf(current.row())) : 0;
    switch (cursor_action) {
    case MoveLeft:
    case MoveUp:
        position = std::max(0, position - 1);
        break;
    case MoveRight:
    case MoveDown:
        position = std::min(int(m_visible.size()) - 1, position + 1);
        break;
    case MoveHome:
        position = 0;
        break;
    case MoveEnd:
        position = int(m_visible.size()) - 1;
        break;
    case MovePageUp:
        position = std::max(0, position - 4);
        break;
    case MovePageDown:
        position = std::min(int(m_visible.size()) - 1, position + 4);
        break;
    default:
        break;
    }
    return model()->index(m_visible[position], 0);
}

void GameCarousel::setSelection(const QRect& rect, QItemSelectionModel::SelectionFlags command) {
    // A carousel cannot be rubber-banded: the card under the press is the card selected.
    const QModelIndex index = indexAt(rect.center());
    if (index.isValid()) {
        selectionModel()->select(index, command);
    }
}

QRegion GameCarousel::visualRegionForSelection(const QItemSelection& selection) const {
    QRegion region;
    for (const QItemSelectionRange& range : selection) {
        for (int row = range.top(); row <= range.bottom(); ++row) {
            const QRect rect = visualRect(model()->index(row, 0));
            if (!rect.isNull()) {
                region += rect;
            }
        }
    }
    return region;
}

void GameCarousel::wheelEvent(QWheelEvent* event) {
    if (m_visible.empty() || !model()) {
        QListView::wheelEvent(event);
        return;
    }
    // The strip is one row deep: the wheel steps between games instead of scrolling.
    const int delta = event->angleDelta().x() + event->angleDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }
    const QModelIndex current = currentIndex();
    int position = current.isValid() ? std::max(0, PositionOf(current.row())) : 0;
    position = std::clamp(position + (delta > 0 ? -1 : 1), 0, int(m_visible.size()) - 1);
    setCurrentIndex(model()->index(m_visible[position], 0));
    event->accept();
}

void GameCarousel::currentChanged(const QModelIndex& current, const QModelIndex& previous) {
    QListView::currentChanged(current, previous);
    if (!current.isValid()) {
        return;
    }
    const int position = PositionOf(current.row());
    if (position >= 0) {
        MoveTo(position);
    }
}
