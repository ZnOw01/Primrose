// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <thread>

#include "common/common_types.h"
#include "common/dynamic_library.h"
#include "yuzu/render/perf_sampler.h"

#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <string_view>
#else
#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto SampleInterval = std::chrono::milliseconds{250};
/// Five samples: busy % over the last ~1 s.
constexpr size_t ThreadHistory = 5;
constexpr auto NameRefresh = std::chrono::seconds{2};

double Seconds(Clock::time_point t) {
    return std::chrono::duration<double>(t.time_since_epoch()).count();
}

#ifndef _WIN32
std::string ReadFirstLine(const std::string& path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}
#endif

/// Per-thread CPU time from the OS, grouped by thread name.
class ThreadSampler {
public:
    ThreadSampler() {
#ifdef _WIN32
        get_description = reinterpret_cast<GetThreadDescriptionFn>(
            GetProcAddress(GetModuleHandleA("KernelBase.dll"), "GetThreadDescription"));
#endif
    }

    std::vector<PerfThreadGroup> Sample() {
        const auto now = Clock::now();
        const bool refresh_names = now - last_names >= NameRefresh;
        if (refresh_names) {
            last_names = now;
        }
        for (auto& [tid, state] : threads) {
            state.seen = false;
        }
        Collect(refresh_names, Seconds(now));
        std::erase_if(threads, [](const auto& entry) { return !entry.second.seen; });

        std::map<std::string, PerfThreadGroup> groups;
        for (const auto& [tid, state] : threads) {
            if (state.history.size() < 2) {
                continue;
            }
            const auto& [cpu0, wall0] = state.history.front();
            const auto& [cpu1, wall1] = state.history.back();
            const double busy =
                std::clamp(100.0 * (cpu1 - cpu0) / std::max(1e-6, wall1 - wall0), 0.0, 100.0);
            auto& group = groups[state.name];
            group.name = state.name;
            ++group.count;
            group.busy_max = std::max(group.busy_max, busy);
        }
        std::vector<PerfThreadGroup> out;
        out.reserve(groups.size());
        for (auto& [name, group] : groups) {
            out.push_back(std::move(group));
        }
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.busy_max > b.busy_max; });
        return out;
    }

private:
    struct ThreadState {
        std::string name;
        std::deque<std::pair<double, double>> history; ///< (CPU seconds, wall seconds)
        bool seen = false;
    };

    void Record(u64 tid, double cpu_s, double wall_s, const std::string* name) {
        auto& state = threads[tid];
        state.seen = true;
        if (name != nullptr) {
            state.name = *name;
        }
        state.history.emplace_back(cpu_s, wall_s);
        if (state.history.size() > ThreadHistory) {
            state.history.pop_front();
        }
    }

#ifdef _WIN32
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

    std::string Describe(HANDLE handle, DWORD tid) const {
        std::string name;
        PWSTR wide = nullptr;
        if (get_description != nullptr && SUCCEEDED(get_description(handle, &wide)) && wide) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (len > 1) {
                name.resize(static_cast<size_t>(len));
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), len, nullptr, nullptr);
                name.resize(static_cast<size_t>(len - 1));
            }
            LocalFree(wide);
        }
        return name.empty() ? "tid " + std::to_string(tid) : name;
    }

    void Collect(bool refresh_names, double wall_s) {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) {
            return;
        }
        const DWORD pid = GetCurrentProcessId();
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
            if (entry.th32OwnerProcessID != pid) {
                continue;
            }
            const HANDLE handle =
                OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (handle == nullptr) {
                continue;
            }
            FILETIME created, exited, kernel, user;
            if (GetThreadTimes(handle, &created, &exited, &kernel, &user)) {
                const u64 ticks = (u64{kernel.dwHighDateTime} << 32 | kernel.dwLowDateTime) +
                                  (u64{user.dwHighDateTime} << 32 | user.dwLowDateTime);
                const bool known = threads.contains(entry.th32ThreadID);
                std::string name;
                if (!known || refresh_names) {
                    name = Describe(handle, entry.th32ThreadID);
                }
                Record(entry.th32ThreadID, static_cast<double>(ticks) * 1e-7, wall_s,
                       !known || refresh_names ? &name : nullptr);
            }
            CloseHandle(handle);
        }
        CloseHandle(snapshot);
    }

    GetThreadDescriptionFn get_description = nullptr;
#else
    void Collect(bool refresh_names, double wall_s) {
        DIR* const dir = opendir("/proc/self/task");
        if (dir == nullptr) {
            return;
        }
        while (const dirent* const entry = readdir(dir)) {
            char* end = nullptr;
            const u64 tid = std::strtoull(entry->d_name, &end, 10);
            if (end == entry->d_name || *end != '\0') {
                continue;
            }
            const std::string base = std::string("/proc/self/task/") + entry->d_name;
            // schedstat: nanoseconds spent on a CPU, exact (stat's utime/stime are in ticks).
            const std::string schedstat = ReadFirstLine(base + "/schedstat");
            if (schedstat.empty()) {
                continue;
            }
            const double cpu_s = static_cast<double>(std::strtoull(schedstat.c_str(), nullptr, 10)) * 1e-9;
            const bool known = threads.contains(tid);
            std::string name;
            if (!known || refresh_names) {
                name = ReadFirstLine(base + "/comm");
                if (name.empty()) {
                    name = "tid " + std::to_string(tid);
                }
            }
            Record(tid, cpu_s, wall_s, !known || refresh_names ? &name : nullptr);
        }
        closedir(dir);
    }
#endif

    std::map<u64, ThreadState> threads;
    Clock::time_point last_names{};
};

/// CPU time and resident memory of the whole process.
class ProcessSampler {
public:
    std::optional<double> CpuPercent(double wall_s) {
        const std::optional<double> cpu_s = CpuSeconds();
        if (!cpu_s) {
            return std::nullopt;
        }
        history.emplace_back(*cpu_s, wall_s);
        if (history.size() > ThreadHistory) {
            history.pop_front();
        }
        if (history.size() < 2) {
            return std::nullopt;
        }
        const double cores = std::max(1u, std::thread::hardware_concurrency());
        const auto& [cpu0, wall0] = history.front();
        const auto& [cpu1, wall1] = history.back();
        return std::clamp(100.0 * (cpu1 - cpu0) / std::max(1e-6, wall1 - wall0) / cores, 0.0,
                          100.0);
    }

    std::optional<unsigned long long> ResidentBytes() const {
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS counters{};
        if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
            return std::nullopt;
        }
        return counters.WorkingSetSize;
#else
        // statm: total pages, resident pages, ...
        const std::string statm = ReadFirstLine("/proc/self/statm");
        const size_t space = statm.find(' ');
        if (space == std::string::npos) {
            return std::nullopt;
        }
        return std::strtoull(statm.c_str() + space + 1, nullptr, 10) *
               static_cast<unsigned long long>(sysconf(_SC_PAGESIZE));
#endif
    }

private:
    static std::optional<double> CpuSeconds() {
#ifdef _WIN32
        FILETIME created, exited, kernel, user;
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            return std::nullopt;
        }
        const u64 ticks = (u64{kernel.dwHighDateTime} << 32 | kernel.dwLowDateTime) +
                          (u64{user.dwHighDateTime} << 32 | user.dwLowDateTime);
        return static_cast<double>(ticks) * 1e-7;
#else
        rusage usage{};
        if (getrusage(RUSAGE_SELF, &usage) != 0) {
            return std::nullopt;
        }
        const auto seconds = [](const timeval& t) {
            return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_usec) * 1e-6;
        };
        return seconds(usage.ru_utime) + seconds(usage.ru_stime);
#endif
    }

    std::deque<std::pair<double, double>> history; ///< (CPU seconds, wall seconds)
};

/// NVIDIA GPU load through the driver's NVML library: this process's SM use when the driver
/// lists it, else the busiest device's.
class NvmlSampler {
public:
    NvmlSampler() {
#ifdef _WIN32
        const char* const names[] = {"nvml.dll"};
#else
        const char* const names[] = {"libnvidia-ml.so.1", "libnvidia-ml.so"};
#endif
        for (const char* name : names) {
            if (library.Open(name)) {
                break;
            }
        }
        if (!library.IsOpen() || !library.GetSymbol("nvmlInit_v2", &init) ||
            !library.GetSymbol("nvmlShutdown", &shutdown) ||
            !library.GetSymbol("nvmlDeviceGetCount_v2", &get_count) ||
            !library.GetSymbol("nvmlDeviceGetHandleByIndex_v2", &get_handle) ||
            !library.GetSymbol("nvmlDeviceGetUtilizationRates", &get_utilization) || init() != 0) {
            return;
        }
        initialized = true;
        (void)library.GetSymbol("nvmlDeviceGetProcessUtilization", &get_process_utilization);
        unsigned count = 0;
        if (get_count(&count) == 0) {
            for (unsigned i = 0; i < count; ++i) {
                void* device = nullptr;
                if (get_handle(i, &device) == 0) {
                    devices.push_back(device);
                }
            }
        }
#ifdef _WIN32
        pid = GetCurrentProcessId();
#else
        pid = static_cast<unsigned>(getpid());
#endif
    }

    ~NvmlSampler() {
        if (initialized) {
            shutdown();
        }
    }

    bool Available() const {
        return initialized && !devices.empty();
    }

    /// (busy %, device wide)
    std::optional<std::pair<double, bool>> Sample() {
        if (!Available()) {
            return std::nullopt;
        }
        if (get_process_utilization != nullptr) {
            if (const auto process = ProcessBusy()) {
                return std::pair{*process, false};
            }
        }
        std::optional<double> best;
        for (void* device : devices) {
            Utilization utilization{};
            if (get_utilization(device, &utilization) == 0) {
                best = std::max(best.value_or(0.0), static_cast<double>(utilization.gpu));
            }
        }
        if (!best) {
            return std::nullopt;
        }
        return std::pair{*best, true};
    }

private:
    struct Utilization {
        unsigned gpu;
        unsigned memory;
    };
    struct ProcessSample {
        unsigned pid;
        unsigned long long timestamp;
        unsigned sm, memory, encoder, decoder;
    };

    /// SM use of this process over the last second, averaged over the driver's samples.
    /// Once the driver has listed this process, no sample means idle; before that (or in a
    /// PID namespace, where it never matches) the device-wide figure stands in.
    std::optional<double> ProcessBusy() {
        const auto since_us = static_cast<unsigned long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count() -
            1'000'000);
        double sum = 0.0;
        int samples = 0;
        for (void* device : devices) {
            unsigned count = 0;
            // NVML_ERROR_INSUFFICIENT_SIZE (7) reports the needed count.
            if (get_process_utilization(device, nullptr, &count, since_us) != 7 || count == 0) {
                continue;
            }
            std::vector<ProcessSample> buffer(count);
            if (get_process_utilization(device, buffer.data(), &count, since_us) != 0) {
                continue;
            }
            for (unsigned i = 0; i < count; ++i) {
                if (buffer[i].pid == pid) {
                    sum += buffer[i].sm;
                    ++samples;
                }
            }
        }
        if (samples > 0) {
            seen_process = true;
            return std::min(100.0, sum / samples);
        }
        return seen_process ? std::optional<double>{0.0} : std::nullopt;
    }

    Common::DynamicLibrary library;
    int (*init)() = nullptr;
    int (*shutdown)() = nullptr;
    int (*get_count)(unsigned*) = nullptr;
    int (*get_handle)(unsigned, void**) = nullptr;
    int (*get_utilization)(void*, Utilization*) = nullptr;
    int (*get_process_utilization)(void*, ProcessSample*, unsigned*, unsigned long long) = nullptr;
    std::vector<void*> devices;
    unsigned pid = 0;
    bool initialized = false;
    bool seen_process = false;
};

#ifdef _WIN32
/// Per-process GPU engine utilisation from the "GPU Engine" performance counters (Windows 10
/// 1709+, WDDM 2.x), any vendor. pdh.dll is loaded at run time.
class PdhGpuSampler {
public:
    PdhGpuSampler() {
        if (!library.Open("pdh.dll") || !library.GetSymbol("PdhOpenQueryW", &open_query) ||
            !library.GetSymbol("PdhAddEnglishCounterW", &add_counter) ||
            !library.GetSymbol("PdhCollectQueryData", &collect) ||
            !library.GetSymbol("PdhCloseQuery", &close_query) ||
            !library.GetSymbol("PdhGetFormattedCounterArrayW", &get_array)) {
            open_query = nullptr;
        }
    }

    ~PdhGpuSampler() {
        Close();
    }

    bool Available() const {
        return open_query != nullptr && !failed;
    }

    /// Busiest engine type %, once two collections have been made.
    std::optional<double> Sample() {
        if (!Available()) {
            return std::nullopt;
        }
        // Engine instances appear once the process uses them, so the wildcard query is rebuilt.
        if (query == nullptr || Clock::now() - built >= std::chrono::seconds{5}) {
            Build();
            if (query == nullptr) {
                return std::nullopt;
            }
        }
        if (collect(query) != ERROR_SUCCESS || ++collected < 2) {
            return std::nullopt;
        }

        DWORD size = 0, count = 0;
        if (get_array(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) !=
                static_cast<PDH_STATUS>(PDH_MORE_DATA) ||
            size == 0) {
            return 0.0;
        }
        std::vector<std::byte> buffer(size);
        auto* const items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (get_array(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) !=
            ERROR_SUCCESS) {
            return std::nullopt;
        }
        std::map<std::wstring, double> by_type;
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) {
                continue;
            }
            const std::wstring_view name = items[i].szName ? items[i].szName : L"";
            std::wstring type = L"Other";
            if (const size_t at = name.find(L"engtype_"); at != std::wstring_view::npos) {
                type = std::wstring(name.substr(at + 8));
            }
            by_type[type] += items[i].FmtValue.doubleValue;
        }
        double busiest = 0.0;
        for (const auto& [type, value] : by_type) {
            busiest = std::max(busiest, value);
        }
        return std::min(100.0, busiest);
    }

private:
    void Build() {
        Close();
        built = Clock::now();
        collected = 0;
        if (open_query(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
            failed = true;
            return;
        }
        const std::wstring path = L"\\GPU Engine(pid_" + std::to_wstring(GetCurrentProcessId()) +
                                  L"_*)\\Utilization Percentage";
        if (add_counter(query, path.c_str(), 0, &counter) != ERROR_SUCCESS) {
            Close();
            failed = true;
        }
    }

    void Close() {
        if (query != nullptr) {
            close_query(query);
            query = nullptr;
        }
    }

    Common::DynamicLibrary library;
    PDH_STATUS(WINAPI* open_query)(LPCWSTR, DWORD_PTR, PDH_HQUERY*) = nullptr;
    PDH_STATUS(WINAPI* add_counter)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*) = nullptr;
    PDH_STATUS(WINAPI* collect)(PDH_HQUERY) = nullptr;
    PDH_STATUS(WINAPI* close_query)(PDH_HQUERY) = nullptr;
    PDH_STATUS(WINAPI* get_array)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD,
                                  PPDH_FMT_COUNTERVALUE_ITEM_W) = nullptr;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    Clock::time_point built{};
    int collected = 0;
    bool failed = false;
};
#else
/// Device-wide load of the first AMD GPU that reports it through amdgpu's sysfs.
class AmdSysfsSampler {
public:
    AmdSysfsSampler() {
        for (int card = 0; card < 16; ++card) {
            const std::string device = "/sys/class/drm/card" + std::to_string(card) + "/device";
            if (ReadFirstLine(device + "/vendor") == "0x1002" &&
                !ReadFirstLine(device + "/gpu_busy_percent").empty()) {
                path = device + "/gpu_busy_percent";
                break;
            }
        }
    }

    std::optional<double> Sample() const {
        if (path.empty()) {
            return std::nullopt;
        }
        const std::string text = ReadFirstLine(path);
        if (text.empty()) {
            return std::nullopt;
        }
        return std::strtod(text.c_str(), nullptr);
    }

private:
    std::string path;
};
#endif

} // namespace

struct PerfSampler::Impl {
    ThreadSampler threads;
    ProcessSampler process;
    NvmlSampler nvml;
#ifdef _WIN32
    PdhGpuSampler pdh;
#else
    AmdSysfsSampler amd;
#endif

    void SampleGpu(PerfHostSnapshot& out) {
#ifdef _WIN32
        if (pdh.Available()) {
            if (const auto busy = pdh.Sample()) {
                out.gpu_busy = busy;
            }
            return;
        }
#endif
        if (const auto sample = nvml.Sample()) {
            out.gpu_busy = sample->first;
            out.gpu_busy_device_wide = sample->second;
            return;
        }
#ifndef _WIN32
        if (const auto busy = amd.Sample()) {
            out.gpu_busy = busy;
            out.gpu_busy_device_wide = true;
        }
#endif
    }
};

PerfSampler::PerfSampler()
    : thread{[this](std::stop_token stop) { Run(std::move(stop)); }} {}

PerfSampler::~PerfSampler() {
    thread.request_stop();
    if (thread.joinable()) {
        thread.join();
    }
}

PerfHostSnapshot PerfSampler::Snapshot() const {
    std::scoped_lock lock{mutex};
    return snapshot;
}

void PerfSampler::Run(std::stop_token stop) {
    // Built here so library loads and driver setup stay off the UI thread.
    impl = std::make_unique<Impl>();
    std::optional<double> last_busy;
    bool last_device_wide = false;
    while (!stop.stop_requested()) {
        const auto next = Clock::now() + SampleInterval;

        PerfHostSnapshot sample;
        sample.threads = impl->threads.Sample();
        sample.process_cpu = impl->process.CpuPercent(Seconds(Clock::now()));
        sample.ram_used = impl->process.ResidentBytes();
        impl->SampleGpu(sample);
        // Rate counters skip a beat now and then: keep the previous GPU figures meanwhile.
        if (!sample.gpu_busy) {
            sample.gpu_busy = last_busy;
            sample.gpu_busy_device_wide = last_device_wide;
        }
        last_busy = sample.gpu_busy;
        last_device_wide = sample.gpu_busy_device_wide;
        {
            std::scoped_lock lock{mutex};
            snapshot = std::move(sample);
        }

        std::mutex wait_mutex;
        std::unique_lock wait_lock{wait_mutex};
        std::condition_variable_any().wait_until(wait_lock, stop, next, [] { return false; });
    }
    impl.reset();
}
