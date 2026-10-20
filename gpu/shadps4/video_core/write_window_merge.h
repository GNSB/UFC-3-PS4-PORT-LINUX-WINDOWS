// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// ufc3 (UFC-48, BB_WRITE_WINDOW): who owns a byte of a page the CPU wrote through a write
// window. Header-only (tests/test_write_window_merge.cpp).
//
// A window lets the CPU write a page with GPU-written bytes (gpu_modified_ranges: VRAM holds
// their value, the guest copy is stale) without a readback. Two ways lost those writes, among
// them the game allocator's free-list links (UFC-2: fault at +0x1262a2a on a -1 head):
// - closing the window dropped the changed words that touched GPU bytes, and left the bytes
//   GPU-owned: the next readback of the page copied VRAM's old value over the CPU's;
// - a readback while the window was open (or in flight across its close) wrote VRAM's value over
//   bytes the CPU had just written.
// Every GPU write a window can overlap was submitted before the window opened (each submission
// closes all windows first): a byte the CPU changed in the window is the newer value. So the
// close hands such words to VRAM and takes them out of the GPU ranges, and a readback writes only
// the bytes nobody changed since it began, nor since the window opened (by 8-byte granules).

#include <cstdint>

namespace VideoCore::WindowMerge {

/// Ownership goes by aligned 8-byte granules: a store that changed some bytes of a pointer may
/// leave others as they were (the stale guest copy can match), and those are the CPU's too.
/// DecideWriteStep must reject windows whose covered granules include GPU bytes, even
/// when the decoded store itself misses them (CoverStore).
constexpr std::uint64_t Granule = 8;

struct StoreRange {
    std::uint64_t begin, end;
};

/// The ownership units a decoded store can hand to VRAM when its window closes. The
/// overlap decision must include unchanged neighbours in those units too (UFC-61).
constexpr StoreRange CoverStore(std::uint64_t begin, std::uint64_t end) {
    return {begin / Granule * Granule, (end + Granule - 1) / Granule * Granule};
}

/// The changed granules of a page between two copies, as runs: run(offset, length). Offsets are
/// page-relative (page-aligned memory): lengths are multiples of 4 (vkCmdUpdateBuffer).
template <typename Run>
void ForEachChangedRun(const std::uint8_t* before, const std::uint8_t* after, std::uint64_t size,
                       Run&& run) {
    const auto differs = [&](std::uint64_t at) {
        for (std::uint64_t i = at; i < at + Granule && i < size; ++i) {
            if (before[i] != after[i]) {
                return true;
            }
        }
        return false;
    };
    for (std::uint64_t at = 0; at < size;) {
        if (!differs(at)) {
            at += Granule;
            continue;
        }
        std::uint64_t end = at + Granule;
        while (end < size && differs(end)) {
            end += Granule;
        }
        run(at, (end < size ? end : size) - at);
        at = end;
    }
}

/// The bytes of a readback payload for guest memory at `address`, as runs: run(offset, length).
/// A granule is written only when the guest still holds there what it held when the download
/// began (`at_start`) and, under an open window, what it held when the window opened (`window`);
/// either may be null. Returns the bytes kept (the CPU's).
/// ufc4: `keep(granule_address)` true also keeps a granule (the bytes a libc copy wrote in the
/// window: the CPU's whatever their value).
template <typename Keep, typename Run>
std::uint64_t ForEachWritableRunKeeping(std::uint64_t address, const std::uint8_t* current,
                                        const std::uint8_t* at_start, const std::uint8_t* window,
                                        std::uint64_t size, Keep&& keep, Run&& run) {
    const auto granule_end = [&](std::uint64_t at) {
        const std::uint64_t end = ((address + at) / Granule + 1) * Granule - address;
        return end < size ? end : size;
    };
    const auto writable = [&](std::uint64_t at) {
        if (keep((address + at) / Granule * Granule)) {
            return false;
        }
        for (std::uint64_t i = at; i < granule_end(at); ++i) {
            if ((at_start && current[i] != at_start[i]) || (window && current[i] != window[i])) {
                return false;
            }
        }
        return true;
    };
    std::uint64_t kept = 0;
    for (std::uint64_t at = 0; at < size;) {
        if (!writable(at)) {
            kept += granule_end(at) - at;
            at = granule_end(at);
            continue;
        }
        std::uint64_t end = granule_end(at);
        while (end < size && writable(end)) {
            end = granule_end(end);
        }
        run(at, end - at);
        at = end;
    }
    return kept;
}

template <typename Run>
std::uint64_t ForEachWritableRun(std::uint64_t address, const std::uint8_t* current,
                                 const std::uint8_t* at_start, const std::uint8_t* window,
                                 std::uint64_t size, Run&& run) {
    return ForEachWritableRunKeeping(
        address, current, at_start, window, size, [](std::uint64_t) { return false; },
        static_cast<Run&&>(run));
}

} // namespace VideoCore::WindowMerge
