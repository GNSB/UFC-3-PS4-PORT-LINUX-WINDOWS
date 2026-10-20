// SPDX-License-Identifier: GPL-2.0-or-later
// ufc3 (UFC-65): one line every BB_GROWTH_SECONDS (60) with what the port keeps over a session.
// FPS fell by 5-6 over an hour of quick fights on a Steam Deck (GitHub #24) and after arena
// changes on a Radeon 680M (#32), without a log saying why; the first and last lines of a long
// session show which of these grew while the frame rate went down.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <time.h>
#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#include <string>
#include <vector>
#endif
#include "bbport_toggles.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"

#ifdef _WIN32
extern "C" __declspec(dllimport) int __stdcall K32GetProcessMemoryInfo(void* process, void* counters,
                                                                      unsigned long cb);
#endif

namespace Vulkan {
namespace {
u64 ResidentMiB() {
#ifdef _WIN32
    struct ProcessMemoryCounters {
        unsigned long cb, page_fault_count;
        size_t peak_working_set, working_set, quota_peak_paged, quota_paged, quota_peak_nonpaged,
            quota_nonpaged, pagefile, peak_pagefile;
    } pmc{};
    pmc.cb = sizeof(pmc);
    return K32GetProcessMemoryInfo(reinterpret_cast<void*>(-1), &pmc, sizeof(pmc))
               ? pmc.working_set >> 20
               : 0;
#else
    unsigned long long size_pages = 0, rss_pages = 0;
    if (FILE* statm = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(statm, "%llu %llu", &size_pages, &rss_pages) != 2) {
            rss_pages = 0;
        }
        std::fclose(statm);
    }
    return u64(rss_pages) * u64(sysconf(_SC_PAGESIZE)) >> 20;
#endif
}

/// CPU time of the GPU command thread (OnSubmit can run on the recording thread instead).
u64 GpuThreadCpuUs() {
    const int clock = BbStats::gpu_thread_clock.load(std::memory_order_relaxed);
    timespec ts{};
    if (clock == -1 || clock_gettime(static_cast<clockid_t>(clock), &ts) != 0) {
        return 0;
    }
    return u64(ts.tv_sec) * 1000000 + u64(ts.tv_nsec) / 1000;
}

#ifndef _WIN32
// ufc3 (UFC-76): in the #47 log the FPS stepped down at minute 71 while the port's own work stayed
// the same (same draws, same GPU thread CPU time) and the GPU thread was preempted 2.5x as often:
// CPU time taken by something outside the game, or the APU clocking down. This line says which.
bool ReadFirstLine(const std::string& path, char* buffer, int size) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        return false;
    }
    const bool ok = std::fgets(buffer, size, file) != nullptr;
    std::fclose(file);
    return ok;
}

long long ReadNumber(const std::string& path) {
    char line[64];
    return ReadFirstLine(path, line, sizeof(line)) ? std::atoll(line) : -1;
}

/// All CPUs: busy and total jiffies from /proc/stat.
void MachineJiffies(u64& busy, u64& total) {
    unsigned long long v[8]{};
    busy = total = 0;
    if (FILE* stat = std::fopen("/proc/stat", "r")) {
        if (std::fscanf(stat, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
                        &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) {
            for (const auto x : v) {
                total += x;
            }
            busy = total - v[3] - v[4]; // without idle and iowait
        }
        std::fclose(stat);
    }
}

/// This process: user + system jiffies, and its involuntary context switches.
void ProcessCpu(u64& jiffies, u64& preempted) {
    jiffies = preempted = 0;
    char line[1024];
    if (ReadFirstLine("/proc/self/stat", line, sizeof(line))) {
        // Fields after the ")" of the command name: state is field 3, utime 14, stime 15.
        const char* p = std::strrchr(line, ')');
        unsigned long long utime = 0, stime = 0;
        if (p && std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                             &utime, &stime) == 2) {
            jiffies = utime + stime;
        }
    }
    if (FILE* status = std::fopen("/proc/self/status", "r")) {
        while (std::fgets(line, sizeof(line), status)) {
            unsigned long long n = 0;
            if (std::sscanf(line, "nonvoluntary_ctxt_switches: %llu", &n) == 1) {
                preempted = n;
            }
        }
        std::fclose(status);
    }
}

void ReportSystem(double window) {
    static u64 last_busy = 0, last_total = 0, last_self = 0, last_preempted = 0;
    static bool primed = false;
    u64 busy, total, self, preempted;
    MachineJiffies(busy, total);
    ProcessCpu(self, preempted);
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (!primed) {
        primed = true;
        last_busy = busy, last_total = total, last_self = self, last_preempted = preempted;
        return;
    }
    const double all = double(total - last_total);
    const double machine = all > 0 ? 100.0 * double(busy - last_busy) / all : 0.0;
    const double ours = all > 0 ? 100.0 * double(self - last_self) / all : 0.0;
    const double preempted_s = double(preempted - last_preempted) / window;
    last_busy = busy, last_total = total, last_self = self, last_preempted = preempted;

    double load = 0.0;
    if (FILE* file = std::fopen("/proc/loadavg", "r")) {
        if (std::fscanf(file, "%lf", &load) != 1) {
            load = 0.0;
        }
        std::fclose(file);
    }
    long long mhz_sum = 0, mhz_min = -1, mhz_max = -1, mhz_n = 0;
    for (long cpu = 0; cpu < cpus; ++cpu) {
        const long long khz = ReadNumber("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                                         "/cpufreq/scaling_cur_freq");
        if (khz > 0) {
            mhz_sum += khz / 1000, ++mhz_n;
            mhz_min = mhz_min < 0 ? khz / 1000 : std::min(mhz_min, khz / 1000);
            mhz_max = std::max(mhz_max, khz / 1000);
        }
    }
    // Sensors: amdgpu (APU/GPU temperature, power and shader clock), k10temp (CPU), Steam Deck.
    std::string sensors;
    if (DIR* dir = opendir("/sys/class/hwmon")) {
        while (const dirent* entry = readdir(dir)) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            const std::string base = std::string("/sys/class/hwmon/") + entry->d_name + "/";
            char name[64];
            if (!ReadFirstLine(base + "name", name, sizeof(name))) {
                continue;
            }
            name[std::strcspn(name, "\n")] = 0;
            const std::string n = name;
            if (n != "amdgpu" && n != "k10temp" && n != "jupiter" && n != "steamdeck_hwmon") {
                continue;
            }
            char part[128];
            const long long temp = ReadNumber(base + "temp1_input");
            std::snprintf(part, sizeof(part), "%s%s %.0f C", sensors.empty() ? "" : ", ",
                          name, temp >= 0 ? temp / 1000.0 : -1.0);
            sensors += part;
            if (n == "amdgpu") {
                long long power = ReadNumber(base + "power1_average");
                if (power < 0) {
                    power = ReadNumber(base + "power1_input");
                }
                const long long sclk = ReadNumber(base + "freq1_input");
                if (power >= 0) {
                    std::snprintf(part, sizeof(part), " %.1f W", power / 1e6);
                    sensors += part;
                }
                if (sclk > 0) {
                    std::snprintf(part, sizeof(part), " GPU %lld MHz", sclk / 1000000);
                    sensors += part;
                }
            }
        }
        closedir(dir);
    }
    std::string battery;
    if (DIR* dir = opendir("/sys/class/power_supply")) {
        while (const dirent* entry = readdir(dir)) {
            const std::string base = std::string("/sys/class/power_supply/") + entry->d_name + "/";
            char type[32], status[32];
            if (entry->d_name[0] == '.' || !ReadFirstLine(base + "type", type, sizeof(type)) ||
                std::strncmp(type, "Battery", 7) != 0) {
                continue;
            }
            if (!ReadFirstLine(base + "status", status, sizeof(status))) {
                status[0] = 0;
            }
            status[std::strcspn(status, "\n")] = 0;
            battery = "; battery " + std::to_string(ReadNumber(base + "capacity")) + "% " + status;
            break;
        }
        closedir(dir);
    }
    std::printf("System (%.0f s): CPU busy %.0f%% of %ld threads, this game %.0f%%, others %.0f%%; "
                "load %.1f; game preempted %.0f/s; CPU %lld MHz (%lld-%lld); %s%s\n",
                window, machine, cpus, ours, std::max(0.0, machine - ours), load, preempted_s,
                mhz_n ? mhz_sum / mhz_n : -1, mhz_min, mhz_max,
                sensors.empty() ? "no sensors" : sensors.c_str(), battery.c_str());
}
#endif
} // namespace

void Rasterizer::ReportGrowth() {
    using Clock = std::chrono::steady_clock;
    static const int seconds = [] {
        const char* env = std::getenv("BB_GROWTH_SECONDS");
        return env ? std::atoi(env) : 60;
    }();
    if (seconds <= 0) {
        return;
    }
    static Clock::time_point last{};
    static u64 last_frames = 0, last_draws = 0, last_cpu_us = 0;
    const auto now = Clock::now();
    if (last != Clock::time_point{} && now - last < std::chrono::seconds(seconds)) {
        return;
    }
    const u64 frames = BbStats::frame_number.load(std::memory_order_relaxed);
    const u64 draws = BbStats::draws.load(std::memory_order_relaxed);
    const u64 cpu_us = GpuThreadCpuUs();
    const bool first = last == Clock::time_point{};
    const double window = first ? 0.0 : std::chrono::duration<double>(now - last).count();
    const u64 window_frames = frames - last_frames, window_draws = draws - last_draws;
    last = now;
    last_frames = frames;
    last_draws = draws;
    const u64 window_cpu_us = cpu_us - last_cpu_us;
    last_cpu_us = cpu_us;
    if (first) {
#ifndef _WIN32
        ReportSystem(0.0); // primes the CPU counters
#endif
        return; // the window starts here
    }

    const auto t = texture_cache.CountGrowth();
    const auto b = buffer_cache.CountGrowth();
    const auto p = pipeline_cache.CountGrowth();
    u64 vma_blocks = 0, vma_used = 0;
    VmaDeviceUsage(vma_blocks, vma_used);
    const u64 device = BbStats::device_alloc_bytes.load() - BbStats::device_free_bytes.load();
    std::printf(
        "Growth (%.0f s): %.1f FPS, GPU thread %.1f us/draw, %.0f draws/frame; images %llu "
        "(%llu MiB; %llu idle 60 s, %llu CPU-overwritten, %llu GPU-written, %llu tracked), "
        "%llu stale freed (BB_GC_STALE_SECONDS); lookup pages %llu with %llu entries, longest "
        "%llu; views %llu, samplers %llu; collector usage %llu of %llu MiB; pipelines %llu "
        "graphics, %llu compute, %llu programs, %llu compiling; buffer cache %llu arenas, "
        "intervals: %llu resident, %llu in place, %llu GPU-modified, %llu GPU-written, %llu "
        "assets; %llu CPU-written blocks, %llu GPU write ranges, %llu VRAM chunks; scene proxies "
        "%zu; Vulkan memory %llu MiB (VMA %llu MiB, images %llu MiB); RSS %llu MiB\n",
        window, window_frames / window,
        window_draws ? double(window_cpu_us) / double(window_draws) : 0.0,
        window_frames ? double(window_draws) / double(window_frames) : 0.0,
        (unsigned long long)t.images, (unsigned long long)t.image_mib,
        (unsigned long long)t.idle_images, (unsigned long long)t.dirty_images,
        (unsigned long long)t.gpu_images, (unsigned long long)t.tracked_images,
        (unsigned long long)t.stale_freed, (unsigned long long)t.pages,
        (unsigned long long)t.page_entries, (unsigned long long)t.longest_page,
        (unsigned long long)t.views, (unsigned long long)t.samplers,
        (unsigned long long)t.used_mib, (unsigned long long)t.trigger_mib,
        (unsigned long long)p[1], (unsigned long long)p[2], (unsigned long long)p[0],
        (unsigned long long)p[3], (unsigned long long)b.arenas,
        (unsigned long long)b.resident_intervals, (unsigned long long)b.in_place_intervals,
        (unsigned long long)b.gpu_ranges, (unsigned long long)b.gpu_written_intervals,
        (unsigned long long)b.asset_intervals, (unsigned long long)b.cpu_write_blocks,
        (unsigned long long)b.gpu_write_ranges, (unsigned long long)b.residency_chunks,
        scene_targets ? scene_targets->Count() : size_t{0}, (unsigned long long)(device >> 20),
        (unsigned long long)(vma_blocks >> 20),
        (unsigned long long)(BbStats::vk_image_bytes.load() >> 20),
        (unsigned long long)ResidentMiB());
#ifndef _WIN32
    ReportSystem(window);
#endif
    std::fflush(stdout);
}
} // namespace Vulkan
