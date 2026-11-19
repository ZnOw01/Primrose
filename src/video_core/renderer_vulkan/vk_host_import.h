// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <span>
#include <map>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <cstdint>
#include <unordered_map>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "video_core/nx2_gpu_side.h"
#include "video_core/renderer_vulkan/vk_nx2_sync.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

/// Guest memory imported as Vulkan buffers (VK_EXT_external_memory_host), so the host GPU reads
/// the guest's own pages with no copy (EDEN_NX2_HOST_IMPORT=1, experimental). A window is the
/// fixed chunk in chunk mode, or the page-aligned requested span otherwise. Windows live as
/// long as the device. Import failures and late chunk misses copy instead.
class HostImporter {
public:
    struct Ref {
        VkBuffer buffer{};
        VkDeviceAddress address{};
        u32 offset{}; ///< Of the asked range inside the window's buffer
    };

    explicit HostImporter(const Device& device_, Nx2Sync::ImportWarmup& warmup_)
        : device{device_}, warmup{warmup_} {
        if ((Async() || Nx2Sync::HostImportChunks()) && device.IsExtExternalMemoryHost()) {
            VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_properties{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
            };
            VkPhysicalDeviceProperties2 properties{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = &host_properties,
            };
            device.GetPhysical().GetProperties2(properties);
            page_mask = (std::max<uintptr_t>)(PageMask + 1,
                host_properties.minImportedHostPointerAlignment) - 1;
        }
    }

    ~HostImporter() {
        if (worker.joinable()) {
            worker.request_stop();
            cv.notify_all();
            worker.join();
        }
    }

    [[nodiscard]] bool Usable() const noexcept {
        return !failed.load(std::memory_order_relaxed) && device.IsExtExternalMemoryHost();
    }

    /// Imports (once) the pages covering [host_ptr, host_ptr + size) and describes the range.
    /// Returns false when it cannot, which the caller treats as "copy instead".
    [[nodiscard]] bool Get(const u8* host_ptr, u64 size, Ref& out, std::span<const u8> chunk = {}) {
        if (Nx2Sync::HostImportChunks()) {
            return GetChunk(host_ptr, size, out, chunk);
        }
        if (!Usable() || host_ptr == nullptr || size == 0) {
            return false;
        }
        const uintptr_t address = reinterpret_cast<uintptr_t>(host_ptr);
        if (address > (std::numeric_limits<uintptr_t>::max)() - page_mask ||
            size > (std::numeric_limits<uintptr_t>::max)() - address - page_mask) {
            return false;
        }
        const uintptr_t first = address & ~page_mask;
        const uintptr_t last = (address + size + page_mask) & ~page_mask;
        const Key key{first, last - first};
        std::unique_lock lock{mutex};
        auto it = windows.find(key);
        if (it == windows.end()) {
            if (windows.size() + pending.size() >= MaxWindows) {
                return false;
            }
            if (Async()) {
                if (pending.insert(key).second) {
                    requests.push_back(key);
                    if (!worker.joinable()) {
                        worker = std::jthread([this](std::stop_token token) { ImportLoop(token); });
                    }
                    cv.notify_one();
                }
                return false;
            }
            it = windows.emplace(key, Import(reinterpret_cast<const void*>(first), last - first))
                     .first;
            if (failed.load(std::memory_order_relaxed)) {
                return false;
            }
        }
        const Window& window = it->second;
        if (window.buffer == VK_NULL_HANDLE) {
            return false;
        }
        out = Ref{window.buffer, window.address, static_cast<u32>(address - first)};
        return true;
    }

    [[nodiscard]] uintptr_t ChunkSize() const {
        if (!Nx2Sync::HostImportChunks()) {
            return 0;
        }
        static constexpr uintptr_t size = 0x10000U;
        return page_mask < size ? size : (page_mask < 0x200000 ? 0x200000 : 0);
    }

    [[nodiscard]] size_t WindowCount() const noexcept {
        std::scoped_lock lock{mutex};
        return windows.size() + chunk_windows.size();
    }

private:
    static constexpr uintptr_t PageMask = 0xfff;
    static constexpr size_t MaxWindows = 1024;

    struct Key {
        uintptr_t start;
        uintptr_t size;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        size_t operator()(const Key& key) const noexcept {
            return std::hash<uintptr_t>{}(key.start) ^ (std::hash<uintptr_t>{}(key.size) << 1);
        }
    };
    using SparseBufferHandle = vk::Handle<VkBuffer, VkDevice, vk::DeviceDispatch>;
    struct Window {
        vk::DeviceMemory memory;
        SparseBufferHandle buffer_handle;
        VkBuffer buffer{};
        VkDeviceAddress address{};
        uintptr_t size{};
    };

    bool GetChunk(const u8* host_ptr, u64 size, Ref& out, std::span<const u8> chunk) {
        if (!Usable() || host_ptr == nullptr || size == 0) {
            return false;
        }
        const uintptr_t address = reinterpret_cast<uintptr_t>(host_ptr);
        // Publication never holds this lock across a Vulkan allocation.
        std::unique_lock lock{mutex, std::try_to_lock};
        if (!lock.owns_lock()) {
            return false;
        }
        auto it = chunk_windows.upper_bound(address);
        if (it != chunk_windows.begin()) {
            --it;
            if (Nx2Sync::ImportChunk{it->first, it->second.size}.Contains(address, size)) {
                if (!it->second.buffer) {
                    return false;
                }
                out = Ref{it->second.buffer, it->second.address,
                          static_cast<u32>(address - it->first)};
                return true;
            }
        }
        const auto planned = Nx2Sync::ImportChunk::For(address, size, ChunkSize());
        if (!planned || warmup.Closed() ||
            reinterpret_cast<uintptr_t>(chunk.data()) != planned->start ||
            chunk.size() != planned->size) {
            return false;
        }
        const Key key{planned->start, planned->size};
        if (pending.contains(key) || chunk_windows.contains(key.start) ||
            chunk_windows.size() + pending.size() >= MaxWindows) {
            return false;
        }
        pending.insert(key);
        requests.push_back(key);
        if (!worker.joinable()) {
            worker = std::jthread([this](std::stop_token token) { ImportLoop(token); });
        }
        cv.notify_one();
        return false;
    }

    static bool Async() {
        return VideoCommon::Nx2GpuSide::On(VideoCommon::Nx2GpuSide::Opt::AsyncImport);
    }

    void ImportLoop(std::stop_token token) {
        Common::SetCurrentThreadName("VulkanImport");
        while (!token.stop_requested()) {
            Key key{};
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, token, [this] { return !requests.empty(); });
                if (token.stop_requested()) {
                    return;
                }
                key = requests.front();
                requests.pop_front();
            }
            Window window;
            if (Usable()) {
                if (Nx2Sync::HostImportChunks()) {
                    warmup.Run([&] {
                        window = Import(reinterpret_cast<const void*>(key.start), key.size);
                    });
                } else {
                    window = Import(reinterpret_cast<const void*>(key.start), key.size);
                }
            }
            {
                std::scoped_lock lock{mutex};
                if (Nx2Sync::HostImportChunks()) {
                    window.size = key.size;
                    chunk_windows.emplace(key.start, std::move(window));
                } else {
                    windows.emplace(key, std::move(window));
                }
                pending.erase(key);
            }
        }
    }

    Window Import(const void* start, size_t size) {
        const VkDevice logical = *device.GetLogical();
        const auto& dld = device.GetDispatchLoader();
        VkMemoryHostPointerPropertiesEXT host_props{
            .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
            .pNext = nullptr,
            .memoryTypeBits = 0,
        };
        if (dld.vkGetMemoryHostPointerPropertiesEXT(
                logical, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, start,
                &host_props) != VK_SUCCESS) {
            return Fail("memory host pointer properties");
        }
        const bool bda = device.IsBufferDeviceAddressSupported();
        const VkExternalMemoryBufferCreateInfo external_info{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        };
        const VkBufferCreateInfo buffer_ci{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = (Async() || Nx2Sync::HostImportChunks()) ? &external_info : nullptr,
            .flags = 0,
            .size = size,
            .usage =
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                (bda ? static_cast<VkBufferUsageFlags>(VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)
                     : VkBufferUsageFlags{0}),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        VkBuffer raw{};
        if (dld.vkCreateBuffer(logical, &buffer_ci, nullptr, &raw) != VK_SUCCESS) {
            return Fail("buffer");
        }
        Window window;
        window.buffer_handle = SparseBufferHandle{raw, logical, dld};
        const VkMemoryRequirements requirements = device.GetLogical().GetBufferMemoryRequirements(raw);
        if (Nx2Sync::HostImportChunks() && requirements.size > size) {
            return Fail("chunk smaller than buffer requirements");
        }
        const u32 bits = host_props.memoryTypeBits & requirements.memoryTypeBits;
        if (bits == 0) {
            return Fail("memory type");
        }
        const VkMemoryAllocateFlagsInfo flags_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .pNext = nullptr,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
            .deviceMask = 0,
        };
        const VkImportMemoryHostPointerInfoEXT import_info{
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
            .pNext = bda ? &flags_info : nullptr,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            .pHostPointer = const_cast<void*>(start),
        };
        const VkMemoryAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &import_info,
            .allocationSize = size,
            .memoryTypeIndex = static_cast<u32>(std::countr_zero(bits)),
        };
        window.memory = device.GetLogical().TryAllocateMemory(allocate_info);
        if (!window.memory) {
            return Fail("memory import");
        }
        if (dld.vkBindBufferMemory(logical, raw, *window.memory, 0) != VK_SUCCESS) {
            return Fail("bind");
        }
        window.buffer = raw;
        if (bda) {
            const VkBufferDeviceAddressInfo info{
                .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .pNext = nullptr,
                .buffer = raw,
            };
            window.address = dld.vkGetBufferDeviceAddress(logical, &info);
        }
        if (!reported.exchange(true, std::memory_order_relaxed)) {
            LOG_INFO(HW_GPU, "nx2: guest memory import active (first window {} KiB)", size >> 10);
        }
        return window;
    }

    Window Fail(const char* what) {
        LOG_WARNING(HW_GPU, "nx2: guest memory import failed ({}), copying instead", what);
        failed.store(true, std::memory_order_relaxed);
        return Window{};
    }

    const Device& device;
    Nx2Sync::ImportWarmup& warmup;
    uintptr_t page_mask{PageMask};
    std::atomic<bool> failed{};
    std::atomic<bool> reported{};
    mutable std::mutex mutex;
    std::condition_variable_any cv;
    std::unordered_map<Key, Window, KeyHash> windows;
    std::map<uintptr_t, Window> chunk_windows;
    std::unordered_set<Key, KeyHash> pending;
    std::deque<Key> requests;
    std::jthread worker;
};

} // namespace Vulkan
