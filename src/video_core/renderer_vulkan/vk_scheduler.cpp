// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "video_core/renderer_vulkan/vk_query_cache.h"

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
#include "video_core/renderer_vulkan/upload_batch.h"
#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(cmdbuf, upload_cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Device& device_, StateTracker& state_tracker_)
    : device{device_}, state_tracker{state_tracker_},
      master_semaphore{std::make_unique<MasterSemaphore>(device)},
      command_pool{std::make_unique<CommandPool>(*master_semaphore, device)},
      upload_batch{std::make_unique<UploadBatch>()} {

    AcquireNewChunk();
    AllocateWorkerCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
}

Scheduler::~Scheduler() = default;

u64 Scheduler::Flush(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Flush");
    }
    VideoCommon::Nx2Profile::AddFlush();
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);
    AllocateNewContext();
    last_flush = std::chrono::steady_clock::now();
    flush_pending = false;
    return signal_value;
}

bool Scheduler::DeferFlush() {
    // Fences come one per compute release: ~11,500 a second in Hyrule Warriors: Age of
    // Imprisonment fights, each a queue submission (~22,000 a second with the others), which
    // kept the Vulkan worker busy submitting. Batched within 500 us: ~10-15% more fps there.
    // Command list copies after dispatches were ~9,000 more a second, of which the front waited
    // for about 5%.
    static const s64 interval_us = [] {
        const char* value = std::getenv("EDEN_NX2_FENCE_FLUSH_US");
        if (value != nullptr) {
            return static_cast<s64>(std::atoll(value));
        }
        return VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::FenceFlush) ? s64{5000} : s64{500};
    }();
    if (interval_us <= 0 || !VideoCommon::GpuSplit::OnBackThread() ||
        std::chrono::steady_clock::now() - last_flush >= std::chrono::microseconds{interval_us}) {
        return false;
    }
    flush_pending = true;
    return true;
}

bool Scheduler::DeferListCopyFlush() {
    if (!Nx2Sync::MergeWaits()) {
        return !VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::FenceFlush) && DeferFlush();
    }
    if (!VideoCommon::GpuSplit::OnBackThread() ||
        VideoCommon::GpuSplit::Get().FrontWaitsForBack() ||
        std::chrono::steady_clock::now() - last_flush >= std::chrono::microseconds{100}) {
        return false;
    }
    flush_pending = true;
    return true;
}

void Scheduler::FlushDeferred() {
    if (flush_pending) {
        Flush();
    }
}

bool Scheduler::WaitSubmitted(u64 tick) {
    if (IsSubmitted(tick)) {
        return true;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    split.RequestIdleHook();
    // No time limit: the back can spend seconds on one entry (a large shader's translation),
    // and a fence reported as submitted before it is would release its downloads too early.
    // Woken by SubmitExecution (it was a 50 us sleep poll, ~1 ms per check on Windows).
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
        submitted_ticks.Wait(
            tick + 1, std::chrono::microseconds{5}, deadline,
            [&] { return IsSubmitted(tick) || !split.BackRunning(); }, std::chrono::milliseconds{1});
        if (IsSubmitted(tick)) {
            return true;
        }
        if (!split.BackRunning()) {
            return IsSubmitted(tick);
        }
        LOG_WARNING(Render_Vulkan, "Waited {} s for the GPU thread to submit tick {}",
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - start)
                        .count(),
                    tick);
        split.RequestIdleHook();
    }
}

void Scheduler::Finish(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::Finish");
    }
    if (Nx2Sync::MergeWaits()) {
        if (uploads_pending) {
            FlushUploads();
        }
        EndPendingOperations();
        if (!commands_pending && !signal_semaphore && !wait_semaphore) {
            master_semaphore->Wait(CurrentTick() - 1);
            return;
        }
    }
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal_semaphore, wait_semaphore);
    Wait(presubmit_tick);
    AllocateNewContext();
}

void Scheduler::WaitWorker() {
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
}

void Scheduler::DispatchWork() {
    if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::DispatchWork");
    }
    if (uploads_pending) {
        // The worker must receive everything recorded so far.
        FlushUploads();
    }
    if (pass_held) {
        ReleasePass();
    }
    if (chunk && !chunk->Empty()) {
        // Waking the worker is a system call (~2% of the GPU thread at one dispatch per 8
        // draws): skip it while the worker is still busy, it pops the queue before waiting.
        bool wake;
        {
            std::scoped_lock ql{queue_mutex};
            work_queue.push(std::move(chunk));
            wake = worker_waiting;
        }
        if (wake) {
            event_cv.notify_all();
        }
        AcquireNewChunk();
    }
}

void Scheduler::RecordUpload(VkBuffer src, VkBuffer dst, std::span<const VkBufferCopy> copies) {
    if (VideoCommon::GpuSplit::check_owner) {
        VideoCommon::GpuSplit::CheckOwner("Scheduler::RecordUpload");
    }
    for (const VkBufferCopy& copy : copies) {
        const std::span<const VkBufferCopy> region{&copy, 1};
        if (uploads_pending && !upload_batch->CanAdd(src, dst, region)) {
            FlushUploads();
        }
        upload_batch->Add(src, dst, region);
        uploads_pending = true;
    }
}

void Scheduler::FlushUploads() {
    auto command = [copies = upload_batch->copies, groups = upload_batch->groups](
                       vk::CommandBuffer cmdbuf) { UploadBatch::Record(cmdbuf, copies, groups); };
    upload_batch->copies.clear();
    upload_batch->groups.clear();
    uploads_pending = false;
    Record(std::move(command));
}

void Scheduler::BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                                    const VkClearValue* clear_values, u32 clear_value_count) {
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    state.renderpass = framebuffer->RenderPass();
    state.framebuffer = framebuffer_handle;
    state.render_area = render_area;

    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string render_pass_info =
            fmt::format("renderArea={}x{}, numImages={}", render_area.width, render_area.height,
                        framebuffer->NumImages());
        GPU::Logging::GPULogger::GetInstance().LogRenderPassBegin(render_pass_info);
    }

    std::array<VkClearValue, 9> values{};
    for (u32 i = 0; i < clear_value_count && i < values.size(); ++i) {
        values[i] = clear_values[i];
    }
    const bool after_compute = std::exchange(compute_writes_pending, false);
    // A pass that begins with a clear writes its attachments before any draw.
    render_pass_depth_written = clear_value_count != 0;
    HoldPass();
    Record([renderpass, framebuffer_handle, render_area, values, clear_value_count,
            after_compute](vk::CommandBuffer cmdbuf) {
        const VkRenderPassBeginInfo renderpass_bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = renderpass,
            .framebuffer = framebuffer_handle,
            .renderArea =
                {
                    .offset = {.x = 0, .y = 0},
                    .extent = render_area,
                },
            .clearValueCount = clear_value_count,
            .pClearValues = clear_value_count != 0 ? values.data() : nullptr,
        };
        if (after_compute) {
            // Draws read compute output as indirect arguments, vertices and resources; nothing
            // else orders those reads after the dispatch.
            static constexpr VkMemoryBarrier COMPUTE_TO_GRAPHICS{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, COMPUTE_TO_GRAPHICS);
        }
        cmdbuf.BeginRenderPass(renderpass_bi, VK_SUBPASS_CONTENTS_INLINE);
    });
    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
    framebuffer->MarkResolveShadowsUpToDate();
}

void Scheduler::RealizeDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    const DeferredClear dc = deferred_clear;
    deferred_clear = {};

    std::array<VkClearValue, 9> clear_values{};
    u32 count = 0;
    const RenderPassKey& base = dc.framebuffer->RenderPassKeyBase();
    for (u32 slot = 0; slot < 8; ++slot) {
        if (base.color_formats[slot] == VideoCore::Surface::PixelFormat::Invalid) {
            continue;
        }
        clear_values[count++] = dc.color_values[slot];
    }
    if (base.depth_format != VideoCore::Surface::PixelFormat::Invalid) {
        clear_values[count++] = dc.depth_stencil_value;
    }
    const u32 color_discard_mask =
        dc.framebuffer->DiscardsMsaaColor() ? dc.color_clear_mask : 0u;
    const bool depth_stencil_discard =
        dc.depth_stencil && dc.framebuffer->DiscardsMsaaDepthStencil();
    const VkRenderPass renderpass = dc.framebuffer->RenderPassVariant(
        dc.color_clear_mask, dc.depth_stencil, color_discard_mask, depth_stencil_discard);
    EndRenderPass();
    BeginRenderPassImpl(dc.framebuffer, renderpass, clear_values.data(), count);
}

bool Scheduler::DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot,
                                const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.color_clear_mask |= 1u << rt_slot;
    deferred_clear.color_values[rt_slot] = value;
    return true;
}

bool Scheduler::DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.depth_stencil = true;
    deferred_clear.depth_stencil_value = value;
    return true;
}

void Scheduler::FlushDeferredClear() {
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    RealizeDeferredClear();
    EndRenderPass();
}

void Scheduler::RequestRenderpass(const Framebuffer* framebuffer) {
    if (deferred_clear.framebuffer == framebuffer) {
        RealizeDeferredClear();
        return;
    }
    const VkRenderPass renderpass = framebuffer->RenderPass();
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    if (renderpass == state.renderpass && framebuffer_handle == state.framebuffer &&
        render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height) {
        return;
    }
    // Ends any active pass and realizes a deferred clear
    EndRenderPass();
    BeginRenderPassImpl(framebuffer, renderpass, nullptr, 0);
}

bool Scheduler::UpdateGraphicsPipelineImpl(GraphicsPipeline* pipeline) {
    if (state.graphics_pipeline == pipeline) {
        if (pipeline && pipeline->UsesExtendedDynamicState() &&
            state.needs_state_enable_refresh) {
            state_tracker.InvalidateStateEnableFlag();
            state.needs_state_enable_refresh = false;
        }
        return false;
    }

    state.graphics_pipeline = pipeline;

    if (!pipeline) {
        return true;
    }

    if (!pipeline->UsesExtendedDynamicState()) {
        state.needs_state_enable_refresh = true;
    } else if (state.needs_state_enable_refresh) {
        state_tracker.InvalidateStateEnableFlag();
        state.needs_state_enable_refresh = false;
    }

    return true;
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }

        work = std::move(work_queue.front());
        work_queue.pop();
        event_cv.notify_all();
        return true;
    }};

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        {
            std::unique_lock lk{queue_mutex};

            // Wait for work.
            event_cv.wait(lk, stop_token, [&] {
                worker_waiting = !TryPopQueue(work);
                return !worker_waiting;
            });
            worker_waiting = false;

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            void(std::exchange(lk, std::unique_lock{execution_mutex}));

            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
        }

        {
            std::scoped_lock rl{reserve_mutex};

            // Recycle the chunk back to the reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
    }
}

void Scheduler::AllocateWorkerCommandBuffer() {
    current_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_upload_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
}

u64 Scheduler::SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    if (Nx2Sync::HostImportChunks()) {
        // One startup barrier, before any queue submission can race a host import.
        master_semaphore->HostImportWarmup().Close();
    }
    if (uploads_pending) {
        FlushUploads();
    }
    EndPendingOperations();
    InvalidateState();
    draws_since_submit = 0;

    const u64 signal_value = master_semaphore->NextTick();
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        static constexpr VkMemoryBarrier READ_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        };
        upload_cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
        upload_cmdbuf.End();
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, READ_BARRIER);
        cmdbuf.End();

        if (on_submit) {
            on_submit();
        }

        std::scoped_lock lock{submit_mutex};
        switch (const VkResult result = master_semaphore->SubmitQueue(
                    cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, signal_value)) {
        case VK_SUCCESS:
            // Log successful queue submission
            if (GPU::Logging::IsActive() &&
                Settings::values.gpu_log_vulkan_calls.GetValue()) {
                GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                    "vkQueueSubmit", "", VK_SUCCESS);
            }
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
    });
    commands_pending = false;
    chunk->MarkSubmit();
    DispatchWork();
    // Handed to the worker: wakes threads in WaitSubmitted.
    submitted_ticks.AdvanceAtLeast(master_semaphore->CurrentTick());
    return signal_value;
}

void Scheduler::AllocateNewContext() {
    // Enable counters once again. These are disabled when a command buffer is finished.
}

void Scheduler::InvalidateState() {
    ++recording_epoch;
    state.graphics_pipeline = nullptr;
    state.rescaling_defined = false;
    state.descriptor_buffer_bound = false;
    state_tracker.InvalidateCommandBufferState();
}

void Scheduler::EndPendingOperations() {
    // Pause, don't reset: a submission can land between a guest's counter reset and its report,
    // and resetting here would drop the samples counted so far (the report then reads too low).
    query_cache->CounterClose(VideoCommon::QueryType::ZPassPixelCount64);
    EndRenderPass();
}

void Scheduler::EndRenderPass()
    {
        RealizeDeferredClear();
        if (!state.renderpass) {
            return;
        }
        ++recording_epoch;
        fragment_barrier.Reset();
        render_pass_has_stores = false;
        render_pass_depth_written = false;

        query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);

        // Log render pass end
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogRenderPassEnd();
        }

        query_cache->CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);
        query_cache->NotifySegment(false);

        Record([num_images = num_renderpass_images,
                       images = renderpass_images,
                       ranges = renderpass_image_ranges,
                       has_transform_feedback = device.IsExtTransformFeedbackSupported()](
                          vk::CommandBuffer cmdbuf) {
            std::array<VkImageMemoryBarrier, 9> barriers;
            for (size_t i = 0; i < num_images; ++i) {
                const VkImageSubresourceRange& range = ranges[i];
                const bool is_color = (range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
                const bool is_depth_stencil = (range.aspectMask
                                              & (VK_IMAGE_ASPECT_DEPTH_BIT
                                                 | VK_IMAGE_ASPECT_STENCIL_BIT)) !=0;

                VkAccessFlags src_access = 0;

                if (is_color)
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                else if (is_depth_stencil)
                    src_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                else
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                  | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

                barriers[i] = VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = src_access,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = images[i],
                        .subresourceRange = range,
                };
            }
            cmdbuf.EndRenderPass();
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                   0, nullptr, nullptr, vk::Span(barriers.data(), num_images));
            if (has_transform_feedback) {
                static constexpr VkMemoryBarrier XFB_OUTPUT_BARRIER{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                    .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, XFB_OUTPUT_BARRIER);
            }
        });

        if (const VkPipelineStageFlags stages = std::exchange(deferred_wfi_stages, 0)) {
            // The guest's wait-for-idle (RasterizerVulkan::WaitForIdle) from inside the pass.
            Record([stages](vk::CommandBuffer cmdbuf) {
                static constexpr VkMemoryBarrier WFI_BARRIER{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                };
                cmdbuf.PipelineBarrier(stages, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WFI_BARRIER);
            });
            storage_writes_unordered = false;
        }
        state.renderpass = VkRenderPass{};
        num_renderpass_images = 0;
        ++pass_epoch;
        if (pass_held) {
            ReleasePass();
        }
    }

void Scheduler::AcquireNewChunk() {
    chunk = TakeReserveChunk();
}

std::unique_ptr<Scheduler::CommandChunk> Scheduler::TakeReserveChunk() {
    std::scoped_lock rl{reserve_mutex};

    if (chunk_reserve.empty()) {
        // If we don't have anything reserved, we need to make a new chunk.
        return std::make_unique<CommandChunk>();
    }
    // Otherwise, we can just take from the reserve.
    std::unique_ptr<CommandChunk> reserved = std::move(chunk_reserve.back());
    chunk_reserve.pop_back();
    return reserved;
}

namespace {
bool PassHoldEnabled() {
    // EDEN_NX2_MESH_HOIST=0: end the render pass at every emulated mesh draw, as before.
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_MESH_HOIST");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}
} // namespace

void Scheduler::HoldPass() {
    // Only without VK_EXT_mesh_shader: mesh draws are emulated with compute dispatches then.
    if (!PassHoldEnabled() || device.IsExtMeshShaderSupported()) {
        return;
    }
    if (pass_held) {
        ReleasePass();
    }
    if (!chunk->Empty()) {
        before_pass_chunks.push_back(std::move(chunk));
        AcquireNewChunk();
    }
    pass_held = true;
}

bool Scheduler::HoldFullPassChunk() {
    // About 256 KiB of commands: past that the worker would sit idle too long behind the pass.
    static constexpr size_t MaxHeldPassChunks = 8;
    if (pass_chunks.size() >= MaxHeldPassChunks) {
        ReleasePass();
        return false;
    }
    pass_chunks.push_back(std::move(chunk));
    AcquireNewChunk();
    return true;
}

void Scheduler::ReleasePass() {
    pass_held = false;
    if (!chunk->Empty()) {
        pass_chunks.push_back(std::move(chunk));
    }
    // The barriers around and between the stages of the work recorded before the pass.
    const auto barrier_chunk = [this](VkAccessFlags src_access, VkAccessFlags dst_access,
                                      VkPipelineStageFlags src_stages,
                                      VkPipelineStageFlags dst_stages) {
        std::unique_ptr<CommandChunk> barrier = TakeReserveChunk();
        auto command = [src_access, dst_access, src_stages, dst_stages](vk::CommandBuffer cmdbuf,
                                                                        vk::CommandBuffer) {
            const VkMemoryBarrier memory_barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = src_access,
                .dstAccessMask = dst_access,
            };
            cmdbuf.PipelineBarrier(src_stages, dst_stages, 0, memory_barrier);
        };
        (void)barrier->Record(command);
        return barrier;
    };
    std::vector<std::unique_ptr<CommandChunk>> pre_pass;
    bool any_stage = false;
    bool any_compute = false;
    constexpr size_t upload_stage = static_cast<size_t>(PrePassStage::Upload);
    if (!pre_pass_chunks[upload_stage].empty()) {
        // Uploads: after what earlier passes, dispatches and copies read and wrote.
        pre_pass.push_back(barrier_chunk(
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT));
        for (auto& held : pre_pass_chunks[upload_stage]) {
            pre_pass.push_back(std::move(held));
        }
        pre_pass_chunks[upload_stage].clear();
        any_stage = true;
    }
    for (size_t stage = upload_stage + 1; stage < pre_pass_chunks.size(); ++stage) {
        auto& chunks = pre_pass_chunks[stage];
        if (chunks.empty()) {
            continue;
        }
        if (!any_compute) {
            // The programs read what earlier passes, dispatches and copies wrote (foliage
            // instance lists, for one).
            pre_pass.push_back(barrier_chunk(
                VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT));
        } else {
            // The distribution reads the task output and writes the mesh dispatch and draw
            // arguments (and the arena counter the host reads after the submission).
            pre_pass.push_back(barrier_chunk(
                VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                    VK_ACCESS_HOST_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                    VK_PIPELINE_STAGE_HOST_BIT));
        }
        any_stage = true;
        any_compute = true;
        for (auto& held : chunks) {
            pre_pass.push_back(std::move(held));
        }
        chunks.clear();
    }
    if (any_stage) {
        // The pass's draws read the mesh output in their vertex shaders and their vertex counts
        // as indirect arguments, and the uploads as vertices, indices, uniforms and storage in
        // any stage.
        pre_pass.push_back(barrier_chunk(
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            vk::PIPELINE_STAGE_GRAPHICS_COMPUTE));
    }
    bool wake;
    {
        std::scoped_lock ql{queue_mutex};
        for (auto* list : {&before_pass_chunks, &pre_pass, &pass_chunks}) {
            for (auto& held : *list) {
                work_queue.push(std::move(held));
            }
            list->clear();
        }
        wake = worker_waiting;
    }
    if (wake) {
        event_cv.notify_all();
    }
    if (!chunk) {
        AcquireNewChunk();
    }
}

} // namespace Vulkan
