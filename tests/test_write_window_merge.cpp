// SPDX-License-Identifier: GPL-2.0-or-later
// UFC-48: write windows keep the CPU's writes over GPU-written bytes (WindowMerge). The case of
// the Windows log UFC2-diag-logfolder-win-20261016_160516: on page 0x100077e000 the game's
// allocator (+0x1262c9f) linked 32-byte free nodes (0x...ec20 -> 0x...ec40) through a window over
// bytes the GPU had written; the close dropped those words and left them GPU-owned, and the next
// readback of the page put VRAM's old bytes back (a -1 free-list head: fault at +0x1262a2a).
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "gpu/shadps4/video_core/write_window_merge.h"

using namespace VideoCore::WindowMerge;

namespace {
constexpr std::uint64_t Page = 4096;
using Bytes = std::array<std::uint8_t, Page>;

// One page as the buffer cache sees it: guest memory, VRAM, the GPU-modified bytes.
struct Model {
    Bytes guest{}, vram{};
    std::array<bool, Page> gpu{};
    Bytes before{}; // the window's snapshot (PageManager WriteWindow::before)
    bool window = false;

    void GpuWrites(std::uint64_t at, std::uint64_t size, std::uint8_t value) {
        std::memset(vram.data() + at, value, size);
        for (std::uint64_t i = at; i < at + size; ++i) {
            gpu[i] = true;
        }
    }
    void OpenWindow() {
        before = guest;
        window = true;
    }
    void CpuWrite(std::uint64_t at, std::uint64_t value) {
        assert(window); // no fault: the page is open
        std::memcpy(guest.data() + at, &value, sizeof(value));
    }
    std::uint64_t Read(std::uint64_t at) const {
        std::uint64_t value = 0;
        std::memcpy(&value, guest.data() + at, sizeof(value));
        return value;
    }
    // CloseWindows + NoteSteppedWrite + ProcessLateWrites; fixed = UFC-48, else as before it.
    void CloseWindow(bool fixed) {
        window = false;
        if (fixed) {
            ForEachChangedRun(before.data(), guest.data(), Page,
                              [&](std::uint64_t at, std::uint64_t n) {
                                  for (std::uint64_t i = at; i < at + n; ++i) {
                                      gpu[i] = false;
                                      vram[i] = guest[i];
                                  }
                              });
            return;
        }
        // Before UFC-48: changed 4-byte words, those with GPU bytes dropped (still GPU-owned).
        for (std::uint64_t w = 0; w < Page; w += 4) {
            if (std::memcmp(before.data() + w, guest.data() + w, 4) == 0 ||
                gpu[w] || gpu[w + 1] || gpu[w + 2] || gpu[w + 3]) {
                continue;
            }
            std::memcpy(vram.data() + w, guest.data() + w, 4);
        }
    }
    // DownloadMemory: the GPU-modified bytes, captured now (ReadbackSnapshot), written later.
    struct Download {
        std::uint64_t at = 0, size = 0;
        Bytes payload{}, start{};
    };
    Download BeginReadback() {
        Download d; // one GPU-written run in these cases
        for (std::uint64_t i = 0; i < Page; ++i) {
            if (gpu[i]) {
                d.at = d.size ? d.at : i;
                d.size = i + 1 - d.at;
                gpu[i] = false;
            }
        }
        d.payload = vram;
        d.start = window ? before : guest;
        return d;
    }
    std::uint64_t FinishReadback(const Download& d, bool fixed) {
        if (!fixed) {
            std::memcpy(guest.data() + d.at, d.payload.data() + d.at, d.size);
            return 0;
        }
        return ForEachWritableRun(
            d.at, guest.data() + d.at, d.start.data() + d.at,
            window ? before.data() + d.at : nullptr, d.size, [&](std::uint64_t at, std::uint64_t n) {
                std::memcpy(guest.data() + d.at + at, d.payload.data() + d.at + at, n);
                if (window) {
                    std::memcpy(before.data() + d.at + at, d.payload.data() + d.at + at, n);
                }
            });
    }
};

constexpr std::uint64_t Node = 0xc20, Next = 0x100077ec40;

using Runs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

void ChangedRuns() {
    Bytes a{}, b{};
    b[5] = 1;   // granule 0
    b[8] = 1;   // granule 8: one run 0..16
    b[100] = 1; // granule 96
    Runs runs;
    ForEachChangedRun(a.data(), b.data(), Page,
                      [&](std::uint64_t at, std::uint64_t n) { runs.emplace_back(at, n); });
    assert((runs == Runs{{0, 16}, {96, 8}}));
}

void WritableRuns() {
    // 20 bytes at 0x...1004: granules [0,4) [4,12) [12,20) of the range.
    std::uint8_t current[20], start[20], window[20];
    for (int i = 0; i < 20; ++i) {
        current[i] = start[i] = window[i] = std::uint8_t(i);
    }
    current[6] = 99; // changed since the download began (a CPU write after it): granule [4,12)
    start[6] = 6;
    window[6] = 99;
    window[13] = 0; // changed in the window before the download: granule [12,20)
    current[13] = start[13] = 13;
    Runs runs;
    const auto kept = ForEachWritableRun(0x1001004, current, start, window, 20,
                                         [&](auto at, auto n) { runs.emplace_back(at, n); });
    assert(kept == 16);
    assert((runs == Runs{{0, 4}}));
    runs.clear();
    assert(ForEachWritableRun(0x1001004, current, nullptr, nullptr, 20,
                              [&](auto at, auto n) { runs.emplace_back(at, n); }) == 0);
    assert((runs == Runs{{0, 20}}));
}

// The free-list link written through a window over GPU bytes, then a readback of the page.
void LinkSurvivesClose(bool fixed) {
    Model m;
    m.GpuWrites(0xc00, 0x100, 0xff);
    m.OpenWindow();
    m.CpuWrite(Node, Next);
    m.CloseWindow(fixed);
    if (fixed) {
        assert(!m.gpu[Node] && m.vram[Node] == 0x40); // the link went to VRAM, CPU-owned
    }
    m.FinishReadback(m.BeginReadback(), fixed);
    if (fixed) {
        assert(m.Read(Node) == Next);
        assert(m.guest[0xc00] == 0xff); // the GPU's other bytes still read back
    } else {
        assert(m.Read(Node) == ~0ull); // the bug: the head the allocator then follows
    }
}

// A readback while the window is open, the CPU writing while it waits for the GPU.
void LinkSurvivesReadbackInWindow() {
    Model m;
    m.GpuWrites(0xc00, 0x100, 0xff);
    m.OpenWindow();
    const auto download = m.BeginReadback();
    m.CpuWrite(Node, Next);
    assert(m.FinishReadback(download, true) == 8);
    assert(m.Read(Node) == Next);
    assert(m.guest[0xc00] == 0xff);
    // The readback's bytes are no CPU write: the close hands only the link to VRAM.
    std::uint64_t handed = 0;
    ForEachChangedRun(m.before.data(), m.guest.data(), Page,
                      [&](std::uint64_t, std::uint64_t n) { handed += n; });
    assert(handed == 8);
    m.CloseWindow(true);
    assert(m.vram[Node] == m.guest[Node]);
}

// The CPU writes, the readback begins, the window closes, then the readback writes.
void LinkSurvivesReadbackAcrossClose() {
    Model m;
    m.GpuWrites(0xc00, 0x100, 0xff);
    m.OpenWindow();
    m.CpuWrite(Node, Next);
    const auto download = m.BeginReadback();
    m.CloseWindow(true);
    m.FinishReadback(download, true);
    assert(m.Read(Node) == Next);
    assert(m.guest[0xc08] == 0xff);
}
} // namespace

int main() {
    ChangedRuns();
    WritableRuns();
    LinkSurvivesClose(false);
    LinkSurvivesClose(true);
    LinkSurvivesReadbackInWindow();
    LinkSurvivesReadbackAcrossClose();
    std::puts("write window merge: ok");
    return 0;
}
