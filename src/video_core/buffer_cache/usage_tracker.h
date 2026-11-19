// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <span>
#include <utility>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_perf.h"

namespace VideoCommon {

class UsageTracker {
    // PAGE_SHIFT is a macro on FreeBSD
    static constexpr size_t BUFFER_BYTES_PER_BITSHIFT = 6;
    static constexpr size_t BUFFER_PAGE_SHIFT = 6 + BUFFER_BYTES_PER_BITSHIFT;
    static constexpr size_t BUFFER_PAGE_BYTES = 1 << BUFFER_PAGE_SHIFT;
public:
    explicit UsageTracker(size_t size) {
        const size_t num_pages = (size >> BUFFER_PAGE_SHIFT) + 1;
        pages.resize(num_pages, 0ULL);
        touched_begin = num_pages;
    }

    void Reset() noexcept {
        interval_begin = interval_end = 0;
        bitmap_mode = false;
        if (!touched) {
            return;
        }
        touched = false;
        static constexpr bool reset_range = true;
        if (reset_range) {
            if (touched_begin < touched_end) {
                std::fill(pages.begin() + touched_begin, pages.begin() + touched_end, 0ULL);
            }
        } else {
            std::ranges::fill(pages, 0ULL);
        }
        touched_begin = pages.size();
        touched_end = 0;
        recent.fill({~u64{0}, 0});
        full_begin = 0;
        full_end = 0;
    }

    void Track(u64 offset, u64 size) noexcept {
        if (IntervalEnabled() && !bitmap_mode && !bitmap_observed && size != 0) {
            const size_t page = offset >> BUFFER_PAGE_SHIFT;
            const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
            if (page_end < page || page_end >= pages.size()) {
                return;
            }
            constexpr u64 granule = u64{1} << BUFFER_BYTES_PER_BITSHIFT;
            const u64 begin = Common::AlignDown(offset, granule);
            const u64 end = Common::AlignUp(offset + size, granule);
            if (interval_begin == interval_end) {
                interval_begin = begin;
                interval_end = end;
                return;
            }
            if (begin <= interval_end && interval_begin <= end) {
                interval_begin = (std::min)(interval_begin, begin);
                interval_end = (std::max)(interval_end, end);
                return;
            }
            bitmap_mode = true;
            // One byte less marks the same granules without entering the sentinel page.
            TrackBitmap(interval_begin, interval_end - interval_begin - 1);
            interval_begin = interval_end = 0;
        }
        TrackBitmap(offset, size);
    }

private:
    void TrackBitmap(u64 offset, u64 size) const noexcept {
        if (size == 0) {
            return;
        }
        // Tracking is idempotent until Reset: skip ranges tracked just before. Draws bind the
        // same (often multi-megabyte) storage buffers again and again, and each Track wrote a
        // word per 4 KiB of the range.
        for (const auto& [recent_offset, recent_size] : recent) {
            if (recent_offset == offset && recent_size == size) {
                return;
            }
        }
        recent[recent_cursor++ % recent.size()] = {offset, size};
        touched = true;
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return;
        }
        touched_begin = (std::min)(touched_begin, page);
        touched_end = (std::max)(touched_end, page_end + 1);
        TrackPage(page, offset, size);
        if (page == page_end) {
            return;
        }
        FillPages(page + 1, page_end);
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        if (offset_end > offset_end_page_aligned) {
            TrackPage(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
        }
    }

public:
    [[nodiscard]] bool IsUsed(u64 offset, u64 size) const noexcept {
        if (size == 0) {
            return false;
        }
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return false;
        }
        if (interval_begin != interval_end) {
            return offset < interval_end && interval_begin < offset + size;
        }
        if (IsPageUsed(page, offset, size)) {
            return true;
        }
        if (page == page_end) {
            return false;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            if (pages[i] != 0) {
                return true;
            }
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        return offset_end > offset_end_page_aligned &&
               IsPageUsed(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
    }

    [[nodiscard]] std::span<const u64> Pages() const noexcept {
        // Keep later writes visible through a span returned by this diagnostic accessor.
        bitmap_observed = true;
        if (interval_begin != interval_end) {
            TrackBitmap(interval_begin, interval_end - interval_begin - 1);
            interval_begin = interval_end = 0;
            bitmap_mode = true;
        }
        return pages;
    }

private:
    [[nodiscard]] static bool IntervalEnabled() noexcept {
        static constexpr bool enabled = true;
        return enabled;
    }

    void FillPages(size_t begin, size_t end) const noexcept {
        if (begin >= end) {
            return;
        }
        // Pages in [full_begin, full_end) are all ones already (only Reset clears a page): fill
        // around that run and grow it. Keeps tracking a multi-megabyte binding O(1) once filled.
        if (!Nx2Perf::On(Nx2Perf::Opt::UsageTracker) || end < full_begin || begin > full_end) {
            std::fill(pages.begin() + begin, pages.begin() + end, ~u64{0});
            if (Nx2Perf::On(Nx2Perf::Opt::UsageTracker) && end - begin > full_end - full_begin) {
                full_begin = begin;
                full_end = end;
            }
            return;
        }
        if (Nx2Perf::Verify()) {
            static std::atomic<u32> logged{};
            const size_t check_begin = (std::max)(begin, full_begin);
            const size_t check_end = (std::min)(end, full_end);
            if (!std::all_of(pages.begin() + check_begin, pages.begin() + check_end,
                             [](u64 page) { return page == ~u64{0}; }) &&
                logged.fetch_add(1, std::memory_order_relaxed) < 64) {
                LOG_ERROR(HW_GPU, "Nx2Perf draw-binds: usage pages {}..{} not all tracked",
                          check_begin, check_end);
            }
            std::fill(pages.begin() + begin, pages.begin() + end, ~u64{0});
        } else {
            if (begin < full_begin) {
                std::fill(pages.begin() + begin, pages.begin() + full_begin, ~u64{0});
            }
            if (end > full_end) {
                std::fill(pages.begin() + full_end, pages.begin() + end, ~u64{0});
            }
        }
        full_begin = (std::min)(begin, full_begin);
        full_end = (std::max)(end, full_end);
    }

    [[nodiscard]] static u64 PageMask(u64 offset, u64 size) noexcept {
        if (size == 0) {
            return 0;
        }
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t end_in_page = std::min<size_t>(BUFFER_PAGE_BYTES, offset_in_page + size);
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t end_bit =
            (end_in_page + (size_t{1} << BUFFER_BYTES_PER_BITSHIFT) - 1) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t num_bits = end_bit - first_bit;
        if (num_bits == 0) {
            return 0;
        }
        const u64 mask = num_bits >= 64 ? ~u64{0} : ((u64{1} << num_bits) - 1);
        return mask << first_bit;
    }

    void TrackPage(u64 page, u64 offset, u64 size) const noexcept {
        pages[page] |= PageMask(offset, size);
    }

    bool IsPageUsed(u64 page, u64 offset, u64 size) const noexcept {
        return (pages[page] & PageMask(offset, size)) != 0;
    }

private:
    mutable std::vector<u64> pages;
    // A contiguous union needs no bitmap writes until a disjoint range arrives.
    mutable u64 interval_begin{};
    mutable u64 interval_end{};
    mutable bool bitmap_mode{};
    mutable bool bitmap_observed{};
    /// Ranges tracked last, (offset, size).
    mutable std::array<std::pair<u64, u64>, 4> recent{{{~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}}};
    mutable size_t recent_cursor{};
    mutable bool touched{}; ///< Tracked since the last Reset (Reset skips untouched trackers)
    mutable size_t touched_begin{};
    mutable size_t touched_end{};
    /// Pages known to be all ones (FillPages).
    mutable size_t full_begin{};
    mutable size_t full_end{};
};

} // namespace VideoCommon
