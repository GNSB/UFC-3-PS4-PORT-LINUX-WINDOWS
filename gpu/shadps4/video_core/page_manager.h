// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include "common/alignment.h"
#include "common/types.h"
#include "video_core/buffer_cache//region_definitions.h"

namespace Vulkan {
class Rasterizer;
}

namespace VideoCore {

struct UffdImpl;
struct SignalImpl;

/// ufc3: Filled by the synchronous GPU command serving a CPU read fault. The handler owns this
/// object until ReadMemory returns; ordinary reads and write faults pass no diagnostics.
struct ReadFaultDownload {
    u64 write_tick = 0, current_tick = 0, bytes = 0, downloads = 0;
    u64 page_gpu_bytes = 0, gpu_distance = ~u64{0};
    double wait_ms = 0;
};

class PageManager {
    // PAGE_SIZE and PAGE_BITS conflicts with machine/param.h definitions on freebsd!
    // Use the same page size as the tracker.
    static constexpr size_t PM_PAGE_BITS = TRACKER_PAGE_BITS;
    static constexpr size_t PM_PAGE_SIZE = TRACKER_BYTES_PER_PAGE;

    // Keep the lock granularity the same as region granularity. (since each regions has
    // itself a lock)
    static constexpr size_t PAGES_PER_LOCK = NUM_PAGES_PER_REGION;

public:
    explicit PageManager(Vulkan::Rasterizer* rasterizer);
    ~PageManager();

    /// bbport: the guest code behind write faults on tracked pages over the last window (frame
    /// stats): which game functions write GPU memory, to replace tracking by explicit hooks.
    static void ReportFaultSites();
    static ReadFaultDownload* CurrentReadFaultDownload();
    /// ufc3: a write fault on this thread became a readback (reason: 0 host copy, 1 page over
    /// the step limit, 2 near GPU bytes, 3 other); reported with the fault sites.
    static void NoteReadbackSite(u32 reason, VAddr address, u64 gpu_bytes);
    /// bbport: the write fault being handled on this thread hit an image (diagnostics: which guest
    /// code writes textures, BB_WRITE_TRACKING=1).
    static void NoteImageFault(VAddr image_address, u64 image_size, u32 width, u32 height,
                               u32 format, u32 tiling);

    /// ufc3 (BB_WRITE_STEP): the single-step trap after a write on an opened GPU-written page;
    /// false when this thread is not stepping one (the trap is not ours).
    static bool OnStepTrap(void* context);

    /// bbport (ufc3, BB_WRITE_WINDOW): modes 1/2 keep GPU-written pages open for writes beside GPU bytes
    /// for the CPU until the game's next submission, then their changed words go to VRAM.
    static bool WriteWindowsEnabled();
    /// bbport (ufc3, BB_WRITE_WINDOW): mode 2 requires a known exact write range to open a window.
    static bool WriteWindowsExactOnly();
    /// Whether the write fault being handled on this thread comes from the game's code.
    static bool FaultFromGuest();
    /// bbport (ufc3): the bytes the guest instruction of this thread's write fault writes, when it was decoded
    /// as one plain store (false otherwise).
    static bool GuestStoreRange(VAddr* begin, VAddr* end);
    /// DecideWriteStep: open a window for this fault instead of single-stepping it.
    static void RequestWindow();
    /// Closes every open window (the submitting thread, before the GPU side sees the submission).
    static void CloseWriteWindows();

    /// UFC-48: faults on pages already open in a window since the last call (protected again
    /// while open; each such fault re-opens the page instead of retrying forever).
    static u64 TakeWindowReprotects();
    /// UFC-48: whether the write fault being handled on this thread comes from a site (instruction
    /// and caller) barred from windows: one of its windows changed GPU-owned bytes. A window
    /// cannot see a CPU write that leaves the stale guest value over GPU bytes (VRAM keeps the
    /// GPU's); a site that writes over GPU data reads the page back first, as without windows.
    static bool WindowSiteBarred();
    /// UFC-48: GPU bytes left GPU-owned, since the last call, on pages where a window's CPU writes
    /// took over others (the bytes a window cannot tell were rewritten unchanged).
    static u64 TakeWindowAmbiguousBytes();
    /// ufc4: whether the write fault being handled on this thread is inside the libc copy
    /// (guest_memcpy, guest_memset, guest_memmove) running on it: a window then knows which
    /// bytes the CPU wrote, and the site's barring does not apply to them.
    static bool FaultInHostCopy(VAddr addr);
    /// ufc4: windows that knew a host copy's bytes, and those bytes, since the last call.
    static void TakeHostCopyStats(u64* windows, u64* bytes);

    /// ufc3 (UFC-48): what a buffer readback beginning now may write over: the guest bytes, and
    /// on a page open in a write window the bytes as the window opened (the CPU's since are kept
    /// even if the window closes before the readback writes).
    static void ReadbackSnapshot(VAddr addr, u8* out, u64 size);
    /// ufc3 (UFC-48): writes a buffer readback's payload to guest memory through `write`, except
    /// the bytes the CPU changed since the download began (`at_start`: ReadbackSnapshot then,
    /// or null) or since their page's write window opened. Returns the bytes kept.
    using ReadbackWriter = void (*)(void* context, VAddr addr, const u8* data, u64 size);
    static u64 WriteReadback(VAddr addr, const u8* payload, const u8* at_start, u64 size,
                             ReadbackWriter write, void* context);

    /// BB_WRITE_WINDOW_DIAG=1: audit a readback (after it wrote) or a skipped upload against
    /// active/recent window snapshots. Observational only; a matching page does not establish
    /// CPU ownership.
    static void DiagnoseWindowWrite(VAddr address, std::span<const u8> data, bool upload_skip);
    /// BB_WRITE_WINDOW_DIAG=1: a binding the GPU writes covers [address, +size) (after the
    /// windows over it closed: their CPU bytes there are superseded, not lost to a readback).
    static void DiagnoseGpuWrite(VAddr address, u64 size);
    /// BB_WRITE_WINDOW_DIAG=1 (UFC-48): a binding the GPU reads (or writes) covers
    /// [address, +size): notes whether it covers GPU-owned bytes a closed window left as they
    /// were (the CPU may have rewritten them with the stale guest value: VRAM's is used).
    static void DiagnoseGpuRead(VAddr address, u64 size, bool written);
    /// The compute shader whose bindings this thread is about to obtain (0: none or a draw).
    static void SetDiagShader(u64 hash);
    /// GPU hang report: the recent DiagnoseGpuRead hits of the stuck shader.
    static void ReportWindowHazards(u64 hash);
    static void DumpWindowFault(void* context);

    /// Register a range of mapped gpu memory.
    void OnGpuMap(VAddr address, size_t size);

    /// Unregister a range of gpu memory that was unmapped.
    void OnGpuUnmap(VAddr address, size_t size);

    /// Updates watches in the pages touching the specified region.
    template <bool track>
    void UpdatePageWatchers(VAddr addr, u64 size) const;

    /// ufc3 (UFC-91, BB_DEFERRED_READBACK): pending image readbacks guard the pages touching
    /// [addr, +size) against writes (`writes`) and reads (`reads`): +1 adds a guard, -1 drops
    /// one, 0 leaves them. A guarded access faults into Rasterizer::ResolveReadbackFault.
    void WatchReadback(VAddr addr, u64 size, s32 writes, s32 reads) const;

    /// Updates watches in the pages touching the specified region using a mask.
    template <bool track, bool is_read = false>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const;

    /// Returns page aligned address.
    static constexpr VAddr GetPageAddr(VAddr addr) {
        return Common::AlignDown(addr, PM_PAGE_SIZE);
    }

    /// Returns address of the next page.
    static constexpr VAddr GetNextPageAddr(VAddr addr) {
        return Common::AlignUp(addr + 1, PM_PAGE_SIZE);
    }

private:
    friend struct UffdImpl;
    friend struct SignalImpl;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore
