// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/ranges.h>

#include "audio_core/audio_core.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/service/audio/errors.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/service.h"
#include "core/memory.h"

namespace Service::Audio {

// Switch 2 sound mixer streams (nn::audio::sysmix, behind audsmx:u). A stream is a ring buffer in
// the game's own memory: CreateStream hands over its address and size (4 KiB aligned), OpenStream
// its format and length in frames, and once the streams are engaged the system moves frames at the
// stream's sample rate. Kind 0 is an output: the game writes the ring and the system takes frames
// out of it into the output sink. Kind 1 is an input: the system writes frames into the ring and the
// game reads them, which is how a title hears the host microphone ("InputStream-1<=1ch48kQ15<
// =TrueRawAudioIn") and, through "AudioVirtualOutput0", its own rendered mix.
// The ring's shared indices, in frames modulo (frame count + 1): the consumer's read index at
// buffer + 0x44, the producer's write index at buffer + 0x1004.
struct Nx2SysmixStream {
    u32 id{};
    u32 owner{}; ///< the user session whose timer drains it
    u32 kind{};  ///< 0 output, 1 input
    u64 buffer{};
    u64 size{};
    u32 sample_type{};
    u32 channels{};
    u32 sample_rate{};
    u32 frames{};
    bool open{};
    bool engaged{};
    double owed_frames{};
    u32 data_mode{};      ///< command 7's mode; 0 leaves the data event alone
    s32 data_threshold{}; ///< free frames (output), filled frames (input) needed for the data event
    Kernel::KEvent* state_event{};
    Kernel::KEvent* data_event{};
    AudioCore::Sink::SinkStream* sink{}; ///< output sink for kind 0, input sink for kind 1
    u64 data_offset{0x1040};
    f32 gain{1.0f};
    bool reported_samples{};
    std::string source; ///< kind 1: the device the stream carries, from its '<=<device>' suffix
    double report_seconds{}; ///< time since the last input-ring report
    s32 report_peak{};       ///< loudest frame written since then
};

/// EDEN_NX2_MIC_LOOPBACK_OUTPUT: feed the virtual-output device's capture stream from the game's
/// own rendered mix instead of the microphone. By default that stream carries the microphone: it is
/// the console's default capture device, and titles listen there (Ocarina of Time reads its
/// "AudioVirtualOutput0" input stream, not only "TrueRawAudioIn", before it hears anything).
inline bool LoopbackUsesOutputMix() {
    static const bool on = std::getenv("EDEN_NX2_MIC_LOOPBACK_OUTPUT") != nullptr;
    return on;
}

/// EDEN_NX2_AUDIO_IN_LEVEL: log what each input stream carries about once a second.
inline bool LogAudioInput() {
    static const bool on = std::getenv("EDEN_NX2_AUDIO_IN_LEVEL") != nullptr;
    return on;
}

/// The device an input stream carries when it is the microphone rather than a virtual-output loop.
constexpr std::string_view RawInputDevice = "TrueRawAudioIn";

/// The sink a stream records from (kind 1) or plays to (kind 0).
inline AudioCore::Sink::Sink& SinkFor(Core::System& system, const Nx2SysmixStream& stream) {
    return stream.kind ? system.AudioCore().GetInputSink() : system.AudioCore().GetOutputSink();
}

struct Nx2SysmixStreams {
    std::mutex mutex;
    std::map<u32, std::shared_ptr<Nx2SysmixStream>> streams;
    u32 next_id{1};
    /// Rendered output not yet read by a loopback input stream: what an "AudioVirtualOutput0" input
    /// hears is the game's own mix, never the microphone.
    std::deque<s16> loopback;
    /// Microphone frames just written, handed to the virtual-output device's capture streams.
    std::deque<s16> mic_tail;
};

constexpr std::size_t MaxLoopbackFrames = 48'000;

// Streams are created through audsmx:u but started and stopped through auddmg:u (AttachStream and
// DetachStream), so both services share one list.
inline std::shared_ptr<Nx2SysmixStreams> Nx2SysmixStreamList() {
    static const auto list = std::make_shared<Nx2SysmixStreams>();
    return list;
}

namespace Devmgr {

// nn::audio::devmgr::IUserSession: Switch 2 virtual output devices
// (nn::audio::ListVirtualOutputDevices and friends), reached through the object auddmg:u command 0
// returns. Eden plays everything through one stereo 48 kHz output, so there is one virtual device.
// The nx2: name keeps the other commands auto-stubbed and logged.
// - 0(u32): u32 count, the device names in the out buffer (0x100 bytes each).
// - 100(u32): the default device's name in the out buffer.
// - 300(16-byte stream format, name buffer): whether the format is supported (result only).
// - 401(u32, name buffer): the device's 0x228-byte property block in the out buffer. The sdk hands
//   the game the name, the channel count (0x108) and the u64 at 0x218; with the zeroed placeholder
//   reply it saw 0 channels and Switch Sports failed to set up audio (2153-1604).
// - 500(16 bytes, name buffer): an event signalled when the device's state changes.
// - 700(u32 stream id, u32 device functionality, name buffer): attach a sysmix stream to the
//   device, which starts it (nn::audio::devmgr::AttachStream, behind OutputStream Start).
// - 800(u32 stream id): detach it again (Stop).
class IUserSession final : public ServiceFramework<IUserSession> {
public:
    explicit IUserSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:auddmg:u:manager"},
          service_context{system_, "nx2:auddmg:u:manager"},
          state_event{service_context.CreateEvent("nx2:auddmg:u:StateEvent")} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IUserSession::ListVirtualOutputDevices, "ListVirtualOutputDevices"},
            {100, &IUserSession::GetDefaultVirtualOutputDevice, "GetDefaultVirtualOutputDevice"},
            {300, &IUserSession::IsStreamFormatSupported, "IsVirtualOutputDeviceStreamFormatSupported"},
            {401, &IUserSession::GetVirtualOutputDeviceProperties, "GetVirtualOutputDeviceProperties"},
            {500, &IUserSession::AcquireStateChangeEvent, "AcquireVirtualOutputDeviceStateEvent (guessed)"},
            {700, &IUserSession::AttachStream, "AttachStream"},
            {800, &IUserSession::DetachStream, "DetachStream"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

    ~IUserSession() override {
        service_context.CloseEvent(state_event);
    }

private:
    static constexpr std::size_t NameSize = 0x100;
    static constexpr std::string_view DeviceName = "AudioVirtualOutput0";
    static constexpr std::string_view PhysicalDeviceName = "AudioTvOutput";

    static void CopyName(u8* out, std::string_view name) {
        std::memcpy(out, name.data(), std::min(name.size(), NameSize - 1));
    }

    void ListVirtualOutputDevices(HLERequestContext& ctx) {
        std::array<u8, NameSize> names{};
        CopyName(names.data(), DeviceName);
        const u32 count = ctx.GetWriteBufferSize() >= NameSize ? 1 : 0;
        if (count) {
            ctx.WriteBuffer(names);
        }
        LOG_DEBUG(Service_Audio, "called, {} device", count);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(count);
    }

    void GetDefaultVirtualOutputDevice(HLERequestContext& ctx) {
        std::array<u8, NameSize> name{};
        CopyName(name.data(), DeviceName);
        ctx.WriteBuffer(name);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void IsStreamFormatSupported(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Audio, "called");
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    // Property block: name (0x000), u32 type (0x100, 3), u32 (0x104, 2), u32 channel count
    // (0x108), u32 sample rate (0x10c), physical output name (0x110), its id (0x210), u64 latency
    // in ns (0x218), u32 device id (0x220). The sdk's output streams pass the device id to
    // sysmix CreateStream, which refuses 0 (Switch Sports failed with 2153-1604 on a zero there).
    void GetVirtualOutputDeviceProperties(HLERequestContext& ctx) {
        std::array<u8, 0x228> block{};
        const auto name = ctx.ReadBuffer(0);
        std::memcpy(block.data(), name.data(), std::min(name.size(), NameSize - 1));
        const auto put32 = [&](std::size_t offset, u32 value) {
            std::memcpy(block.data() + offset, &value, sizeof(value));
        };
        put32(0x100, 3);
        put32(0x104, 2);
        put32(0x108, 2);     // stereo
        put32(0x10c, 48000); // Hz
        CopyName(block.data() + 0x110, PhysicalDeviceName);
        const u64 latency_ns = 10'000'000;
        std::memcpy(block.data() + 0x218, &latency_ns, sizeof(latency_ns));
        put32(0x220, 1);
        ctx.WriteBuffer(block);
        LOG_DEBUG(Service_Audio, "called for {}", Common::StringFromBuffer(name));
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void AcquireStateChangeEvent(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, state_event->GetReadableEvent());
    }

    void AttachStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto stream_id = rp.Pop<u32>();
        const auto functionality = rp.Pop<u32>();
        LOG_INFO(Service_Audio, "attach stream {} to {} (functionality {})", stream_id,
                 Common::StringFromBuffer(ctx.ReadBuffer(0)), functionality);
        SetStreamEngaged(stream_id, true);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void DetachStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto stream_id = rp.Pop<u32>();
        LOG_INFO(Service_Audio, "detach stream {}", stream_id);
        SetStreamEngaged(stream_id, false);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void SetStreamEngaged(u32 stream_id, bool engaged) {
        const auto list = Nx2SysmixStreamList();
        std::scoped_lock lk{list->mutex};
        if (const auto it = list->streams.find(stream_id); it != list->streams.end()) {
            it->second->engaged = engaged;
            it->second->owed_frames = 0;
            if (it->second->state_event) {
                it->second->state_event->Signal(system.Kernel());
            }
        }
    }

    KernelHelpers::ServiceContext service_context;
    Kernel::KEvent* state_event;
};

// Switch 2 "auddmg:u" (nn::audio::devmgr::IUserSessionManager): command 0 (16 bytes and the pid)
// returns the user session.
class IUserSessionManager final : public ServiceFramework<IUserSessionManager> {
public:
    explicit IUserSessionManager(Core::System& system_)
        : ServiceFramework{system_, "nx2:auddmg:u"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IUserSessionManager::OpenUserSession, "OpenUserSession"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void OpenUserSession(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Audio, "called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IUserSession>(ctx, system);
    }
};

} // namespace Devmgr

namespace Sysmix {

// nn::audio::sysmix::IExternalStreamSession, the stream object IUserSession command 1 returns.
// Commands (ids, request -> reply):
// - 0(u32 sample type, u32 channels, u32 sample rate, u32 frames, u32 prefill, u32 flags): open.
// - 1: close. 2: u32 state. 3: f32 gain. 4(f32 gain, s64 ramp ns): set the gain.
// - 5 / 6: acquire (copy handle) / release the state-change event.
// - 7(u32 mode, s32 threshold) / 8: acquire (copy handle) / release the data event.
// - 9(s64 ns): clock the stream. 10(s32 frames) -> u32: prime. 11(s32 frames) -> u32: purge.
// - 12(name buffer): rename.
class IExternalStreamSession final : public ServiceFramework<IExternalStreamSession> {
public:
    explicit IExternalStreamSession(Core::System& system_,
                                     std::shared_ptr<Nx2SysmixStreams> streams_,
                                     std::shared_ptr<Nx2SysmixStream> stream_)
        : ServiceFramework{system_, "nx2:audsmx:u:stream"},
          service_context{system_, "nx2:audsmx:u:stream"}, streams{std::move(streams_)},
          stream{std::move(stream_)} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IExternalStreamSession::OpenStream, "OpenStream"},
            {1, &IExternalStreamSession::CloseStream, "CloseStream"},
            {2, &IExternalStreamSession::GetStreamState, "GetStreamState"},
            {3, &IExternalStreamSession::GetStreamGain, "GetStreamGain"},
            {4, &IExternalStreamSession::SetStreamGain, "SetStreamGain"},
            {5, &IExternalStreamSession::AcquireStateChangeEvent, "AcquireStreamStateChangeEvent"},
            {6, &IExternalStreamSession::Succeed, "ReleaseStreamStateChangeEvent"},
            {7, &IExternalStreamSession::AcquireDataEvent, "AcquireStreamDataEvent"},
            {8, &IExternalStreamSession::Succeed, "ReleaseStreamDataEvent"},
            {9, &IExternalStreamSession::ClockStream, "ClockStream"},
            {10, &IExternalStreamSession::EchoFrames, "PrimeStream"},
            {11, &IExternalStreamSession::EchoFrames, "PurgeStream"},
            {12, &IExternalStreamSession::Succeed, "RenameStream"},
        };
        // clang-format on
        RegisterHandlers(functions);
        std::scoped_lock lk{streams->mutex};
        stream->state_event = service_context.CreateEvent("nx2:audsmx:u:StreamStateEvent");
        stream->data_event = service_context.CreateEvent("nx2:audsmx:u:StreamDataEvent");
    }

    ~IExternalStreamSession() override {
        // The drain timer signals these events under the lock: unlist the stream first.
        {
            std::scoped_lock lk{streams->mutex};
            streams->streams.erase(stream->id);
            if (stream->sink) {
                SinkFor(system, *stream).CloseStream(stream->sink);
                stream->sink = nullptr;
            }
            service_context.CloseEvent(stream->state_event);
            service_context.CloseEvent(stream->data_event);
            stream->state_event = nullptr;
            stream->data_event = nullptr;
        }
    }

private:
    // States the session reports: 0 created, 1 open, 2 open and engaged.
    void OpenStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto sample_type = rp.Pop<u32>();
        const auto channels = rp.Pop<u32>();
        const auto sample_rate = rp.Pop<u32>();
        const auto frames = rp.Pop<u32>();
        const auto prefill = rp.Pop<u32>();
        const auto flags = rp.Pop<u32>();
        LOG_INFO(Service_Audio,
                 "stream {} open: sample type {}, {} channels, {} Hz, {} frames, prefill {}, flags "
                 "{:#x}",
                 stream->id, sample_type, channels, sample_rate, frames, prefill, flags);
        const u32 sample_bytes = sample_type == 5 ? 4 : sample_type == 2 ? 2 : 0;
        const u64 data_offset = 0x1040 + ((flags & 1) ? 0x80 : 0);
        if (!frames || frames > 48000 || !channels || channels > 8 ||
            (sample_bytes &&
             data_offset + u64{frames + 1} * channels * sample_bytes > stream->size)) {
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultInvalidArgument);
            return;
        }
        {
            std::scoped_lock lk{streams->mutex};
            if (stream->sink) {
                SinkFor(system, *stream).CloseStream(stream->sink);
                stream->sink = nullptr;
            }
            stream->sample_type = sample_type;
            stream->channels = channels;
            stream->sample_rate = sample_rate;
            stream->frames = frames;
            stream->data_offset = data_offset;
            const bool input = stream->kind == 1;
            // A virtual-output loopback needs no capture device: it carries the game's own mix.
            const bool from_microphone = stream->source.empty() || stream->source == RawInputDevice;
            if (sample_bytes && sample_rate == 48000 &&
                (channels == 1 || channels == 2 || (channels == 6 && !input))) {
                if (!input || from_microphone) {
                    stream->sink = SinkFor(system, *stream).AcquireSinkStream(
                        system, input || channels != 1 ? channels : 2,
                        input ? "NX2 Sysmix In" : "NX2 Sysmix",
                        input ? AudioCore::Sink::StreamType::In : AudioCore::Sink::StreamType::Out);
                    stream->sink->SetSystemVolume(stream->gain);
                }
            } else {
                LOG_WARNING(Service_Audio,
                            "Unsupported sysmix {} format: type {} channels {} rate {}",
                            input ? "input" : "output", sample_type, channels, sample_rate);
            }
            stream->owed_frames = 0;
            stream->open = true;
            stream->state_event->Signal(system.Kernel());
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void CloseStream(HLERequestContext& ctx) {
        {
            std::scoped_lock lk{streams->mutex};
            stream->open = false;
            if (stream->sink) {
                SinkFor(system, *stream).CloseStream(stream->sink);
                stream->sink = nullptr;
            }
            stream->state_event->Signal(system.Kernel());
        }
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void GetStreamState(HLERequestContext& ctx) {
        u32 state{};
        {
            std::scoped_lock lk{streams->mutex};
            state = stream->open ? (stream->engaged ? 2 : 1) : 0;
        }
        LOG_DEBUG(Service_Audio, "stream {} state {}", stream->id, state);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(state);
    }

    void GetStreamGain(HLERequestContext& ctx) {
        std::scoped_lock lk{streams->mutex};
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push(stream->gain);
    }

    void SetStreamGain(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const f32 gain = rp.Pop<f32>();
        std::scoped_lock lk{streams->mutex};
        stream->gain = std::isfinite(gain) ? std::clamp(gain, 0.0f, 1.0f) : 0.0f;
        if (stream->sink)
            stream->sink->SetSystemVolume(stream->gain);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void AcquireStateChangeEvent(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, stream->state_event->GetReadableEvent());
    }

    void AcquireDataEvent(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto mode = rp.Pop<u32>();
        const auto threshold = rp.Pop<s32>();
        LOG_INFO(Service_Audio, "stream {} data event: mode {}, threshold {}", stream->id, mode,
                 threshold);
        {
            // For an output the game waits on this before its first write, so signal it at once when
            // the ring already has room (it is empty right after OpenStream); an input stream is
            // signalled by the drain timer once captured frames land in the ring.
            std::scoped_lock lk{streams->mutex};
            stream->data_mode = mode;
            stream->data_threshold = threshold;
            if (mode != 0 && stream->open && stream->kind == 0 &&
                static_cast<s64>(stream->frames) >= threshold) {
                stream->data_event->Signal(system.Kernel());
            }
        }
        IPC::ResponseBuilder rb{ctx, 2, 1};
        rb.Push(ResultSuccess);
        rb.PushCopyObjects(ctx, stream->data_event->GetReadableEvent());
    }

    void EchoFrames(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto frames = rp.Pop<s32>();
        LOG_DEBUG(Service_Audio, "stream {} command {} frames {}", stream->id, ctx.GetCommand(),
                  frames);
        IPC::ResponseBuilder rb{ctx, 3};
        rb.Push(ResultSuccess);
        rb.Push<u32>(static_cast<u32>((std::max)(frames, 0)));
    }

    void ClockStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto ns = rp.Pop<s64>();
        LOG_DEBUG(Service_Audio, "stream {} clock {} ns", stream->id, ns);
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void Succeed(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    KernelHelpers::ServiceContext service_context;
    std::shared_ptr<Nx2SysmixStreams> streams;
    std::shared_ptr<Nx2SysmixStream> stream;
};

// nn::audio::sysmix::IUserSession, the session audsmx:u command 0 returns. Commands:
// - 1(u32, u32 creation flags, u64 pid, u64 buffer address, u64 buffer size, name buffer):
//   u32 stream id and the stream object.
// - 2 / 3(buffer of u32 stream ids): engage / disengage those streams.
class IUserSession final : public ServiceFramework<IUserSession> {
public:
    explicit IUserSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:audsmx:u:session"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {1, &IUserSession::CreateStream, "CreateStream"},
            {2, &IUserSession::EngageStreams, "EngageStreams"},
            {3, &IUserSession::DisengageStreams, "DisengageStreams"},
        };
        // clang-format on
        RegisterHandlers(functions);
        drain_timer = Core::Timing::CreateEvent(
            "nx2:audsmx:u drain",
            [streams = streams, &system = system, owner = session_id](
                s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                Drain(system, *streams, owner);
                return std::nullopt;
            });
        system.CoreTiming().ScheduleLoopingEvent(DrainInterval, DrainInterval, drain_timer);
    }

    ~IUserSession() override {
        system.CoreTiming().UnscheduleEvent(drain_timer);
    }

private:
    static constexpr std::chrono::nanoseconds DrainInterval{std::chrono::milliseconds{5}};
    static constexpr u64 ReadIndexOffset = 0x44;
    static constexpr u64 WriteIndexOffset = 0x1004;

    // Takes up to one interval's worth of frames out of each engaged ring (frames owed while a ring
    // is empty are dropped, as an underrun plays silence), and signals the data event of every open
    // stream with at least its threshold of free frames, engaged or not: the game fills the ring
    // before it engages the stream.
    static void Drain(Core::System& system, Nx2SysmixStreams& state, u32 owner) {
        std::scoped_lock lk{state.mutex};
        auto& memory = system.ApplicationMemory();
        for (auto& [id, stream] : state.streams) {
            if (stream->owner != owner || !stream->open || stream->frames == 0 || !stream->buffer) {
                continue;
            }
            const u32 capacity = stream->frames + 1;
            const u32 write = memory.Read32(stream->buffer + WriteIndexOffset);
            const u32 read = memory.Read32(stream->buffer + ReadIndexOffset);
            if (write >= capacity || read >= capacity) {
                continue;
            }
            u32 filled = (write + capacity - read) % capacity;
            if (!stream->engaged && stream->sink && !stream->sink->IsPaused()) {
                stream->sink->Stop();
                stream->sink->ClearQueue();
            }
            if (stream->engaged) {
                stream->owed_frames +=
                    stream->sample_rate * std::chrono::duration<double>(DrainInterval).count();
                const auto owed = static_cast<u32>(stream->owed_frames);
                stream->owed_frames -= owed;
                if (stream->kind == 1) {
                    filled = CaptureIntoRing(memory, state, *stream, write, filled, owed, capacity);
                    // EDEN_NX2_AUDIO_IN_LEVEL: once a second, show where the game is reading and
                    // whether what sits there is our audio.
                    if (LogAudioInput()) {
                        stream->report_seconds +=
                            std::chrono::duration<double>(DrainInterval).count();
                    }
                    if (LogAudioInput() && stream->report_seconds >= 1.0) {
                        stream->report_seconds = 0.0;
                        const u32 read_index = memory.Read32(stream->buffer + ReadIndexOffset);
                        const u32 write_index = memory.Read32(stream->buffer + WriteIndexOffset);
                        const u64 stride = u64{stream->channels} *
                                           (stream->sample_type == 5 ? 4u : 2u);
                        std::array<s16, 4> head{};
                        if (read_index < capacity) {
                            memory.ReadBlock(stream->buffer + stream->data_offset +
                                                 u64{read_index} * stride,
                                             head.data(), head.size() * sizeof(s16));
                        }
                        LOG_INFO(Service_Audio,
                                 "sysmix stream {} in: read {} write {} at-read [{}, {}, {}, {}] "
                                 "peak {}",
                                 stream->id, read_index, write_index, head[0], head[1], head[2],
                                 head[3], stream->report_peak);
                        stream->report_peak = 0;
                    }
                } else if (!stream->sink || stream->sink->GetQueueSize() < 8) {
                    filled = PlayFromRing(memory, state, *stream, read, filled, owed, capacity);
                }
            }
            // An output stream wakes the game once it has room to write, an input stream once there
            // are frames to read.
            const s64 available = stream->kind == 1 ? static_cast<s64>(filled)
                                                   : static_cast<s64>(capacity - 1 - filled);
            if (stream->data_event && stream->data_mode != 0 &&
                available >= stream->data_threshold) {
                stream->data_event->Signal(system.Kernel());
            }
        }
    }

    // Takes up to one interval's worth of the game's frames out of an output ring and queues them to
    // the sink. Frames owed while the ring is empty are dropped, as an underrun plays silence.
    static u32 PlayFromRing(Core::Memory::Memory& memory, Nx2SysmixStreams& state,
                            Nx2SysmixStream& stream, u32 read, u32 filled, u32 owed,
                            u32 capacity) {
        const u32 taken = (std::min)(filled, owed);
        if (!stream.sink || !owed) {
            return filled;
        }
        const u32 channels = stream.channels;
        const u32 output_channels = channels == 1 ? 2 : channels;
        std::vector<s16> samples(u64{owed} * output_channels, 0);
        const u32 sample_bytes = stream.sample_type == 5 ? 4 : 2;
        std::vector<u8> input(u64{taken} * channels * sample_bytes);
        const u32 first = (std::min)(taken, capacity - read);
        const u64 data = stream.buffer + stream.data_offset;
        const u64 stride = u64{channels} * sample_bytes;
        memory.ReadBlock(data + u64{read} * stride, input.data(), first * stride);
        if (first < taken) {
            memory.ReadBlock(data, input.data() + first * stride, (taken - first) * stride);
        }
        s32 peak{};
        for (u32 frame = 0; frame < taken; ++frame) {
            for (u32 channel = 0; channel < output_channels; ++channel) {
                const u32 source = frame * channels + (channels == 1 ? 0 : channel);
                s16 sample{};
                if (sample_bytes == 4) {
                    f32 value{};
                    std::memcpy(&value, input.data() + source * 4, 4);
                    if (std::isfinite(value)) {
                        const f32 scaled = std::clamp(value, -1.0f, 1.0f) * 32768.0f;
                        sample = static_cast<s16>(std::clamp(scaled, -32768.0f, 32767.0f));
                    }
                } else {
                    std::memcpy(&sample, input.data() + source * 2, 2);
                }
                samples[u64{frame} * output_channels + channel] = sample;
                peak = (std::max)(peak, std::abs(static_cast<s32>(sample)));
            }
        }
        AudioCore::Sink::SinkBuffer buffer{owed, 0, 0, false};
        stream.sink->AppendBuffer(buffer, samples);
        stream.sink->Start();
        // Keep what was played for input streams that loop the virtual output device back.
        for (u32 frame = 0; frame < taken; ++frame) {
            s32 sum{};
            for (u32 channel = 0; channel < output_channels; ++channel) {
                sum += samples[u64{frame} * output_channels + channel];
            }
            state.loopback.push_back(static_cast<s16>(sum / static_cast<s32>(output_channels)));
        }
        while (state.loopback.size() > MaxLoopbackFrames) {
            state.loopback.pop_front();
        }
        if (peak && !stream.reported_samples) {
            stream.reported_samples = true;
            LOG_INFO(Service_Audio, "sysmix stream {} output active: {} frames, PCM16 peak {}",
                     stream.id, taken, peak);
        }
        if (taken != 0) {
            memory.Write32(stream.buffer + ReadIndexOffset, (read + taken) % capacity);
            filled -= taken;
        }
        return filled;
    }

    // Fills up to one interval's worth of frames into an input ring, which is what a title reading
    // its microphone consumes: the host microphone for a "TrueRawAudioIn" stream, the game's own
    // rendered mix for a virtual-output loop. Frames owed while the game is behind are dropped, like
    // a stream that overruns.
    static u32 CaptureIntoRing(Core::Memory::Memory& memory, Nx2SysmixStreams& state,
                               Nx2SysmixStream& stream, u32 write, u32 filled, u32 owed,
                               u32 capacity) {
        const u32 free_frames = capacity - 1 - filled;
        const u32 want = (std::min)(owed, free_frames);
        const bool microphone = stream.source.empty() || stream.source == RawInputDevice;
        if (!want || (microphone && !stream.sink)) {
            return filled;
        }
        std::vector<s16> frames(u64{want} * stream.channels, 0);
        if (microphone) {
            const u32 device_channels = (std::max)(stream.sink->GetDeviceChannels(), 1u);
            // The capture device runs stereo (and ReleaseBuffer pads with silence before the first
            // samples arrive), so a mono guest stream averages the device channels.
            stream.sink->Start();
            const std::vector<s16> captured =
                stream.sink->PopSamples(u64{want} * device_channels);
            for (u32 frame = 0; frame < want; ++frame) {
                const u64 base = u64{frame} * device_channels;
                if (base + device_channels > captured.size()) {
                    break;
                }
                if (stream.channels == 1) {
                    s32 sum{};
                    for (u32 source = 0; source < device_channels; ++source) {
                        sum += captured[base + source];
                    }
                    frames[frame] = static_cast<s16>(sum / static_cast<s32>(device_channels));
                } else {
                    for (u32 channel = 0; channel < stream.channels; ++channel) {
                        frames[u64{frame} * stream.channels + channel] =
                            captured[base + channel % device_channels];
                    }
                }
            }
        } else {
            // A capture stream of the virtual output device is where a title listens for the
            // microphone; EDEN_NX2_MIC_LOOPBACK_OUTPUT swaps in the game's own rendered mix.
            auto& source = LoopbackUsesOutputMix() ? state.loopback : state.mic_tail;
            for (u32 frame = 0; frame < want; ++frame) {
                s16 sample{};
                if (!source.empty()) {
                    sample = source.front();
                    source.pop_front();
                }
                for (u32 channel = 0; channel < stream.channels; ++channel) {
                    frames[u64{frame} * stream.channels + channel] = sample;
                }
            }
        }
        if (microphone) {
            for (const s16 sample : frames) {
                state.mic_tail.push_back(sample);
            }
            while (state.mic_tail.size() > MaxLoopbackFrames) {
                state.mic_tail.pop_front();
            }
        }
        const u32 sample_bytes = stream.sample_type == 5 ? 4 : 2;
        std::vector<u8> out(u64{want} * stream.channels * sample_bytes);
        s32 peak{};
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const u64 offset = u64{i} * sample_bytes;
            if (sample_bytes == 4) {
                const f32 value = static_cast<f32>(frames[i]) / 32768.0f;
                std::memcpy(out.data() + offset, &value, 4);
            } else {
                std::memcpy(out.data() + offset, &frames[i], 2);
            }
            peak = (std::max)(peak, std::abs(static_cast<s32>(frames[i])));
        }
        stream.report_peak = (std::max)(stream.report_peak, peak);
        const u64 stride = u64{stream.channels} * sample_bytes;
        const u64 data = stream.buffer + stream.data_offset;
        const u32 first = (std::min)(want, capacity - write);
        memory.WriteBlock(data + u64{write} * stride, out.data(), first * stride);
        if (first < want) {
            memory.WriteBlock(data, out.data() + first * stride, (want - first) * stride);
        }
        memory.Write32(stream.buffer + WriteIndexOffset, (write + want) % capacity);
        if (peak && !stream.reported_samples) {
            stream.reported_samples = true;
            LOG_INFO(Service_Audio, "sysmix stream {} {} active: {} frames, PCM16 peak {}",
                     stream.id, microphone ? "input" : "loopback", want, peak);
        }
        return filled + want;
    }

    void CreateStream(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const auto kind = rp.Pop<u32>();
        const auto flags = rp.Pop<u32>();
        [[maybe_unused]] const auto pid = rp.Pop<u64>();
        const auto buffer = rp.Pop<u64>();
        const auto size = rp.Pop<u64>();
        const auto name = Common::StringFromBuffer(ctx.ReadBuffer(0));
        auto stream = std::make_shared<Nx2SysmixStream>();
        stream->kind = kind;
        stream->buffer = buffer;
        stream->size = size;
        // 'InputStream-<n><=<format><=<device>': the device an input stream carries.
        if (const auto suffix = name.rfind("<="); suffix != std::string::npos) {
            stream->source = name.substr(suffix + 2);
        }
        {
            std::scoped_lock lk{streams->mutex};
            stream->id = streams->next_id++;
            stream->owner = session_id;
            streams->streams.emplace(stream->id, stream);
        }
        LOG_INFO(Service_Audio, "stream {} '{}': kind {}, flags {:#x}, buffer {:#x} size {:#x}",
                 stream->id, name, kind, flags, buffer, size);
        IPC::ResponseBuilder rb{ctx, 3, 0, 1};
        rb.Push(ResultSuccess);
        rb.Push(stream->id);
        rb.PushIpcInterface<IExternalStreamSession>(ctx, system, streams, stream);
    }

    void SetEngaged(HLERequestContext& ctx, bool engaged) {
        const auto buffer = ctx.ReadBuffer(0);
        std::vector<u32> ids(buffer.size() / sizeof(u32));
        std::memcpy(ids.data(), buffer.data(), ids.size() * sizeof(u32));
        {
            std::scoped_lock lk{streams->mutex};
            for (const u32 id : ids) {
                if (const auto it = streams->streams.find(id); it != streams->streams.end()) {
                    it->second->engaged = engaged;
                    it->second->owed_frames = 0;
                    if (it->second->state_event) {
                        it->second->state_event->Signal(system.Kernel());
                    }
                }
            }
        }
        LOG_INFO(Service_Audio, "{} streams {}", engaged ? "engage" : "disengage",
                 fmt::join(ids, ","));
        IPC::ResponseBuilder rb{ctx, 2};
        rb.Push(ResultSuccess);
    }

    void EngageStreams(HLERequestContext& ctx) {
        SetEngaged(ctx, true);
    }

    void DisengageStreams(HLERequestContext& ctx) {
        SetEngaged(ctx, false);
    }

    static inline std::atomic<u32> next_session_id{1};
    const u32 session_id = next_session_id++;
    std::shared_ptr<Nx2SysmixStreams> streams = Nx2SysmixStreamList();
    std::shared_ptr<Core::Timing::EventType> drain_timer;
};

// Switch 2 "audsmx:u" (nn::audio::sysmix::IUserSessionManager): command 0 (16 bytes, the pid and
// a copy handle) returns the user session.
class IUserSessionManager final : public ServiceFramework<IUserSessionManager> {
public:
    explicit IUserSessionManager(Core::System& system_)
        : ServiceFramework{system_, "nx2:audsmx:u"} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &IUserSessionManager::OpenUserSession, "OpenUserSession"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    void OpenUserSession(HLERequestContext& ctx) {
        LOG_DEBUG(Service_Audio, "called");
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<IUserSession>(ctx, system);
    }
};

} // namespace Sysmix

} // namespace Service::Audio
