// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QImage>
#include <QObject>
#include <QPixmap>
#include <QString>

#include "common/common_types.h"

class QMovie;
class QPainter;
class QRect;
class QSize;

#if YUZU_USE_QT_MULTIMEDIA
class QMediaPlayer;
class QVideoSink;
#endif

/// The home screen background: a picture (a still, an animated image or a muted video), a solid
/// color or a two-color gradient. Consumers paint Frame() for pictures and the kind's colors
/// otherwise, and repaint on FrameChanged.
class HomeBackground : public QObject {
    Q_OBJECT

public:
    /// What the background shows, in the order the home_background_kind setting stores them.
    enum class Kind : int { Picture, Solid, Gradient };

    explicit HomeBackground(QObject* parent = nullptr);
    ~HomeBackground() override;

    /// Loads `path` and switches back to the picture kind; an empty path clears the picture.
    /// Returns false if it could not be loaded.
    bool SetSource(const QString& path);
    /// Fills the background with one color, or with a diagonal blend of two. The loaded picture,
    /// if any, stays ready for a switch back to the picture kind.
    void SetSolid(const QColor& color);
    void SetGradient(const QColor& start, const QColor& end);
    const QString& Source() const {
        return source;
    }
    Kind GetKind() const {
        return kind;
    }
    /// The solid color, and the gradient's start color.
    const QColor& Color() const {
        return color;
    }
    /// The gradient's end color.
    const QColor& Color2() const {
        return color2;
    }
    bool IsEmpty() const {
        return kind == Kind::Picture && frame.isNull();
    }
    const QImage& Frame() const {
        return frame;
    }

    /// Pauses animation and video playback while nothing shows the background.
    void SetActive(bool active);

    static bool IsVideo(const QString& path);
    static bool CanLoad(const QString& path);
    static QString FileDialogFilter();
    /// A small still of `path` for previews; null if it cannot be decoded without playing it.
    static QImage LoadThumbnail(const QString& path, const QSize& size);
    /// Scales `image` to cover `target`, cropping the overflow evenly on both sides.
    static void PaintCover(QPainter& painter, const QRect& target, const QImage& image);
    /// A game's banner, cover-cropped to `target` and softly blurred, as a backdrop. Cached
    /// per size and file stamp; a null pixmap when the game has no banner.
    static QPixmap ScaledHero(u64 program_id, const QSize& target, qreal dpr);
    /// The stored kind value; anything outside Picture..Gradient reads as a picture.
    static Kind KindFromInt(int value) {
        return value == 1 ? Kind::Solid : value == 2 ? Kind::Gradient : Kind::Picture;
    }
    /// Parses a stored "#rrggbb" color; a missing or invalid value falls back to a neutral dark.
    static QColor ColorFromString(const QString& name);

signals:
    void FrameChanged();

private:
    void Clear();
    void UpdatePlayback();

    Kind kind = Kind::Picture;
    QColor color;
    QColor color2;
    QString source;
    QImage frame;
    bool active = true;

    QMovie* movie = nullptr;
#if YUZU_USE_QT_MULTIMEDIA
    QMediaPlayer* player = nullptr;
    QVideoSink* sink = nullptr;
#endif
};
