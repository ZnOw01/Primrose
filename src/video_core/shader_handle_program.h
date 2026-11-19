// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/shader_info.h"
#include "video_core/memory_manager.h"

namespace VideoCommon {

/// The texture handle a shader reads from a storage buffer, computed from the current constant
/// buffer and storage buffer words (Shader::Info::handle_programs). cbuf_address(index) is the
/// bound constant buffer's address, 0 when none is bound. storage_alignment is the host storage
/// buffer offset alignment: storage offsets in the program are relative to the buffer address
/// aligned down to it (StorageOffset), which is also where the buffer cache binds the buffer.
template <typename Memory, typename CbufAddress>
u32 EvaluateTextureHandle(const Shader::Info& info, u32 program, Memory& gpu_memory,
                          CbufAddress&& cbuf_address, u64 storage_alignment) {
    const GPUVAddr storage_mask{~(GPUVAddr{(std::max)(storage_alignment, u64{1})} - 1)};
    return Shader::EvaluateHandleProgram(
               info.handle_programs[program],
               [&](u32 index, u32 offset) -> u32 {
                   const GPUVAddr base{cbuf_address(index)};
                   return base != 0 ? gpu_memory.template Read<u32>(base + offset) : 0U;
               },
               [&](u32 binding, u32 offset) -> u32 {
                   if (binding >= info.storage_buffers_descriptors.size()) {
                       return 0U;
                   }
                   const auto& descriptor{info.storage_buffers_descriptors[binding]};
                   const GPUVAddr cbuf{cbuf_address(descriptor.cbuf_index)};
                   if (cbuf == 0) {
                       return 0U;
                   }
                   GPUVAddr base{gpu_memory.template Read<u64>(cbuf + descriptor.cbuf_offset)};
                   if (base != 0 && descriptor.indirect_offset != ~0U) {
                       // The table entry is read at its exact address. Only the buffer address
                       // it holds is aligned down, as the shader does.
                       base = gpu_memory.template Read<u64>(base + descriptor.indirect_offset);
                   }
                   base &= storage_mask;
                   return base != 0 ? gpu_memory.template Read<u32>(base + offset) : 0U;
               })
        .value_or(0U);
}

// Cached arithmetic is reused only after all of its input words have been read again.
class TextureHandleCache {
public:
    static bool Enabled() {
        static constexpr bool enabled = true;
        return enabled;
    }

    template <typename Memory, typename CbufAddress>
    u32 Evaluate(const Shader::Info& info, u32 program, Memory& memory,
                 CbufAddress&& cbuf_address, u64 storage_alignment) {
        if (!Enabled()) {
            return EvaluateTextureHandle(info, program, memory, cbuf_address, storage_alignment);
        }
        bool unchanged = valid && last_info == &info && last_program == program &&
                         last_alignment == storage_alignment;
        if (unchanged) {
            for (const auto& input : cbuf_inputs) {
                if (cbuf_address(input.index) != input.address) {
                    unchanged = false;
                    break;
                }
            }
        }
        if (unchanged) {
            for (const auto& input : memory_inputs) {
                const u64 value = input.size == sizeof(u32)
                                      ? memory.template Read<u32>(input.address)
                                      : memory.template Read<u64>(input.address);
                if (value != input.value) {
                    unchanged = false;
                    break;
                }
            }
        }
        if (unchanged) {
            return result;
        }
        valid = false;
        cbuf_inputs.clear();
        memory_inputs.clear();
        RecordingMemory<Memory> recording{memory, memory_inputs};
        result = EvaluateTextureHandle(
            info, program, recording,
            [&](u32 index) -> GPUVAddr {
                const GPUVAddr address = cbuf_address(index);
                cbuf_inputs.push_back({index, address});
                return address;
            },
            storage_alignment);
        last_info = &info;
        last_program = program;
        last_alignment = storage_alignment;
        valid = true;
        return result;
    }

private:
    struct CbufInput {
        u32 index;
        GPUVAddr address;
    };
    struct MemoryInput {
        GPUVAddr address;
        u64 value;
        size_t size;
    };
    template <typename Memory>
    struct RecordingMemory {
        Memory& memory;
        std::vector<MemoryInput>& inputs;

        template <typename T>
        T Read(GPUVAddr address) const {
            const T value = memory.template Read<T>(address);
            inputs.push_back({address, value, sizeof(T)});
            return value;
        }
    };
    std::vector<CbufInput> cbuf_inputs;
    std::vector<MemoryInput> memory_inputs;
    const Shader::Info* last_info{};
    u64 last_alignment{};
    u32 last_program{};
    u32 result{};
    bool valid{};
};

} // namespace VideoCommon
