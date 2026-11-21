// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QFileInfo>
#include <QImageReader>
#include <QMovie>
#include <QPainter>
#include <QPixmapCache>
#include <QStringList>

#if YUZU_USE_QT_MULTIMEDIA
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#endif

#include <algorithm>
#include <cmath>

#include "qt_common/util/game_art.h"
#include "yuzu/game/home_background.h"

namespace {

// Larger stills are downscaled on load; the background never needs more than a 4K screen.
constexpr int MaxStillDimension = 3840;

const QStringList& VideoSuffixes() {
    static const QStringList suffixes{QStringLiteral("mp4"), QStringLiteral("m4v"),
                                      QStringLiteral("webm"), QStringLiteral("mkv"),
                                      QStringLiteral("mov"), QStringLiteral("avi")};
    return suffixes;
}

bool IsAnimatedImage(const QString& path) {
    QImageReader reader(path);
    return reader.supportsAnimation() && reader.imageCount() != 1;
}

QImage ReadStill(const QString& path, const QSize& bound) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > bound.width() || size.height() > bound.height())) {
        reader.setScaledSize(size.scaled(bound, Qt::KeepAspectRatio));
    }
    return reader.read();
}

/// The soft backdrop blur for banners: three box passes each way approximate a Gaussian closely
/// enough for a background wash, and run only once, when a banner enters the cache.
void BlurImage(QImage& image, int radius) {
    if (image.isNull() || radius < 1 || image.width() < 3 || image.height() < 3) {
        return;
    }
    image = image.convertToFormat(QImage::Format_ARGB32);
    QImage temp(image.size(), QImage::Format_ARGB32);
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < image.height(); ++y) {
            const auto* in = reinterpret_cast<const QRgb*>(image.scanLine(y));
            auto* out = reinterpret_cast<QRgb*>(temp.scanLine(y));
            int r = 0, g = 0, b = 0, a = 0;
            auto add = [&](int x) {
                const QRgb c = in[std::clamp(x, 0, image.width() - 1)];
                r += qRed(c);
                g += qGreen(c);
                b += qBlue(c);
                a += qAlpha(c);
            };
            for (int x = -radius; x <= radius; ++x) {
                add(x);
            }
            const int window = 2 * radius + 1;
            for (int x = 0; x < image.width(); ++x) {
                out[x] = qRgba(r / window, g / window, b / window, a / window);
                add(x + radius + 1);
                const QRgb gone = in[std::clamp(x - radius, 0, image.width() - 1)];
                r -= qRed(gone);
                g -= qGreen(gone);
                b -= qBlue(gone);
                a -= qAlpha(gone);
            }
        }
        image.swap(temp);
        for (int x = 0; x < image.width(); ++x) {
            int r = 0, g = 0, b = 0, a = 0;
            auto add = [&](int y) {
                const QRgb c = reinterpret_cast<const QRgb*>(
                    image.scanLine(std::clamp(y, 0, image.height() - 1)))[x];
                r += qRed(c);
                g += qGreen(c);
                b += qBlue(c);
                a += qAlpha(c);
            };
            for (int y = -radius; y <= radius; ++y) {
                add(y);
            }
            const int window = 2 * radius + 1;
            for (int y = 0; y < image.height(); ++y) {
                reinterpret_cast<QRgb*>(temp.scanLine(y))[x] =
                    qRgba(r / window, g / window, b / window, a / window);
                add(y + radius + 1);
                const QRgb gone = reinterpret_cast<const QRgb*>(
                    image.scanLine(std::clamp(y - radius, 0, image.height() - 1)))[x];
                r -= qRed(gone);
                g -= qGreen(gone);
                b -= qBlue(gone);
                a -= qAlpha(gone);
            }
        }
        image.swap(temp);
    }
}

} // namespace

HomeBackground::HomeBackground(QObject* parent) : QObject{parent} {}

HomeBackground::~HomeBackground() = default;

bool HomeBackground::IsVideo(const QString& path) {
    return VideoSuffixes().contains(QFileInfo(path).suffix().toLower());
}

bool HomeBackground::CanLoad(const QString& path) {
    if (!QFileInfo(path).isFile()) {
        return false;
    }
    if (IsVideo(path)) {
#if YUZU_USE_QT_MULTIMEDIA
        return true;
#else
        return false;
#endif
    }
    return QImageReader(path).canRead();
}

QString HomeBackground::FileDialogFilter() {
    QStringList patterns;
    for (const QByteArray& format : QImageReader::supportedImageFormats()) {
        patterns << QStringLiteral("*.%1").arg(QString::fromLatin1(format));
    }
#if YUZU_USE_QT_MULTIMEDIA
    for (const QString& suffix : VideoSuffixes()) {
        patterns << QStringLiteral("*.%1").arg(suffix);
    }
#endif
    return tr("Images and videos (%1)").arg(patterns.join(QLatin1Char{' '}));
}

QImage HomeBackground::LoadThumbnail(const QString& path, const QSize& size) {
    if (IsVideo(path)) {
        return {};
    }
    // Decode just large enough to cover twice the cell, so the crop stays sharp on HiDPI.
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize source = reader.size();
    const QSize cover = source.scaled(size * 2, Qt::KeepAspectRatioByExpanding);
    if (source.isValid() && cover.width() < source.width()) {
        reader.setScaledSize(cover);
    }
    return reader.read();
}

void HomeBackground::PaintCover(QPainter& painter, const QRect& target, const QImage& image) {
    if (image.isNull() || target.isEmpty()) {
        return;
    }
    const QSize scaled = image.size().scaled(target.size(), Qt::KeepAspectRatioByExpanding);
    const double scale = static_cast<double>(scaled.width()) / image.width();
    const QSizeF visible(target.width() / scale, target.height() / scale);
    const QRectF source((image.width() - visible.width()) / 2.0,
                        (image.height() - visible.height()) / 2.0, visible.width(),
                        visible.height());
    painter.drawImage(QRectF(target), image, source);
}

QPixmap HomeBackground::ScaledHero(u64 program_id, const QSize& target, qreal dpr) {
    if (program_id == 0) {
        return {};
    }
    const QString path = QtCommon::GameArt::HeroPath(program_id);
    const QFileInfo info(path);
    if (!info.isFile()) {
        return {};
    }
    // The file's stamp is part of the key: saving or removing a banner changes the pixels
    // without changing the id, and a stale entry would keep the old image on screen.
    const QString key = QStringLiteral("eglantine-hero-backdrop-%1-%2x%3-%4-%5")
                            .arg(program_id)
                            .arg(target.width())
                            .arg(target.height())
                            .arg(dpr)
                            .arg(info.lastModified().toMSecsSinceEpoch());
    QPixmap cached;
    if (QPixmapCache::find(key, &cached)) {
        return cached;
    }
    QImageReader reader(path);
    const QSize full = reader.size();
    if (!full.isValid() || full.isEmpty()) {
        return {};
    }
    // Decode and blur at half resolution: the blur hides it, and the cache stays small.
    const QSize physical(qRound(target.width() * dpr / 2.0), qRound(target.height() * dpr / 2.0));
    if (physical.isEmpty()) {
        return {};
    }
    QSize scaled = full;
    scaled.scale(physical, Qt::KeepAspectRatioByExpanding);
    reader.setScaledSize(scaled);
    QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    const int left = std::max(0, (image.width() - physical.width()) / 2);
    const int top = std::max(0, (image.height() - physical.height()) / 2);
    image = image.copy(left, top, physical.width(), physical.height());
    BlurImage(image, std::max(2, qRound(6 * dpr)));
    // Stretch back to the full physical size once, so every frame is an exact 1:1 blit. The
    // stretch is done here rather than with a sub-1 device pixel ratio, which paint paths are
    // free to ignore.
    const QSize full_physical(qRound(target.width() * dpr), qRound(target.height() * dpr));
    image = image.scaled(full_physical, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QPixmap out = QPixmap::fromImage(image);
    out.setDevicePixelRatio(dpr);
    QPixmapCache::insert(key, out);
    return out;
}

QColor HomeBackground::ColorFromString(const QString& name) {
    const QColor color(name);
    return color.isValid() ? color : QColor(30, 33, 40);
}

bool HomeBackground::SetSource(const QString& path) {
    if (kind != Kind::Picture) {
        // Selecting a picture switches back to it, even when that picture is already loaded.
        kind = Kind::Picture;
        UpdatePlayback();
        emit FrameChanged();
    }
    if (path == source && (path.isEmpty() || !frame.isNull())) {
        return true;
    }
    Clear();
    if (path.isEmpty()) {
        emit FrameChanged();
        return true;
    }
    if (!CanLoad(path)) {
        emit FrameChanged();
        return false;
    }

    source = path;
    if (IsVideo(path)) {
#if YUZU_USE_QT_MULTIMEDIA
        player = new QMediaPlayer(this);
        sink = new QVideoSink(this);
        player->setVideoSink(sink);
        player->setLoops(QMediaPlayer::Infinite);
        // No QAudioOutput is attached, so the video plays muted.
        connect(sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& video) {
            QImage image = video.toImage();
            if (!image.isNull()) {
                frame = std::move(image);
                emit FrameChanged();
            }
        });
        player->setSource(QUrl::fromLocalFile(path));
#endif
    } else if (IsAnimatedImage(path)) {
        movie = new QMovie(path, {}, this);
        movie->setCacheMode(QMovie::CacheNone);
        connect(movie, &QMovie::frameChanged, this, [this] {
            frame = movie->currentImage();
            emit FrameChanged();
        });
        movie->jumpToFrame(0);
        frame = movie->currentImage();
    } else {
        frame = ReadStill(path, QSize(MaxStillDimension, MaxStillDimension));
        if (frame.isNull()) {
            source.clear();
            emit FrameChanged();
            return false;
        }
    }

    UpdatePlayback();
    emit FrameChanged();
    return true;
}

void HomeBackground::SetSolid(const QColor& solid) {
    kind = Kind::Solid;
    color = solid;
    UpdatePlayback();
    emit FrameChanged();
}

void HomeBackground::SetGradient(const QColor& start, const QColor& end) {
    kind = Kind::Gradient;
    color = start;
    color2 = end;
    UpdatePlayback();
    emit FrameChanged();
}

void HomeBackground::SetActive(bool active_) {
    if (active == active_) {
        return;
    }
    active = active_;
    UpdatePlayback();
}

void HomeBackground::UpdatePlayback() {
    // A fill replaces the picture on screen, so its playback pauses until it shows again.
    const bool playing = active && kind == Kind::Picture;
    if (movie) {
        if (!playing) {
            movie->setPaused(true);
        } else if (movie->state() == QMovie::Paused) {
            movie->setPaused(false);
        } else if (movie->state() == QMovie::NotRunning) {
            movie->start();
        }
    }
#if YUZU_USE_QT_MULTIMEDIA
    if (player) {
        if (playing) {
            player->play();
        } else {
            player->pause();
        }
    }
#endif
}

void HomeBackground::Clear() {
    if (movie) {
        movie->disconnect(this);
        movie->stop();
        movie->deleteLater();
        movie = nullptr;
    }
#if YUZU_USE_QT_MULTIMEDIA
    if (player) {
        player->stop();
        player->deleteLater();
        player = nullptr;
    }
    if (sink) {
        sink->disconnect(this);
        sink->deleteLater();
        sink = nullptr;
    }
#endif
    source.clear();
    frame = {};
}
