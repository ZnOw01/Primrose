// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace VideoCommon::Nx2GpuSide {

enum class Opt { CopyBatch, DrawState, QueryReadback, AsyncImport };

constexpr bool On(Opt) {
    return true;
}

} // namespace VideoCommon::Nx2GpuSide
