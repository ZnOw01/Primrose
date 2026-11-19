// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "common/common_types.h"
#include "common/cpu_features.h"

#if defined(ARCHITECTURE_x86_64)
#include <immintrin.h>
#endif

namespace VideoCommon {

#if defined(ARCHITECTURE_x86_64)
#if defined(_MSC_VER) && !defined(__clang__)
__declspec(noinline)
#else
__attribute__((target("avx2"), noinline))
#endif
inline bool BufferBytesEqualAvx2(const u8* left, const u8* right, size_t size) noexcept {
    while (size >= 128) {
        const auto diff0 = _mm256_xor_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(left)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(right)));
        const auto diff1 = _mm256_xor_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(left + 32)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(right + 32)));
        const auto diff2 = _mm256_xor_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(left + 64)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(right + 64)));
        const auto diff3 = _mm256_xor_si256(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(left + 96)),
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(right + 96)));
        const auto diff = _mm256_or_si256(_mm256_or_si256(diff0, diff1),
                                        _mm256_or_si256(diff2, diff3));
        if (!_mm256_testz_si256(diff, diff)) {
            return false;
        }
        left += 128;
        right += 128;
        size -= 128;
    }
    return size == 0 || std::memcmp(left, right, size) == 0;
}
#endif

[[nodiscard]] inline bool BufferBytesEqual(const u8* left, const u8* right, size_t size) noexcept {
#if defined(ARCHITECTURE_x86_64)
    if (Common::g_cpu_caps.avx2 && size >= 128) {
        return BufferBytesEqualAvx2(left, right, size);
    }
#endif
    return std::memcmp(left, right, size) == 0;
}

} // namespace VideoCommon
