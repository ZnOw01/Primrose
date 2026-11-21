// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <vector>

#include <QFont>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include "common/common_types.h"
#include "yuzu/render/perf_sampler.h"

namespace VideoCore {
class ShaderNotify;
}
namespace Core {
struct PerfStatsResults;
}

class MainWindow;
class QPainter;
class QWindow;
class PerformanceOverlaySurface;

/// Frame pacing, host GPU load and host thread load over the running game. Frame times come
/// from PerfStats; threads and GPU load from a PerfSampler while the overlay is shown.
class PerformanceOverlay : public QWidget {
    Q_OBJECT

public:
    /// On Wayland, where a top-level window cannot place itself, the overlay is drawn in a child
    /// window of the top-level holding `render_window` (a subsurface over the game) instead of
    /// a tool window that follows the main window.
    explicit PerformanceOverlay(MainWindow* parent, QWidget* render_window);
    ~PerformanceOverlay() override;

    void setVisible(bool visible) override;

    /// Draws the panel; shared by the widget and the Wayland child window.
    void Paint(QPainter& p, const QSize& size);
    void DragStart(const QPoint& local);
    void DragMove(const QPoint& local, const QPoint& global);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    struct FrameStats {
        bool valid = false;
        double fps = 0.0;
        double avg_ms = 0.0;
        double max_ms = 0.0;
        double median_ms = 0.0;
        double low1_fps = 0.0;
        double low01_fps = 0.0;
        int hitches = 0;
    };

    void resetPosition(const QPoint& pos);
    void updateStats(const Core::PerfStatsResults& results, const VideoCore::ShaderNotify& shaders);
    void Tick();
    void SetActive(bool active);
    /// Wayland: keeps the child window on the render window's top-level at m_offset.
    void PlaceSurface();
    /// Whether the game's render surface is on screen (so a new subsurface goes above it).
    bool RenderSurfaceShown() const;
    QString Limiter() const;
    /// The limiter verdict wrapped to `width`.
    QStringList LimiterLines(int width) const;
    /// Thread rows to draw, and how many busy ones did not fit.
    std::vector<PerfThreadGroup> ShownThreads(int& hidden) const;
    int ThreadColumns(size_t rows) const;

    MainWindow* m_mainWindow = nullptr;
    bool m_embedded = false;
    QPointer<QWidget> m_renderWindow;
    QPointer<PerformanceOverlaySurface> m_surface;
    bool m_surface_wanted = false;
    QPoint m_offset{25, 75};
    QPoint m_drag_start_pos;

    QTimer m_timer;
    std::unique_ptr<PerfSampler> m_sampler;

    QString m_title;
    QFont m_font;
    QFont m_bold;
    QFont m_big;

    // Refreshed every tick
    std::vector<double> m_graph_ms; ///< the last 300 frame intervals
    FrameStats m_frames;
    PerfHostSnapshot m_host;
    std::optional<u64> m_vram_used;
    u64 m_vram_total = 0;
    std::optional<double> m_speed;
    QString m_limiter;

signals:
    void closed();
};
