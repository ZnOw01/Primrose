// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <numeric>

#include <QCoreApplication>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWindow>
#include <QRasterWindow>
#include <QSurfaceFormat>
#include <QTextLayout>

#include "common/memory_detect.h"
#include "core/core.h"
#include "core/perf_stats.h"
#include "qt_common/qt_common.h"
#include "video_core/gpu.h"
#include "video_core/renderer_base.h"
#include "yuzu/main_window.h"
#include "yuzu/render/performance_overlay.h"

namespace {

constexpr int Pad = 8;
constexpr int LineHeight = 16;
constexpr int GraphHeight = 62;
constexpr int PanelWidth = 452;
constexpr int ColumnGap = 14;
constexpr size_t GraphFrames = 300;
constexpr int RowsPerColumn = 6;
constexpr int MaxColumns = 2;
constexpr double VsyncMs = 1000.0 / 60.0;
constexpr double BusyLimit = 85.0;
/// A thread named as the limiter stays it until it drops below 82 %.
constexpr double BusyHysteresis = 3.0;
/// Lines kept for the limiter verdict, so the panel never changes size.
constexpr int LimiterLineCount = 2;

const QColor White{236, 238, 242};
const QColor Dim{150, 156, 168};
const QColor Green{96, 214, 120};
const QColor Yellow{245, 200, 66};
const QColor Red{245, 78, 66};
const QColor Cyan{120, 200, 255};
const QColor Panel{10, 12, 16, 218};
const QColor GraphBackground{2, 3, 6, 235};
const QColor BarTrack{60, 64, 72, 200};
const QColor GuideText{200, 204, 212};

QColor BusyColour(double percent) {
    return percent >= 85 ? Red : percent >= 60 ? Yellow : Green;
}

QString Number(double value, int digits = 1) {
    return QString::number(value, 'f', digits);
}

/// sorted_ms ascending; the same definition as the eglantine-perf tools.
double Percentile(const std::vector<double>& sorted_ms, double fraction) {
    if (sorted_ms.empty()) {
        return 0.0;
    }
    const auto index = static_cast<long>(std::ceil(fraction * sorted_ms.size())) - 1;
    return sorted_ms[std::clamp<long>(index, 0, static_cast<long>(sorted_ms.size()) - 1)];
}

/// The trailing frames of `frames` that add up to at most `span_ms`.
std::vector<double> Tail(const std::vector<double>& frames, double span_ms) {
    double sum = 0.0;
    size_t first = frames.size();
    while (first > 0 && sum + frames[first - 1] <= span_ms) {
        sum += frames[--first];
    }
    return {frames.begin() + static_cast<std::ptrdiff_t>(first), frames.end()};
}

bool IsKeyThread(const std::string& name) {
    return name == "GPU" || name == "GPUBack" || name == "CPUThread" || name == "VulkanWorker" ||
           name == "VulkanPresent" || name.starts_with("CPUCore_");
}

QString DescribeThread(const std::string& name) {
    if (name == "GPU") {
        return QObject::tr("GPU thread: emulated GPU command processing");
    }
    if (name == "GPUBack") {
        return QObject::tr("GPU back thread: emulated GPU methods");
    }
    if (name.starts_with("CPUCore_")) {
        return QObject::tr("Guest CPU core %1 (includes guest spin-waits)")
            .arg(QString::fromStdString(name.substr(8)));
    }
    if (name == "CPUThread") {
        return QObject::tr("Guest CPU (single core)");
    }
    if (name == "VulkanWorker") {
        return QObject::tr("Vulkan submit worker");
    }
    return QObject::tr("Vulkan present thread");
}

struct Span {
    QString text;
    QColor colour;
    bool bold = false;
};

} // namespace

/// Wayland: a plain child window of the main window's top-level. It becomes a subsurface over
/// the game without making any widget native: native widgets there make Qt rebuild the window
/// tree, which destroys the surface the Vulkan swapchain presents to.
class PerformanceOverlaySurface : public QRasterWindow {
public:
    PerformanceOverlaySurface(PerformanceOverlay* overlay_, QWindow* parent)
        : QRasterWindow(parent), overlay{overlay_} {
        QSurfaceFormat format = this->format();
        format.setAlphaBufferSize(8);
        setFormat(format);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.fillRect(QRect(QPoint(), size()), Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        overlay->Paint(p, size());
    }

    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            overlay->DragStart(event->position().toPoint());
        }
    }

    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton) {
            overlay->DragMove(event->position().toPoint(),
                              event->globalPosition().toPoint());
        }
    }

private:
    PerformanceOverlay* overlay;
};

PerformanceOverlay::PerformanceOverlay(MainWindow* parent, QWidget* render_window)
    : QWidget(parent), m_mainWindow{parent} {
    setAttribute(Qt::WA_TranslucentBackground);
    m_embedded = render_window != nullptr &&
                 QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
    if (m_embedded) {
        // The widget itself stays hidden; PerformanceOverlaySurface shows the panel.
        m_renderWindow = render_window;
        m_offset = {12, 12};
    } else {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool);
    }

    m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_font.setPixelSize(13);
    m_bold = m_font;
    m_bold.setBold(true);
    m_big = m_bold;
    m_big.setPixelSize(30);

    m_title = QFileInfo(QCoreApplication::applicationFilePath()).fileName();

    m_timer.setInterval(250);
    connect(&m_timer, &QTimer::timeout, this, &PerformanceOverlay::Tick);

    // Room for the most the panel shows: two columns of threads, the "+N more" line and a
    // two-line limiter verdict.
    const int height = Pad + (LineHeight + 2) + 34 + (2 + GraphHeight + 4) + (2 + LineHeight) +
                       (3 + (RowsPerColumn + 1) * LineHeight) +
                       (3 + LimiterLineCount * LineHeight) + Pad;
    setFixedSize(PanelWidth, height);
    if (!m_embedded) {
        raise();
    }

    resetPosition(m_mainWindow->pos());
    if (!m_embedded) {
        connect(parent, &MainWindow::positionChanged, this, &PerformanceOverlay::resetPosition);
    }
    connect(m_mainWindow, &MainWindow::statsUpdated, this, &PerformanceOverlay::updateStats);
}

PerformanceOverlay::~PerformanceOverlay() {
    delete m_surface.data();
}

void PerformanceOverlay::setVisible(bool visible) {
    if (!m_embedded) {
        QWidget::setVisible(visible);
        return;
    }
    // Wayland stacks the newest subsurface on top and QtWayland's raise() only places a
    // subsurface right above its parent, so the panel's surface is created once the game is
    // presenting (in Tick), after the game's own surface, and destroyed when hidden.
    m_surface_wanted = visible;
    if (!visible) {
        delete m_surface.data();
    }
    SetActive(visible);
}

void PerformanceOverlay::showEvent(QShowEvent* event) {
    SetActive(true);
    QWidget::showEvent(event);
}

void PerformanceOverlay::hideEvent(QHideEvent* event) {
    SetActive(false);
    QWidget::hideEvent(event);
}

void PerformanceOverlay::SetActive(bool active) {
    if (active) {
        if (!m_sampler) {
            m_sampler = std::make_unique<PerfSampler>();
        }
        m_timer.start();
        Tick();
        return;
    }
    // Thread and GPU sampling cost nothing while the overlay is hidden.
    m_timer.stop();
    m_sampler.reset();
}

bool PerformanceOverlay::RenderSurfaceShown() const {
    if (!m_renderWindow || !m_renderWindow->window()->windowHandle()) {
        return false;
    }
    // The game's surface only becomes a subsurface once the render widget is shown, which
    // can be after frames are already being presented.
    const auto children = m_renderWindow->findChildren<QWidget*>(Qt::FindDirectChildrenOnly);
    return std::any_of(children.begin(), children.end(), [this](const QWidget* child) {
        return child != this && child->windowHandle() && child->windowHandle()->isExposed();
    });
}

void PerformanceOverlay::PlaceSurface() {
    if (!m_surface || !m_renderWindow) {
        return;
    }
    QWidget* const top = m_renderWindow->window();
    // The render window can move to another top-level (fullscreen, detached render window).
    if (top->windowHandle() && m_surface->parent() != top->windowHandle()) {
        m_surface->setParent(top->windowHandle());
    }
    m_offset = {std::clamp(m_offset.x(), 0, std::max(0, m_renderWindow->width() - width())),
                std::clamp(m_offset.y(), 0, std::max(0, m_renderWindow->height() - height()))};
    const QPoint position = m_renderWindow->mapTo(top, m_offset);
    if (m_surface->position() != position) {
        m_surface->setPosition(position);
    }
}

void PerformanceOverlay::resetPosition(const QPoint& _) {
    if (m_embedded) {
        PlaceSurface();
        return;
    }
    auto pos = m_mainWindow->pos();
    move(pos.x() + m_offset.x(), pos.y() + m_offset.y());
}

void PerformanceOverlay::updateStats(const Core::PerfStatsResults& results,
                                     const VideoCore::ShaderNotify&) {
    if (!std::isnan(results.emulation_speed)) {
        m_speed = results.emulation_speed * 100.0;
    }
}

void PerformanceOverlay::Tick() {
    auto& system = QtCommon::system;
    if (!system || !system->IsPoweredOn()) {
        return;
    }

    const std::vector<double> recent =
        system->GetPerfStats().GetRecentFrameTimes(std::chrono::seconds{30});
    m_graph_ms.assign(recent.end() - static_cast<std::ptrdiff_t>(
                                         std::min(recent.size(), GraphFrames)),
                      recent.end());

    m_frames = {};
    const std::vector<double> second = Tail(recent, 1000.0);
    if (!second.empty()) {
        std::vector<double> ten = Tail(recent, 10000.0);
        std::sort(ten.begin(), ten.end());
        std::vector<double> sorted_second = second;
        std::sort(sorted_second.begin(), sorted_second.end());

        const double sum = std::accumulate(second.begin(), second.end(), 0.0);
        const double median_ten = Percentile(ten, 0.5);
        m_frames.valid = true;
        m_frames.avg_ms = sum / second.size();
        m_frames.fps = sum > 0 ? 1000.0 * second.size() / sum : 0.0;
        m_frames.max_ms = sorted_second.back();
        m_frames.median_ms = Percentile(sorted_second, 0.5);
        m_frames.low1_fps = 1000.0 / std::max(0.001, Percentile(ten, 0.99));
        m_frames.low01_fps = 1000.0 / std::max(0.001, Percentile(ten, 0.999));
        m_frames.hitches = static_cast<int>(std::count_if(ten.begin(), ten.end(), [&](double ms) {
            return ms > 2.0 * median_ten && ms > 8.0;
        }));
    }

    m_host = m_sampler ? m_sampler->Snapshot() : PerfHostSnapshot{};
    if (const auto memory = system->GPU().Renderer().GetDeviceMemory()) {
        m_vram_used = memory->used;
        m_vram_total = memory->total;
    } else {
        m_vram_used.reset();
    }
    m_limiter = Limiter();
    if (m_surface_wanted && !m_surface && RenderSurfaceShown()) {
        m_surface = new PerformanceOverlaySurface(this, m_renderWindow->window()->windowHandle());
        m_surface->resize(size());
        PlaceSurface();
        m_surface->show();
    }
    if (m_surface) {
        PlaceSurface();
        m_surface->update();
    } else {
        update();
    }
}

QString PerformanceOverlay::Limiter() const {
    const PerfThreadGroup* busiest = nullptr;
    for (const auto& group : m_host.threads) {
        if (IsKeyThread(group.name) && (!busiest || group.busy_max > busiest->busy_max)) {
            busiest = &group;
        }
    }
    double limit = BusyLimit;
    if (busiest && m_limiter == DescribeThread(busiest->name)) {
        limit -= BusyHysteresis;
    }
    const bool saturated = busiest && busiest->busy_max >= limit;
    const double gpu = m_host.gpu_busy.value_or(0.0);

    if (m_frames.valid && m_frames.median_ms > 0 && !saturated && gpu < 90) {
        const double periods = std::round(m_frames.median_ms / VsyncMs);
        if (periods >= 1 && periods <= 4 &&
            std::abs(m_frames.median_ms - periods * VsyncMs) < 0.05 * periods * VsyncMs &&
            m_frames.fps >= 0.95 * 1000.0 / (periods * VsyncMs)) {
            return tr("Steady cap (speed limit or vsync; Ctrl+U uncaps)");
        }
    }
    if (m_host.gpu_busy && gpu >= 90) {
        return m_host.gpu_busy_device_wide ? tr("Host GPU (device-wide load)") : tr("Host GPU");
    }
    if (saturated) {
        return DescribeThread(busiest->name);
    }
    return tr("No thread saturated: sync/latency bound");
}

std::vector<PerfThreadGroup> PerformanceOverlay::ShownThreads(int& hidden) const {
    std::vector<PerfThreadGroup> rows;
    for (const auto& group : m_host.threads) {
        // HostTiming spins by design; idle helper threads would only add noise.
        if (group.name != "HostTiming" && group.busy_max >= 1.0) {
            rows.push_back(group);
        }
    }
    const size_t cap = static_cast<size_t>(RowsPerColumn * MaxColumns);
    hidden = static_cast<int>(rows.size() > cap ? rows.size() - cap : 0);
    rows.resize(std::min(rows.size(), cap));
    return rows;
}

int PerformanceOverlay::ThreadColumns(size_t rows) const {
    return rows > static_cast<size_t>(RowsPerColumn) ? 2 : 1;
}

void PerformanceOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    Paint(p, size());
}

void PerformanceOverlay::Paint(QPainter& p, const QSize& size) {
    p.setRenderHint(QPainter::Antialiasing);

    const int w = size.width();
    QPainterPath panel;
    panel.addRoundedRect(QRectF(QPointF(), QSizeF(size)), 6, 6);
    p.fillPath(panel, Panel);

    const QFontMetrics fm(m_font);
    const int ascent = fm.ascent() + (LineHeight - fm.height()) / 2;

    const auto text = [&](int x, int y, const QString& s, const QColor& colour,
                          const QFont& font) {
        p.setFont(font);
        p.setPen(colour);
        p.drawText(x, y + ascent, s);
        return p.fontMetrics().horizontalAdvance(s);
    };
    const auto spans = [&](int x, int y, std::initializer_list<Span> parts) {
        for (const Span& part : parts) {
            x += text(x, y, part.text, part.colour, part.bold ? m_bold : m_font);
        }
        return x;
    };

    int y = Pad;

    // Header: tool name and executable, emulation speed on the right
    int x = Pad + text(Pad, y, QStringLiteral("EGL PERF "), Cyan, m_bold);
    text(x, y, m_title, Dim, m_font);
    if (m_speed) {
        const QString value = QStringLiteral("%1%").arg(std::round(*m_speed), 0, 'f', 0);
        const QString label = tr("speed ");
        const int right = w - Pad - fm.horizontalAdvance(value);
        text(right - fm.horizontalAdvance(label), y, label, Dim, m_font);
        text(right, y, value, *m_speed >= 97 ? White : Yellow, m_font);
    }
    y += LineHeight + 2;

    // FPS and frame-time stats
    const QString fps = m_frames.valid ? Number(m_frames.fps, 0) : QStringLiteral("-");
    p.setFont(m_big);
    p.setPen(White);
    const QFontMetrics big(m_big);
    p.drawText(Pad, y - 2 + big.ascent(), fps);
    text(Pad + big.horizontalAdvance(fps) + 4, y + 14, QStringLiteral("fps"), Dim, m_font);

    const int stats_x = Pad + 118;
    const auto value = [&](double v, int digits = 1) {
        return m_frames.valid ? Number(v, digits) : QStringLiteral("-");
    };
    spans(stats_x, y,
          {{tr("avg "), Dim}, {value(m_frames.avg_ms), White}, {tr(" ms  max "), Dim},
           {value(m_frames.max_ms), White}, {tr(" ms"), Dim}});
    spans(stats_x, y + LineHeight,
          {{QStringLiteral("1% "), Dim},
           {value(m_frames.low1_fps), White},
           {QStringLiteral("  0.1% "), Dim},
           {value(m_frames.low01_fps), White},
           {tr("  hitch "), Dim},
           {QString::number(m_frames.hitches), m_frames.hitches > 0 ? Yellow : White}});
    y += 34;

    // Frame-time graph: one bar per frame, slow frames yellow, hitches red
    {
        const QRectF area(Pad, y + 2, w - 2 * Pad, GraphHeight);
        p.fillRect(area, GraphBackground);
        const double peak =
            m_graph_ms.empty() ? 0.0 : *std::max_element(m_graph_ms.begin(), m_graph_ms.end());
        const double top =
            peak <= 45 ? 50.0 : std::min(100.0, std::ceil(peak * 1.1 / 10.0) * 10.0);
        const auto bar_y = [&](double ms) {
            return area.bottom() - std::min(area.height(), ms / top * area.height());
        };

        p.setPen(QPen(QColor(150, 154, 160, 235), 1, Qt::DashLine));
        for (const double guide : {VsyncMs, 2 * VsyncMs}) {
            p.drawLine(QPointF(area.left(), bar_y(guide)), QPointF(area.right(), bar_y(guide)));
        }

        if (!m_graph_ms.empty()) {
            std::vector<double> sorted = m_graph_ms;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            const double median = sorted[sorted.size() / 2];
            const double step = area.width() / GraphFrames;
            const double start = area.right() - m_graph_ms.size() * step;

            QPainterPath bars;
            bars.moveTo(start, area.bottom());
            for (size_t i = 0; i < m_graph_ms.size(); ++i) {
                bars.lineTo(start + i * step, bar_y(m_graph_ms[i]));
                bars.lineTo(start + (i + 1) * step, bar_y(m_graph_ms[i]));
            }
            bars.lineTo(area.right(), area.bottom());
            bars.closeSubpath();
            p.setRenderHint(QPainter::Antialiasing, false);
            p.fillPath(bars, QColor(Green.red(), Green.green(), Green.blue(), 235));
            for (size_t i = 0; i < m_graph_ms.size(); ++i) {
                const double ms = m_graph_ms[i];
                if (ms <= 1.4 * median) {
                    continue;
                }
                const bool hitch = ms > 2 * median && ms > 8;
                const double wide = std::max(hitch ? 2.0 : 1.0, step); // hitches stay visible
                const double bx = start + i * step;
                p.fillRect(QRectF(bx, bar_y(ms), wide, area.bottom() - bar_y(ms)),
                           hitch ? Red : Yellow);
            }
            p.setRenderHint(QPainter::Antialiasing);
        }

        for (const auto& [guide, label] :
             {std::pair{VsyncMs, QStringLiteral("16.7")}, std::pair{2 * VsyncMs, QStringLiteral("33.3")}}) {
            const int gy = static_cast<int>(bar_y(guide));
            const int lw = fm.horizontalAdvance(label);
            p.fillRect(QRect(static_cast<int>(area.right()) - lw - 4, gy - 15, lw + 4, 14),
                       QColor(0, 0, 0, 220));
            text(static_cast<int>(area.right()) - lw - 2, gy - 16, label, GuideText, m_font);
        }
        text(Pad + 3, static_cast<int>(area.top()), QStringLiteral("%1 ms").arg(top, 0, 'f', 0),
             GuideText, m_font);
        y = static_cast<int>(area.bottom()) + 4;
    }

    // Host resources: process CPU and GPU load, RAM and VRAM against what the machine has
    y += 2;
    x = Pad;
    const auto percent = [](const std::optional<double>& load) {
        return load ? QStringLiteral("%1%").arg(*load, 0, 'f', 0) : QStringLiteral("-");
    };
    const auto gib = [](u64 bytes) {
        return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 'f', 1);
    };
    // Memory turns yellow past 85 % of the total and red past 95 %.
    const auto fill_colour = [](u64 used, u64 total) {
        const double share = total ? static_cast<double>(used) / static_cast<double>(total) : 0;
        return share >= 0.95 ? Red : share >= 0.85 ? Yellow : White;
    };
    x = spans(x, y,
              {{tr("CPU "), Dim},
               {percent(m_host.process_cpu), White},
               {QStringLiteral("  "), Dim},
               // GPU* marks a whole-device figure, when this process's own is not reported.
               {m_host.gpu_busy_device_wide ? tr("GPU* ") : tr("GPU "), Dim},
               {percent(m_host.gpu_busy),
                m_host.gpu_busy ? BusyColour(*m_host.gpu_busy * 0.95) : White},
               {QStringLiteral("  "), Dim}});
    const u64 ram_total = Common::GetMemInfo().TotalPhysicalMemory;
    if (m_host.ram_used && ram_total) {
        x = spans(x, y,
                  {{tr("RAM "), Dim},
                   {gib(*m_host.ram_used), fill_colour(*m_host.ram_used, ram_total)},
                   {QStringLiteral("/%1 GiB  ").arg(gib(ram_total)), Dim}});
    }
    if (m_vram_used && m_vram_total) {
        spans(x, y,
              {{tr("VRAM "), Dim},
               {gib(*m_vram_used), fill_colour(*m_vram_used, m_vram_total)},
               {QStringLiteral("/%1 GiB").arg(gib(m_vram_total)), Dim}});
    }
    y += LineHeight;

    // Host threads, in two columns once there are more than fit in one
    y += 3;
    int hidden = 0;
    const auto rows = ShownThreads(hidden);
    const int columns = ThreadColumns(rows.size());
    const int per_column = static_cast<int>((rows.size() + columns - 1) / columns);
    const int column_width = (w - 2 * Pad - (columns - 1) * ColumnGap) / columns;
    const int name_width = columns > 1 ? 104 : 150;
    const int percent_width = fm.horizontalAdvance(QStringLiteral("100%")) + 6;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& group = rows[i];
        const int column = static_cast<int>(i) / std::max(1, per_column);
        const int row = static_cast<int>(i) % std::max(1, per_column);
        const int cx = Pad + column * (column_width + ColumnGap);
        const int cy = y + row * LineHeight;

        QString name = QString::fromStdString(group.name);
        if (group.count > 1) {
            name += QStringLiteral(" x%1").arg(group.count);
        }
        text(cx, cy, fm.elidedText(name, Qt::ElideRight, name_width - 6), White, m_font);

        const int bar_x = cx + name_width;
        const int bar_w = column_width - name_width - percent_width;
        const int bar_h = 10;
        const int bar_y = cy + (LineHeight - bar_h) / 2;
        p.fillRect(bar_x, bar_y, bar_w, bar_h, BarTrack);
        p.fillRect(bar_x, bar_y, static_cast<int>(bar_w * std::min(group.busy_max, 100.0) / 100.0),
                   bar_h, BusyColour(group.busy_max));
        const QString busy_text = QStringLiteral("%1%").arg(group.busy_max, 0, 'f', 0);
        text(cx + column_width - fm.horizontalAdvance(busy_text), cy, busy_text, White, m_font);
    }
    y += per_column * LineHeight;
    if (hidden > 0) {
        text(Pad, y, tr("+%n more busy thread(s)", "", hidden), Dim, m_font);
        y += LineHeight;
    }

    // Limiter verdict, wrapped with continuation lines under the text
    y += 3;
    const bool notable = !m_limiter.isEmpty() &&
                         !m_limiter.startsWith(QStringLiteral("Steady")) &&
                         !m_limiter.startsWith(QStringLiteral("No thread"));
    const int label_w = text(Pad, y, tr("limit") + QLatin1Char(' '), Dim, m_font);
    QStringList lines = LimiterLines(w - 2 * Pad - label_w);
    if (lines.size() > LimiterLineCount) {
        lines = lines.mid(0, LimiterLineCount);
        lines.back() = p.fontMetrics().elidedText(lines.back() + QStringLiteral(" ..."),
                                                  Qt::ElideRight, w - 2 * Pad - label_w);
    }
    for (const QString& line : lines) {
        text(Pad + label_w, y, line, notable ? Yellow : White, m_bold);
        y += LineHeight;
    }
}

QStringList PerformanceOverlay::LimiterLines(int width) const {
    const QString limiter = m_limiter.isEmpty() ? QStringLiteral("-") : m_limiter;
    QStringList lines;
    QTextLayout layout(limiter, m_bold);
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width);
        lines << limiter.mid(line.textStart(), line.textLength()).trimmed();
    }
    layout.endLayout();
    return lines;
}

void PerformanceOverlay::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        DragStart(event->pos());
    }
}

void PerformanceOverlay::mouseMoveEvent(QMouseEvent* event) {
    if (event->buttons() & Qt::LeftButton) {
        DragMove(event->pos(), event->globalPosition().toPoint());
    }
}

void PerformanceOverlay::DragStart(const QPoint& local) {
    m_drag_start_pos = local;
}

void PerformanceOverlay::DragMove(const QPoint& local, const QPoint& global) {
    if (m_embedded) {
        m_offset += local - m_drag_start_pos;
        PlaceSurface();
        return;
    }
    QPoint new_global_pos = global - m_drag_start_pos;
    m_offset = new_global_pos - m_mainWindow->pos();
    move(new_global_pos);
}

void PerformanceOverlay::closeEvent(QCloseEvent* event) {
    emit closed();
}
