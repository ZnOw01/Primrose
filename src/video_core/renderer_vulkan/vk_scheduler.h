// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <queue>
#include <span>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "common/progress_counter.h"
#include "common/settings.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2_gpu_side.h"
#include "video_core/renderer_vulkan/fragment_barrier_state.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCommon {
template <typename Trait>
class QueryCacheBase;
}

namespace Vulkan {

class CommandPool;
class Device;
class Framebuffer;
class GraphicsPipeline;
class StateTracker;
class UploadBatch;

struct QueryCacheParams;

/// The scheduler abstracts command buffer and fence management with an interface that's able to do
/// OpenGL-like operations on Vulkan command buffers.
class Scheduler {
public:

    explicit Scheduler(const Device& device, StateTracker& state_tracker);
    ~Scheduler();

    /// Sends the current execution context to the GPU.
    u64 Flush(VkSemaphore signal_semaphore = {}, VkSemaphore wait_semaphore = {});

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish(VkSemaphore signal_semaphore = {}, VkSemaphore wait_semaphore = {});

    /// On the split GPU thread, a fence or a command list copy submits only if the last
    /// submission is older than a window (EDEN_NX2_FENCE_FLUSH_US, default 500 us, 0 submits at
    /// once); otherwise it goes with the next one. The back submits deferred work before it
    /// sleeps and when another thread asks (WaitSubmitted). True when the flush was deferred.
    bool DeferFlush();
    /// Coalesce short list-copy bursts; demanded copies are submitted by WaitSubmitted.
    bool DeferListCopyFlush();
    /// Submits deferred work (the split's back, from its idle hook).
    void FlushDeferred();
    /// Threads other than the split's back: waits until tick is submitted, asking the back to
    /// submit deferred work (logged every 2 s while it waits). False only when the back thread
    /// is not running, so nothing will submit it: the caller then submits it itself.
    bool WaitSubmitted(u64 tick);
    /// Whether everything up to tick has been handed to the worker for submission.
    [[nodiscard]] bool IsSubmitted(u64 tick) const noexcept {
        return tick < master_semaphore->CurrentTick();
    }

    /// Nx2Perf submit_while_waited: FlushWork counts its draws; only a submission (Flush,
    /// Finish) resets the count, not a deferred FlushCommands.
    void NoteDraw() noexcept {
        ++draws_since_submit;
    }
    [[nodiscard]] u32 DrawsSinceSubmit() const noexcept {
        return draws_since_submit;
    }
    /// Whether the host GPU has finished everything submitted so far (reads the semaphore).
    [[nodiscard]] bool GpuCaughtUp() {
        master_semaphore->Refresh();
        return master_semaphore->KnownGpuTick() + 1 >= master_semaphore->CurrentTick();
    }

    /// Waits for the worker thread to finish executing everything. After this function returns it's
    /// safe to touch worker resources.
    void WaitWorker();

    /// Sends currently recorded work to the worker thread.
    void DispatchWork();

    /// Requests to begin a renderpass.
    void RequestRenderpass(const Framebuffer* framebuffer);

    /// Defers a full-attachment color clear so it becomes the next render pass.
    bool DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot, const VkClearValue& value);

    /// Defers a full depth/stencil clear so it becomes the next render pass.
    bool DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value);

    /// Realizes any pending deferred clear before its framebuffer can be moved or freed.
    void FlushDeferredClear();

    /// Requests the current execution context to be able to execute operations only allowed outside
    /// of a renderpass.
    void RequestOutsideRenderPassOperationContext() {
        if (!state.renderpass && deferred_clear.framebuffer == nullptr) {
            return;
        }
        EndRenderPass();
    }

    void DeferFragmentBarrier() {
        fragment_barrier.Defer(state.renderpass != VK_NULL_HANDLE);
    }

    bool HasFragmentBarrier() const {
        return fragment_barrier.Pending();
    }

    void NoteRenderPassStores() {
        render_pass_has_stores = true;
        storage_writes_unordered = true;
    }

    /// A draw wrote storage since the last memory barrier after all graphics stages: a host
    /// indirect draw reading GPU-written arguments needs one first (ReadyIndirectRecord).
    bool StorageWritesUnordered() const {
        return storage_writes_unordered;
    }

    void NoteStorageWritesOrdered() {
        storage_writes_unordered = false;
    }

    /// A draw or clear of the active render pass may write its depth/stencil attachment.
    void NoteDepthStencilWrite() {
        render_pass_depth_written = true;
    }

    /// Whether the active render pass may have written its depth/stencil attachment (a clear
    /// when it began, or a draw or clear since).
    [[nodiscard]] bool RenderPassWroteDepthStencil() const {
        return render_pass_depth_written;
    }

    bool RenderPassHasStores() const {
        return render_pass_has_stores;
    }

    /// A dispatch was recorded: the next render pass must see what it wrote.
    void NoteComputeWrites() {
        compute_writes_pending = true;
    }

    /// A guest wait-for-idle while the render pass is active: its barrier is recorded when the
    /// pass ends instead of ending it now. Draws of the pass are ordered with each other by
    /// rasterization order already; only work outside the pass needs the barrier.
    void DeferWaitForIdle(VkPipelineStageFlags stages) {
        deferred_wfi_stages |= stages;
    }

    /// Returns true when a render pass is currently active in the scheduler state.
    bool IsRenderPassActive() const {
        return state.renderpass != VK_NULL_HANDLE;
    }

    /// Update the pipeline to the current execution context.
    bool UpdateGraphicsPipeline(GraphicsPipeline* pipeline) {
        if (state.graphics_pipeline == pipeline && !state.needs_state_enable_refresh) [[likely]] {
            return false;
        }
        return UpdateGraphicsPipelineImpl(pipeline);
    }

    /// Worker thread only: whether the bound graphics pipeline failed to build, so the draws
    /// recorded after it must be dropped instead of reaching the driver without a pipeline.
    void SetGraphicsPipelineMissing(bool missing) {
        graphics_pipeline_missing = missing;
    }
    [[nodiscard]] bool GraphicsPipelineMissing() const {
        return graphics_pipeline_missing;
    }

    /// Update the rescaling state. Returns true if the state has to be updated.
    bool UpdateRescaling(bool is_rescaling) {
        if (state.rescaling_defined && is_rescaling == state.is_rescaling) [[likely]] {
            return false;
        }
        state.rescaling_defined = true;
        state.is_rescaling = is_rescaling;
        return true;
    }

    /// Returns true when the descriptor buffer chunk has to be bound into the command buffer.
    bool UpdateDescriptorBufferChunk(u32 descriptor_chunk) {
        if (state.descriptor_buffer_bound && descriptor_chunk == state.descriptor_buffer_chunk)
            [[likely]] {
            return false;
        }
        state.descriptor_buffer_bound = true;
        state.descriptor_buffer_chunk = descriptor_chunk;
        return true;
    }

    /// Marks the descriptor buffer as unbound (e.g. after vkCmdBindDescriptorSets on any bind
    /// point, which Vulkan specifies unbinds all descriptor buffers across all bind points).
    void InvalidateDescriptorBuffer() {
        state.descriptor_buffer_bound = false;
        state.rescaling_defined = false;
    }

    /// Invalidates current command buffer state except for render passes
    void InvalidateState();

    /// Assigns the query cache.
    void SetQueryCache(VideoCommon::QueryCacheBase<QueryCacheParams>& query_cache_) {
        query_cache = &query_cache_;
    }

    // Registers a callback to perform on queue submission.
    void RegisterOnSubmit(std::function<void()>&& func) {
        on_submit = std::move(func);
    }

    /// Consecutive in-order staging uploads share barriers until their destinations overlap,
    /// a source depends on a pending copy, or the batch is full. Call outside a render pass.
    void RecordUpload(VkBuffer src, VkBuffer dst, std::span<const VkBufferCopy> copies);

    /// Send work to a separate thread.
    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer, vk::CommandBuffer>
    void RecordWithUploadBuffer(T&& command) {
        commands_pending = true;
        if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
            VideoCommon::GpuSplit::CheckOwner("Scheduler::Record");
        }
        if (uploads_pending) {
            // Pending uploads precede every other recorded command, including upload-buffer work.
            FlushUploads();
        }
        if (chunk->Record(command)) {
            return;
        }
        if (pass_held && HoldFullPassChunk()) {
            (void)chunk->Record(command);
            return;
        }
        DispatchWork();
        (void)chunk->Record(command);
    }

    /// Whether a buffer upload can run before the active render pass (RecordBeforeRenderPass,
    /// Upload stage) instead of ending it. The caller checks that no draw of the pass used the
    /// destination (Buffer::IsRegionUsedInPass).
    [[nodiscard]] bool CanHoistUpload() const {
        return pass_held && state.renderpass != VK_NULL_HANDLE &&
               deferred_clear.framebuffer == nullptr;
    }

    /// Changes when a render pass ends: usage marked since then is usage by the current (or
    /// next) render pass, including the bindings of its first draw, made before it begins.
    [[nodiscard]] u64 PassEpoch() const noexcept {
        return pass_epoch;
    }

    /// Whether work recorded with RecordBeforeRenderPass runs before the render pass that is
    /// active on `framebuffer` (mesh emulation dispatches then do not end it).
    [[nodiscard]] bool CanRecordBeforeRenderPass(VkFramebuffer framebuffer) const {
        return pass_held && state.renderpass != VK_NULL_HANDLE &&
               state.framebuffer == framebuffer && !render_pass_has_stores &&
               deferred_clear.framebuffer == nullptr;
    }

    /// Stages of the work recorded before a render pass. The work of all a pass's emulated mesh
    /// draws runs stage by stage, with one barrier between stages (not 2-4 per draw).
    enum class PrePassStage : u32 {
        Upload = 0,       ///< buffer uploads no draw of the pass has used yet (upload-hoist)
        Task = 1,         ///< task program dispatches
        Distribution = 2, ///< task -> mesh distribution dispatches (read the task output)
        Mesh = 3,         ///< mesh program dispatches (read the distribution output)
    };

    /// Records work that runs before the active render pass begins (CanRecordBeforeRenderPass),
    /// in the given stage. Barriers: before the first stage (everything earlier written ->
    /// compute), between the stages (compute -> compute, indirect, host) and after the last
    /// (compute -> vertex input, vertex shader, indirect).
    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer>
    void RecordBeforeRenderPass(PrePassStage stage, T&& c) {
        auto command = [command = std::move(c)](vk::CommandBuffer cmdbuf, vk::CommandBuffer) {
            command(cmdbuf);
        };
        auto& chunks = pre_pass_chunks[static_cast<size_t>(stage)];
        if (chunks.empty() || !chunks.back()->Record(command)) {
            chunks.push_back(TakeReserveChunk());
            (void)chunks.back()->Record(command);
        }
    }

    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer>
    void Record(T&& c) {
        commands_pending = true;
        static constexpr bool direct_command = true;
        if (direct_command) {
            if (VideoCommon::GpuSplit::check_owner) [[unlikely]] {
                VideoCommon::GpuSplit::CheckOwner("Scheduler::Record");
            }
            if (uploads_pending) {
                FlushUploads();
            }
            if (chunk->Record<true>(c)) {
                return;
            }
            DispatchWork();
            (void)chunk->Record<true>(c);
            return;
        }
        this->RecordWithUploadBuffer(
            [command = std::move(c)](vk::CommandBuffer cmdbuf, vk::CommandBuffer) {
                command(cmdbuf);
            });
    }

    /// State commands do not access buffer contents; pending copies may follow them.
    template <typename T>
        requires std::is_invocable_v<T, vk::CommandBuffer>
    void RecordState(T&& command) {
        commands_pending = true;
        if (!VideoCommon::Nx2GpuSide::On(VideoCommon::Nx2GpuSide::Opt::CopyBatch)) {
            Record(std::forward<T>(command));
            return;
        }
        if (VideoCommon::GpuSplit::check_owner) {
            VideoCommon::GpuSplit::CheckOwner("Scheduler::RecordState");
        }
        if (!chunk->Record<true>(command)) {
            DispatchWork();
            (void)chunk->Record<true>(command);
        }
    }

    /// Whether the recording thread submitted `tick` within 20 ms.
    [[nodiscard]] bool WaitForSubmission(u64 tick) const {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{20};
        while (tick >= master_semaphore->CurrentTick()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
        return true;
    }

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore->CurrentTick();
    }

    /// Changes whenever recorded command buffer state may have been reset (InvalidateState, the
    /// end of a render pass), so a binding recorded at the same epoch is still bound.
    [[nodiscard]] u64 RecordingEpoch() const noexcept {
        return recording_epoch;
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return master_semaphore->IsFree(tick);
    }

    /// Waits for the given GPU tick, optionally pacing frames. Threads other than the one that
    /// records (the async present thread) pass may_flush = false: flushing from there records
    /// into the current chunk while that thread does (corrupt command buffers, crashes). They
    /// wait for the recording thread to submit the tick instead, and give up after 20 ms (it may
    /// be waiting for them).
    void Wait(u64 tick, double target_fps = 0.0, bool may_flush = true) {
        if (tick > 0 && (may_flush || WaitForSubmission(tick))) {
            if (tick >= master_semaphore->CurrentTick()) {
                Flush();
            }
            master_semaphore->Wait(tick);
        }
        if (Settings::values.use_speed_limit.GetValue() && target_fps > 0.0) {
            auto now = std::chrono::steady_clock::now();
            if (last_target_fps != target_fps) {
                frame_interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / target_fps));
                max_frame_count = static_cast<int>(0.1 * target_fps);
                last_target_fps = target_fps;
                frame_counter = 0;
                start_time = now;
            }
            frame_counter++;
            auto target_time = start_time + frame_interval * frame_counter;
            if (target_time >= now) {
                constexpr auto spin_tail = std::chrono::milliseconds(1);
                auto sleep_time = target_time - now;
                if (sleep_time > spin_tail * 2) {
                    std::this_thread::sleep_for(sleep_time - spin_tail);
                }
                while (std::chrono::steady_clock::now() < target_time) {
                    std::this_thread::yield();
                }
            } else if (frame_counter > max_frame_count) {
                frame_counter = 0;
                start_time = now;
            }
        }
    }

    /// Returns the master timeline semaphore.
    [[nodiscard]] MasterSemaphore& GetMasterSemaphore() const noexcept {
        return *master_semaphore;
    }

    std::mutex submit_mutex;

private:
    std::chrono::steady_clock::time_point last_flush{};
    bool flush_pending{};
    bool commands_pending{};
    /// The master semaphore's current tick as of the last submission (WaitSubmitted waits on it).
    Common::ProgressCounter submitted_ticks;

    class Command {
    public:
        virtual ~Command() = default;

        virtual void Execute(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) const = 0;

        Command* GetNext() const {
            return next;
        }

        void SetNext(Command* next_) {
            next = next_;
        }

    private:
        Command* next = nullptr;
    };

    template <typename T, bool SingleBuffer>
    class TypedCommand final : public Command {
    public:
        explicit TypedCommand(T&& command_) : command{std::move(command_)} {}
        ~TypedCommand() override = default;

        TypedCommand(TypedCommand&&) = delete;
        TypedCommand& operator=(TypedCommand&&) = delete;

        void Execute(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) const override {

            if constexpr (SingleBuffer) {
                command(cmdbuf);
            } else {
                command(cmdbuf, upload_cmdbuf);
            }
        }

    private:
        T command;
    };

    class CommandChunk final {
    public:
        void ExecuteAll(vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf);

        template <bool SingleBuffer = false, typename T>
        bool Record(T& command) {
            using FuncType = TypedCommand<T, SingleBuffer>;
            static_assert(sizeof(FuncType) < sizeof(data), "Lambda is too large");

            command_offset = Common::AlignUp(command_offset, alignof(FuncType));
            if (command_offset > sizeof(data) - sizeof(FuncType)) {
                return false;
            }
            Command* const current_last = last;
            last = new (data.data() + command_offset) FuncType(std::move(command));

            if (current_last) {
                current_last->SetNext(last);
            } else {
                first = last;
            }
            command_offset += sizeof(FuncType);
            return true;
        }

        void MarkSubmit() {
            submit = true;
        }

        bool Empty() const {
            return command_offset == 0;
        }

        bool HasSubmit() const {
            return submit;
        }

    private:
        Command* first = nullptr;
        Command* last = nullptr;

        size_t command_offset = 0;
        bool submit = false;
        alignas(std::max_align_t) std::array<u8, 0x8000> data{};
    };

    struct State {
        VkRenderPass renderpass{};
        VkFramebuffer framebuffer{};
        VkExtent2D render_area = {0, 0};
        GraphicsPipeline* graphics_pipeline = nullptr;
        bool is_rescaling = false;
        bool rescaling_defined = false;
        bool needs_state_enable_refresh = false;
        u32 descriptor_buffer_chunk = 0;
        bool descriptor_buffer_bound = false;
    };

    struct DeferredClear {
        const Framebuffer* framebuffer = nullptr;
        u32 color_clear_mask = 0;
        std::array<VkClearValue, 8> color_values{};
        bool depth_stencil = false;
        VkClearValue depth_stencil_value{};
    };

    /// Begins a render pass for the given framebuffer, optionally with clear values.
    void BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                             const VkClearValue* clear_values, u32 clear_value_count);

    bool UpdateGraphicsPipelineImpl(GraphicsPipeline* pipeline);

    /// If a deferred clear is pending.
    void RealizeDeferredClear();

    void WorkerThread(std::stop_token stop_token);

    void AllocateWorkerCommandBuffer();

    u64 SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore);

    void AllocateNewContext();

    void EndPendingOperations();

    void EndRenderPass();

    void AcquireNewChunk();

    /// A chunk from the reserve (or a new one).
    std::unique_ptr<CommandChunk> TakeReserveChunk();

    // Held render passes: the work before a render pass, the pass itself and the work recorded
    // before it (RecordBeforeRenderPass) go to the worker together when the pass ends, in the
    // order: work before the pass, work before the pass recorded during it, the pass. Mesh
    // emulation dispatches then run before the pass instead of ending it at every emulated draw
    // (Ocarina of Time: 150-300 of them a frame, each one a load and store of the 2560x1440
    // targets on an RX 580).
    bool pass_held{};
    std::vector<std::unique_ptr<CommandChunk>> before_pass_chunks;
    std::array<std::vector<std::unique_ptr<CommandChunk>>, 4> pre_pass_chunks;
    u64 pass_epoch{1};
    std::vector<std::unique_ptr<CommandChunk>> pass_chunks;
    /// Holds the render pass about to begin (BeginRenderPassImpl).
    void HoldPass();
    /// The current chunk is full while a pass is held: keep it in the pass. False (and the pass
    /// is released) when the pass has grown too large to keep holding.
    bool HoldFullPassChunk();
    /// Sends the held work to the worker in order; the rest of the pass is not held.
    void ReleasePass();

    const Device& device;
    StateTracker& state_tracker;

    std::unique_ptr<MasterSemaphore> master_semaphore;

private:
    FragmentBarrierState fragment_barrier;
    bool render_pass_has_stores{};
    bool storage_writes_unordered{};
    bool render_pass_depth_written{};
    VkPipelineStageFlags deferred_wfi_stages{}; ///< DeferWaitForIdle: barrier due at the pass end
    bool compute_writes_pending{};
    bool graphics_pipeline_missing{};
    std::unique_ptr<CommandPool> command_pool;
    std::unique_ptr<UploadBatch> upload_batch;
    bool uploads_pending{};
    void FlushUploads();

    VideoCommon::QueryCacheBase<QueryCacheParams>* query_cache = nullptr;

    vk::CommandBuffer current_cmdbuf;
    vk::CommandBuffer current_upload_cmdbuf;

    DeferredClear deferred_clear;

    std::unique_ptr<CommandChunk> chunk;
    std::function<void()> on_submit;

    State state;
    u64 recording_epoch = 0;
    u32 draws_since_submit = 0;

    u32 num_renderpass_images = 0;
    std::array<VkImage, 9> renderpass_images{};
    std::array<VkImageSubresourceRange, 9> renderpass_image_ranges{};

    std::queue<std::unique_ptr<CommandChunk>> work_queue;
    std::vector<std::unique_ptr<CommandChunk>> chunk_reserve;
    std::mutex execution_mutex;
    std::mutex reserve_mutex;
    std::mutex queue_mutex;
    std::condition_variable_any event_cv;
    bool worker_waiting{}; ///< The worker is waiting for work (guarded by queue_mutex)
    std::jthread worker_thread;

    std::chrono::steady_clock::duration frame_interval{};
    std::chrono::steady_clock::time_point start_time{};
    double last_target_fps{};
    u64 max_frame_count{};
    u64 frame_counter{};
};

} // namespace Vulkan
