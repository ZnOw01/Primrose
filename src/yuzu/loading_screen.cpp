// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <QBuffer>
#include <QByteArray>
#include <QEvent>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIODevice>
#include <QImage>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include "core/frontend/framebuffer_layout.h"
#include "core/loader/loader.h"
#include "qt_common/util/game_art.h"
#include "video_core/rasterizer_interface.h"
#include "yuzu/game/home_background.h"
#include "yuzu/loading_screen.h"

// Mingw seems to not have QMovie at all. If QMovie is missing then use a single frame instead of an
// showing the full animation
#if !YUZU_QT_MOVIE_MISSING
#include <QMovie>
#endif

namespace {

constexpr int IconSize = 128;
constexpr int BarWidth = 420;
constexpr int BarHeight = 6;
// Builds that finish faster than this keep the busy bar, so a warm cache doesn't flash a counter.
constexpr std::chrono::milliseconds BuildDetailDelay{300};
constexpr std::chrono::milliseconds TextUpdateInterval{100};
constexpr std::chrono::milliseconds RateSampleInterval{500};
constexpr std::chrono::seconds EtaDelay{2};
constexpr double RateSmoothing = 0.3;

QColor Mix(const QColor& from, const QColor& to, qreal amount) {
    return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * amount,
                            from.greenF() + (to.greenF() - from.greenF()) * amount,
                            from.blueF() + (to.blueF() - from.blueF()) * amount);
}

QPixmap RoundedIcon(const QImage& image, int size, qreal dpr) {
    QPixmap out(QSize(size, size) * dpr);
    out.setDevicePixelRatio(dpr);
    out.fill(Qt::transparent);

    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    const qreal radius = size * 0.16;
    clip.addRoundedRect(QRectF(0, 0, size, size), radius, radius);
    painter.setClipPath(clip);
    painter.drawImage(QRectF(0, 0, size, size), image);
    return out;
}

} // namespace

/// Thin rounded bar in the palette's highlight colour, with a sliding segment when busy.
class LoadingProgressBar : public QWidget {
public:
    explicit LoadingProgressBar(QWidget* parent) : QWidget(parent) {
        setFixedSize(BarWidth, BarHeight);
        busy_animation.setStartValue(0.0);
        busy_animation.setEndValue(1.0);
        busy_animation.setDuration(1400);
        busy_animation.setLoopCount(-1);
        busy_animation.setEasingCurve(QEasingCurve::InOutSine);
        QObject::connect(&busy_animation, &QVariantAnimation::valueChanged, this,
                         [this] { update(); });
    }

    void SetBusy() {
        if (busy_animation.state() != QAbstractAnimation::Running) {
            busy_animation.start();
        }
    }

    void Stop() {
        busy_animation.stop();
        fraction = 0.0;
    }

    void SetProgress(std::size_t value, std::size_t total) {
        busy_animation.stop();
        const qreal next = total == 0 ? 0.0 : std::clamp(qreal(value) / qreal(total), 0.0, 1.0);
        if (next != fraction) {
            fraction = next;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);

        const QPalette& pal = palette();
        const QRectF track = rect();
        const qreal radius = track.height() / 2.0;
        painter.setBrush(Mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.14));
        painter.drawRoundedRect(track, radius, radius);

        QRectF fill = track;
        if (busy_animation.state() == QAbstractAnimation::Running) {
            const qreal segment = track.width() * 0.3;
            const qreal phase = busy_animation.currentValue().toReal();
            fill.setLeft((track.width() - segment) * phase);
            fill.setWidth(segment);
        } else {
            fill.setWidth(std::max(track.height(), track.width() * fraction));
            if (fraction == 0.0) {
                return;
            }
        }
        painter.setBrush(pal.color(QPalette::Highlight));
        painter.drawRoundedRect(fill, radius, radius);
    }

private:
    QVariantAnimation busy_animation;
    qreal fraction = 0.0;
};

/// Holds the faded content and paints the game's custom banner, if it has one, behind it.
class LoadingBackdrop : public QWidget {
public:
    using QWidget::QWidget;

    void SetImage(const QImage& image_) {
        image = image_;
        cached = {};
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (image.isNull()) {
            return;
        }
        const qreal dpr = devicePixelRatioF();
        if (cached.isNull() || cached.size() != size() * dpr) {
            cached = QPixmap(size() * dpr);
            cached.setDevicePixelRatio(dpr);
            QPainter p(&cached);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            HomeBackground::PaintCover(p, rect(), image);
            // Keep the title and progress text readable on any artwork.
            const QColor window = palette().color(QPalette::Window);
            QColor scrim = window;
            scrim.setAlpha(150);
            p.fillRect(rect(), scrim);
            QLinearGradient fade(0, 0, 0, height());
            QColor clear = window;
            clear.setAlpha(0);
            fade.setColorAt(0.0, clear);
            fade.setColorAt(1.0, window);
            p.fillRect(rect(), fade);
        }
        QPainter p(this);
        p.drawPixmap(0, 0, cached);
    }

    void changeEvent(QEvent* event) override {
        if (event->type() == QEvent::PaletteChange) {
            cached = {};
        }
        QWidget::changeEvent(event);
    }

private:
    QImage image;
    QPixmap cached;
};

LoadingScreen::LoadingScreen(QWidget* parent)
    : QWidget(parent), previous_stage(VideoCore::LoadCallbackStage::Complete) {
    setMinimumSize(Layout::MinimumSize::Width, Layout::MinimumSize::Height);

    // The opacity effect sits on an inner widget so the fade reveals this widget's own
    // background instead of the parent's.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    fade_parent = new LoadingBackdrop(this);
    outer->addWidget(fade_parent);

    logo = new QLabel(fade_parent);
    banner = new QLabel(fade_parent);
    icon = new QLabel(fade_parent);
    title = new QLabel(fade_parent);
    stage_label = new QLabel(fade_parent);
    progress_bar = new LoadingProgressBar(fade_parent);
    detail = new QLabel(fade_parent);

    title->setAlignment(Qt::AlignCenter);
    title->setWordWrap(true);
    stage_label->setAlignment(Qt::AlignCenter);
    detail->setAlignment(Qt::AlignCenter);
    detail->setMinimumWidth(BarWidth);

    QFont title_font = title->font();
    title_font.setPointSizeF(title_font.pointSizeF() * 1.8);
    title_font.setWeight(QFont::Bold);
    title->setFont(title_font);
    QFont stage_font = stage_label->font();
    stage_font.setPointSizeF(stage_font.pointSizeF() * 1.15);
    stage_label->setFont(stage_font);
    stage_label->setMinimumHeight(stage_label->fontMetrics().height());
    detail->setMinimumHeight(detail->fontMetrics().height());

    auto* layout = new QVBoxLayout(fade_parent);
    layout->setContentsMargins(30, 30, 30, 30);
    layout->setSpacing(0);
    layout->addWidget(logo, 0, Qt::AlignLeft | Qt::AlignTop);
    layout->addStretch(1);
    layout->addWidget(icon, 0, Qt::AlignHCenter);
    layout->addSpacing(20);
    layout->addWidget(title);
    layout->addSpacing(28);
    layout->addWidget(stage_label, 0, Qt::AlignHCenter);
    layout->addSpacing(12);
    layout->addWidget(progress_bar, 0, Qt::AlignHCenter);
    layout->addSpacing(10);
    layout->addWidget(detail, 0, Qt::AlignHCenter);
    layout->addStretch(1);
    layout->addWidget(banner, 0, Qt::AlignRight | Qt::AlignBottom);

    opacity_effect = new QGraphicsOpacityEffect(this);
    opacity_effect->setOpacity(1);
    fade_parent->setGraphicsEffect(opacity_effect);
    fadeout_animation = std::make_unique<QPropertyAnimation>(opacity_effect, "opacity");
    fadeout_animation->setDuration(400);
    fadeout_animation->setStartValue(1);
    fadeout_animation->setEndValue(0);
    fadeout_animation->setEasingCurve(QEasingCurve::OutCubic);

    // After the fade completes, hide the widget and reset the opacity
    connect(fadeout_animation.get(), &QPropertyAnimation::finished, [this] {
        hide();
        opacity_effect->setOpacity(1);
        emit Hidden();
    });
    connect(this, &LoadingScreen::LoadProgress, this, &LoadingScreen::OnLoadProgress,
            Qt::QueuedConnection);
    qRegisterMetaType<VideoCore::LoadCallbackStage>();

    ApplyPalette();
}

LoadingScreen::~LoadingScreen() = default;

void LoadingScreen::Prepare(Loader::AppLoader& loader) {
    std::vector<u8> buffer;
    bool has_icon = false;
    if (loader.ReadIcon(buffer) == Loader::ResultStatus::Success) {
        QImage image;
        image.loadFromData(buffer.data(), static_cast<int>(buffer.size()));
        if (!image.isNull()) {
            icon->setPixmap(RoundedIcon(image, IconSize, devicePixelRatioF()));
            has_icon = true;
        }
        buffer.clear();
    }
    icon->setVisible(has_icon);

    u64 program_id = 0;
    QImage hero;
    if (loader.ReadProgramId(program_id) == Loader::ResultStatus::Success && program_id != 0) {
        const QString hero_path = QtCommon::GameArt::HeroPath(program_id);
        if (QFileInfo::exists(hero_path)) {
            hero.load(hero_path);
        }
    }
    fade_parent->SetImage(hero);

    std::string name;
    if (loader.ReadTitle(name) == Loader::ResultStatus::Success) {
        title->setText(QString::fromStdString(name));
    }
    title->setVisible(!title->text().isEmpty());

    if (loader.ReadBanner(buffer) == Loader::ResultStatus::Success) {
#ifdef YUZU_QT_MOVIE_MISSING
        QPixmap map;
        map.loadFromData(buffer.data(), buffer.size());
        banner->setPixmap(map);
#else
        backing_mem = std::make_unique<QByteArray>(reinterpret_cast<char*>(buffer.data()),
                                                   static_cast<int>(buffer.size()));
        backing_buf = std::make_unique<QBuffer>(backing_mem.get());
        backing_buf->open(QIODevice::ReadOnly);
        animation = std::make_unique<QMovie>(backing_buf.get(), QByteArray());
        animation->start();
        banner->setMovie(animation.get());
#endif
        buffer.clear();
    }
    if (loader.ReadLogo(buffer) == Loader::ResultStatus::Success) {
        QPixmap map;
        map.loadFromData(buffer.data(), static_cast<uint>(buffer.size()));
        logo->setPixmap(map);
    }

    OnLoadProgress(VideoCore::LoadCallbackStage::Prepare, 0, 0);
}

void LoadingScreen::OnLoadComplete() {
    fadeout_animation->start(QPropertyAnimation::KeepWhenStopped);
}

void LoadingScreen::OnLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value,
                                   std::size_t total) {
    const auto now = Clock::now();
    const bool stage_changed = stage != previous_stage;
    previous_stage = stage;

    switch (stage) {
    case VideoCore::LoadCallbackStage::Prepare:
        ShowBusy(tr("Loading…"));
        return;
    case VideoCore::LoadCallbackStage::Complete:
        ShowBusy(tr("Starting game…"));
        return;
    case VideoCore::LoadCallbackStage::Build:
        break;
    }

    if (stage_changed) {
        build_start = now;
        last_sample_time = now;
        last_sample_value = value;
        last_text_update = {};
        pipelines_per_second = 0.0;
    }
    // Nothing cached, or a build quick enough that a counter would only flash.
    if (total == 0 || now - build_start < BuildDetailDelay) {
        ShowBusy(tr("Loading…"));
        return;
    }

    if (now - last_sample_time >= RateSampleInterval && value >= last_sample_value) {
        const double seconds = std::chrono::duration<double>(now - last_sample_time).count();
        const double rate = static_cast<double>(value - last_sample_value) / seconds;
        pipelines_per_second =
            pipelines_per_second == 0.0
                ? rate
                : RateSmoothing * rate + (1.0 - RateSmoothing) * pipelines_per_second;
        last_sample_time = now;
        last_sample_value = value;
    }

    progress_bar->SetProgress(value, total);
    // Pipelines report from worker threads, often thousands per second.
    if (value < total && now - last_text_update < TextUpdateInterval) {
        return;
    }
    last_text_update = now;
    UpdateBuildText(value, total, now);
}

void LoadingScreen::ShowBusy(const QString& text) {
    stage_label->setText(text);
    detail->clear();
    progress_bar->SetBusy();
}

void LoadingScreen::UpdateBuildText(std::size_t value, std::size_t total, Clock::time_point now) {
    stage_label->setText(tr("Building shaders"));

    const int percent = static_cast<int>(value * 100 / total);
    QString text = tr("%1 / %2  ·  %3%").arg(value).arg(total).arg(percent);
    if (value < total && now - build_start >= EtaDelay && pipelines_per_second > 0.0) {
        const auto remaining = std::chrono::seconds{static_cast<long long>(
            static_cast<double>(total - value) / pipelines_per_second + 0.5)};
        text += QStringLiteral("  ·  ") + FormatRemaining(remaining);
    }
    detail->setText(text);
}

QString LoadingScreen::FormatRemaining(std::chrono::seconds remaining) const {
    const auto secs = remaining.count();
    if (secs < 10) {
        return tr("a few seconds left");
    }
    if (secs < 60) {
        // Round to 5 s so the label doesn't tick every update.
        return tr("about %1 s left").arg((secs + 4) / 5 * 5);
    }
    const auto minutes = (secs + 30) / 60;
    if (minutes < 60) {
        return tr("about %n min left", nullptr, static_cast<int>(minutes));
    }
    return tr("about %1 h %2 min left").arg(minutes / 60).arg(minutes % 60);
}

void LoadingScreen::ApplyPalette() {
    if (!stage_label) {
        return;
    }
    const QPalette& pal = palette();
    QPalette dimmed = pal;
    QColor text = pal.color(QPalette::WindowText);
    text.setAlphaF(0.65f);
    dimmed.setColor(QPalette::WindowText, text);
    stage_label->setPalette(dimmed);
    detail->setPalette(dimmed);
}

void LoadingScreen::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        ApplyPalette();
    }
    QWidget::changeEvent(event);
}

void LoadingScreen::paintEvent(QPaintEvent* event) {
    QPainter p(this);
    p.fillRect(rect(), palette().color(QPalette::Window));
    QWidget::paintEvent(event);
}

void LoadingScreen::Clear() {
#ifndef YUZU_QT_MOVIE_MISSING
    animation.reset();
    backing_buf.reset();
    backing_mem.reset();
#endif
    progress_bar->Stop();
    fade_parent->SetImage({});
    icon->clear();
    title->clear();
    logo->clear();
    banner->clear();
}
