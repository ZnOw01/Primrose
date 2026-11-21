// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/// Host threads of this process grouped by name, busy % of one core over the last ~1 s.
struct PerfThreadGroup {
    std::string name;
    int count = 0;
    double busy_max = 0.0;
};

struct PerfHostSnapshot {
    /// Busiest first.
    std::vector<PerfThreadGroup> threads;
    /// Host GPU load: this process's when the platform reports it, else the whole device's.
    std::optional<double> gpu_busy;
    bool gpu_busy_device_wide = false;
    /// This process's CPU use over the last ~1 s, as a share of all logical cores.
    std::optional<double> process_cpu;
    /// Resident memory of this process (working set on Windows), in bytes.
    std::optional<unsigned long long> ram_used;
};

/// Samples host thread and process CPU time, process memory and GPU load every 250 ms on its
/// own thread, since GPU queries (NVML, PDH) can take milliseconds.
class PerfSampler {
public:
    PerfSampler();
    ~PerfSampler();

    PerfSampler(const PerfSampler&) = delete;
    PerfSampler& operator=(const PerfSampler&) = delete;

    PerfHostSnapshot Snapshot() const;

private:
    struct Impl;

    void Run(std::stop_token stop);

    std::unique_ptr<Impl> impl;
    mutable std::mutex mutex;
    PerfHostSnapshot snapshot;
    std::jthread thread;
};
