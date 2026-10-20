// SPDX-License-Identifier: GPL-2.0-or-later
// ufc3 (UFC-65): one line every BB_GROWTH_SECONDS (60) with what the port keeps over a session.
// FPS fell by 5-6 over an hour of quick fights on a Steam Deck (GitHub #24) and after arena
// changes on a Radeon 680M (#32), without a log saying why; the first and last lines of a long
// session show which of these grew while the frame rate went down.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
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
    std::fflush(stdout);
}
} // namespace Vulkan
