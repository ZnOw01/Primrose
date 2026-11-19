// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>

namespace Vulkan::Nx2Sync {

inline bool HostImportChunks() {
    static constexpr bool enabled = true;
    return enabled;
}

inline bool MergeWaits() {
    static constexpr bool enabled = true;
    return enabled;
}

struct ImportChunk {
    uintptr_t start;
    uintptr_t size;

    static std::optional<ImportChunk> For(uintptr_t address, uint64_t size, uintptr_t chunk_size) {
        if (address == 0 || size == 0 || !std::has_single_bit(chunk_size) || size > chunk_size ||
            size > (std::numeric_limits<uintptr_t>::max)() - address) {
            return std::nullopt;
        }
        const uintptr_t first = address & ~(chunk_size - 1);
        if (size > chunk_size - (address - first) ||
            chunk_size > (std::numeric_limits<uintptr_t>::max)() - first) {
            return std::nullopt;
        }
        return ImportChunk{first, chunk_size};
    }

    bool Contains(uintptr_t address, uint64_t length) const {
        return address >= start && address - start < size && length <= size - (address - start);
    }
};

// Allocations finish before the first submission; late misses always copy.
class ImportWarmup {
public:
    bool Closed() const {
        return closing.load(std::memory_order_acquire);
    }

    template <typename F>
    bool Run(F&& import) {
        std::scoped_lock lock{mutex};
        if (Closed()) {
            return false;
        }
        import();
        return true;
    }

    void Close() {
        if (sealed.load(std::memory_order_acquire)) {
            return;
        }
        closing.store(true, std::memory_order_release);
        std::scoped_lock lock{mutex};
        sealed.store(true, std::memory_order_release);
    }

private:
    std::mutex mutex;
    std::atomic<bool> closing{};
    std::atomic<bool> sealed{};
};

// Equal ticks share one kernel wait. Different ticks must progress independently.
class TimelineWaits {
public:
    template <typename IsFree, typename Refresh, typename Wait>
    void Run(uint64_t tick, IsFree&& is_free, Refresh&& refresh, Wait&& wait) {
        if (is_free(tick)) {
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds{20};
        for (unsigned poll = 0; poll < 8; ++poll) {
            refresh();
            if (is_free(tick)) {
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
        }
        std::unique_lock lock{mutex};
        while (!is_free(tick)) {
            Slot* slot = nullptr;
            Slot* free_slot = nullptr;
            for (auto& candidate : slots) {
                if (candidate.users != 0 && candidate.tick == tick) {
                    slot = &candidate;
                    break;
                }
                if (candidate.users == 0) {
                    free_slot = &candidate;
                }
            }
            if (slot != nullptr && slot->waiting) {
                ++slot->users;
                cv.wait(lock, [&] { return !slot->waiting || is_free(tick); });
                --slot->users;
                continue;
            }
            if (slot == nullptr) {
                slot = free_slot;
            }
            if (slot == nullptr) {
                // A busy table must never serialize an earlier tick behind a later one.
                lock.unlock();
                wait(tick);
                refresh();
                return;
            }
            slot->tick = tick;
            slot->waiting = true;
            ++slot->users;
            lock.unlock();
            try {
                refresh();
                if (!is_free(tick)) {
                    wait(tick);
                    refresh();
                }
            } catch (...) {
                lock.lock();
                slot->waiting = false;
                --slot->users;
                lock.unlock();
                cv.notify_all();
                throw;
            }
            lock.lock();
            slot->waiting = false;
            --slot->users;
            cv.notify_all();
        }
    }

private:
    struct Slot {
        uint64_t tick{};
        size_t users{};
        bool waiting{};
    };
    std::array<Slot, 16> slots{};
    std::mutex mutex;
    std::condition_variable cv;
};

} // namespace Vulkan::Nx2Sync
