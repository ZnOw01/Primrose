// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include "common/common_types.h"

namespace VideoCommon {

[[nodiscard]] inline bool NativeTextureCacheBudgetEnabled() {
    static constexpr bool enabled = false;
    return enabled;
}

[[nodiscard]] constexpr u64 NativeTextureCacheCriticalLimit(u64 critical_budget, u64 heap_budget,
                                                           u64 legacy_limit) {
    constexpr u64 small_card_budget = u64{8} << 30;
    const u64 limit = heap_budget <= small_card_budget ? legacy_limit
                                                       : (std::max)(legacy_limit, critical_budget);
    return (std::min)(critical_budget, limit);
}

[[nodiscard]] constexpr u64 TextureCacheCollectionAge(bool high_priority, bool aggressive,
                                                      u64 idle_age) {
    return aggressive ? 10 : high_priority ? 25 : (std::clamp<u64>)(idle_age, 50, 3600);
}

[[nodiscard]] inline u64 NativeTextureCacheIdleAge() {
    static constexpr u64 age = 50;
    return age;
}

} // namespace VideoCommon
