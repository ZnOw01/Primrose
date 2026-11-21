// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <memory>
#include <QString>
#include <QWidget>
#include <QtGlobal>

#if !QT_CONFIG(movie)
#define YUZU_QT_MOVIE_MISSING 1
#endif

namespace Loader {
class AppLoader;
}

namespace VideoCore {
enum class LoadCallbackStage;
}

class QBuffer;
class QByteArray;
class QGraphicsOpacityEffect;
class QLabel;
class QMovie;
class QPropertyAnimation;
class LoadingProgressBar;
class LoadingBackdrop;

class LoadingScreen : public QWidget {
    Q_OBJECT

public:
    explicit LoadingScreen(QWidget* parent = nullptr);

    ~LoadingScreen();

    /// Call before showing the loading screen to load the widgets with the icon, title, logo and
    /// banner for the currently loaded application.
    void Prepare(Loader::AppLoader& loader);

    /// After the loading screen is hidden, the owner of this class can call this to clean up any
    /// used resources such as the logo and banner.
    void Clear();

    /// Slot used to update the status of the progress bar
    void OnLoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);

    /// Hides the LoadingScreen with a fade out effect
    void OnLoadComplete();

signals:
    void LoadProgress(VideoCore::LoadCallbackStage stage, std::size_t value, std::size_t total);
    /// Signals that this widget is completely hidden now and should be replaced with the other
    /// widget
    void Hidden();

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    using Clock = std::chrono::steady_clock;

    void ApplyPalette();
    void ShowBusy(const QString& text);
    void UpdateBuildText(std::size_t value, std::size_t total, Clock::time_point now);
    QString FormatRemaining(std::chrono::seconds remaining) const;

#ifndef YUZU_QT_MOVIE_MISSING
    std::unique_ptr<QMovie> animation;
    std::unique_ptr<QBuffer> backing_buf;
    std::unique_ptr<QByteArray> backing_mem;
#endif

    LoadingBackdrop* fade_parent = nullptr;
    QLabel* logo = nullptr;
    QLabel* icon = nullptr;
    QLabel* title = nullptr;
    QLabel* stage_label = nullptr;
    LoadingProgressBar* progress_bar = nullptr;
    QLabel* detail = nullptr;
    QLabel* banner = nullptr;

    QGraphicsOpacityEffect* opacity_effect = nullptr;
    std::unique_ptr<QPropertyAnimation> fadeout_animation;

    VideoCore::LoadCallbackStage previous_stage;

    // Build stage timing. The rate is an exponential moving average over samples at least
    // RateSampleInterval apart, which keeps the estimate from jumping between cheap cached
    // pipelines and expensive new ones.
    Clock::time_point build_start;
    Clock::time_point last_sample_time;
    Clock::time_point last_text_update;
    std::size_t last_sample_value = 0;
    double pipelines_per_second = 0.0;
};

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
Q_DECLARE_METATYPE(VideoCore::LoadCallbackStage);
#endif
