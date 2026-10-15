#include <vector>
#include <memory>
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <boost/container/small_vector.hpp>
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <deque>
#ifndef _WIN32
#include <sys/uio.h>
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <string>
#include <fmt/format.h>
#ifndef _WIN32
#include <ucontext.h>
#endif
#include <unistd.h>
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#endif

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unistd.h>
#include <sys/ioctl.h>
#ifndef _WIN32
#include <sys/syscall.h>
#endif
#else
#include "common/spin_lock.h"
#endif

extern "C" void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch);

extern "C" void runtime_memory_read_backing(uintptr_t address, void* data, uint64_t size);

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

namespace {
/// bbport: write fault sites (guest code), an open-addressing table keyed by the instruction.
struct FaultSite {
    std::atomic<u64> rip{0};
    std::atomic<u64> caller{0};
    std::atomic<u64> count{0};
    std::atomic<u64> last_address{0};
};
std::array<FaultSite, 512> fault_sites;
std::atomic<u64> fault_sites_dropped{0};
constexpr u64 GuestImage = 0x800000000ull, GuestImageEnd = 0x810000000ull;
constexpr u64 HostSite = 1ull << 63;
constexpr u64 ReadSite = 1ull << 62; ///< a read fault (BB_READBACKS=2 protects GPU data from reads)

/// "+offset" for guest code, the symbol for host code.
std::string SiteName(u64 key) {
    const char* access = key & ReadSite ? "read:" : "";
    key &= ~ReadSite;
    if (!(key & HostSite)) {
        return fmt::format("{}+{:#x}", access, key);
    }
    const u64 address = key & ~HostSite;
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_sname) {
        return fmt::format("{}host:{}+{:#x}", access, info.dli_sname, address - u64(info.dli_saddr));
    }
    if (info.dli_fname) {
        return fmt::format("{}host:{}+{:#x}", access, info.dli_fname, address - u64(info.dli_fbase));
    }
    return fmt::format("{}host:{:#x}", access, address);
}

/// The write fault this thread is handling (guest offsets; 0: none).
thread_local u64 current_fault_rip = 0, current_fault_caller = 0;

/// Guest code that wrote image (texture) memory: keyed by instruction and image.
struct ImageFaultSite {
    std::atomic<u64> key{0}; // a mix of the site and the image address
    std::atomic<u64> rip{0}, caller{0}, count{0}, image{0}, size{0};
    std::atomic<u32> width{0}, height{0}, format{0}, tiling{0};
};
std::array<ImageFaultSite, 256> image_fault_sites;

/// ufc3: write faults that ended in a readback, keyed by site and reason.
struct ReadbackSite {
    std::atomic<u64> key{0}; // site key | reason << 56
    std::atomic<u64> caller{0}, count{0}, last_address{0}, gpu_bytes{0};
};
std::array<ReadbackSite, 256> readback_sites;

void NoteFaultSite(void* context, VAddr address) {
    current_fault_rip = 0;
    const u64 rip = Common::GetFaultRegister(context, Common::FaultRegister::Rip);
    const bool guest_code = rip >= GuestImage && rip < GuestImageEnd;
    u64 caller = 0;
    if (guest_code) {
        // The caller: [rbp + 8] when the guest code keeps frames (its memcpy-like leaves do not).
        u64 saved[2] = {};
        iovec local{saved, sizeof(saved)}, remote{reinterpret_cast<void*>(Common::GetFaultRegister(context, Common::FaultRegister::Rbp)), sizeof(saved)};
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(sizeof(saved)) &&
            saved[1] >= GuestImage && saved[1] < GuestImageEnd) {
            caller = saved[1];
        }
    } else {
        // Host code (a libc import the port runs natively, the runtime): the first guest return
        // address on the stack is its guest caller.
        std::array<u64, 64> stack{};
        iovec local{stack.data(), sizeof(stack)},
            remote{reinterpret_cast<void*>(Common::GetFaultRegister(context, Common::FaultRegister::Rsp)), sizeof(stack)};
        const ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
        for (ssize_t i = 0; i < got / 8; ++i) {
            if (stack[i] >= GuestImage && stack[i] < GuestImageEnd) {
                caller = stack[i];
                break;
            }
        }
    }
    // Guest code: its offset in the image; host code: its address with HostSite set.
    const u64 key = (guest_code ? rip - GuestImage : rip | HostSite) |
                    (Common::IsWriteError(context) ? 0 : ReadSite);
    current_fault_rip = key;
    current_fault_caller = caller ? caller - GuestImage : 0;
    for (u64 i = 0, slot = (key * 0x9E3779B97F4A7C15ull) >> 55; i < fault_sites.size(); ++i) {
        auto& site = fault_sites[(slot + i) % fault_sites.size()];
        u64 expected = 0;
        if (site.rip.load(std::memory_order_relaxed) == key ||
            site.rip.compare_exchange_strong(expected, key)) {
            site.caller.store(caller ? caller - GuestImage : 0, std::memory_order_relaxed);
            site.last_address.store(address, std::memory_order_relaxed);
            site.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    fault_sites_dropped.fetch_add(1, std::memory_order_relaxed);
}
} // namespace

static void ReportImageFaultSites() {
    bool any = false;
    for (auto& site : image_fault_sites) {
        const u64 count = site.count.exchange(0, std::memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        std::printf("%s %s (from +%#llx) %llu into image %#llx size %llu %ux%u fmt %u tile %u",
                    any ? ";" : "Image write sites (guest code -> texture, this window):",
                    SiteName(site.rip.load()).c_str(), (unsigned long long)site.caller.load(),
                    (unsigned long long)count, (unsigned long long)site.image.load(),
                    (unsigned long long)site.size.load(), site.width.load(), site.height.load(),
                    site.format.load(), site.tiling.load());
        any = true;
    }
    if (any) {
        std::printf("\n");
    }
}

void PageManager::NoteImageFault(VAddr image_address, u64 image_size, u32 width, u32 height,
                                 u32 format, u32 tiling) {
    if (current_fault_rip == 0) {
        return;
    }
    const u64 key = (current_fault_rip * 0x9E3779B97F4A7C15ull) ^ image_address;
    for (u64 i = 0, slot = (key * 0x9E3779B97F4A7C15ull) >> 56; i < image_fault_sites.size(); ++i) {
        auto& site = image_fault_sites[(slot + i) % image_fault_sites.size()];
        u64 expected = 0;
        if (site.key.load(std::memory_order_relaxed) == key ||
            site.key.compare_exchange_strong(expected, key)) {
            site.rip.store(current_fault_rip, std::memory_order_relaxed);
            site.caller.store(current_fault_caller, std::memory_order_relaxed);
            site.image.store(image_address, std::memory_order_relaxed);
            site.size.store(image_size, std::memory_order_relaxed);
            site.width.store(width, std::memory_order_relaxed);
            site.height.store(height, std::memory_order_relaxed);
            site.format.store(format, std::memory_order_relaxed);
            site.tiling.store(tiling, std::memory_order_relaxed);
            site.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void PageManager::NoteReadbackSite(u32 reason, VAddr address, u64 gpu_bytes) {
    const u64 key = (current_fault_rip ? current_fault_rip : 1) | (u64(reason & 3) << 56);
    for (u64 i = 0, slot = (key * 0x9E3779B97F4A7C15ull) >> 56; i < readback_sites.size(); ++i) {
        auto& site = readback_sites[(slot + i) % readback_sites.size()];
        u64 expected = 0;
        if (site.key.load(std::memory_order_relaxed) == key ||
            site.key.compare_exchange_strong(expected, key)) {
            site.caller.store(current_fault_caller, std::memory_order_relaxed);
            site.last_address.store(address, std::memory_order_relaxed);
            site.gpu_bytes.store(gpu_bytes, std::memory_order_relaxed);
            site.count.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

namespace {
void ReportReadbackSites() {
    struct Row {
        u64 key, caller, count, address, gpu_bytes;
    };
    std::array<Row, 256> rows{};
    std::size_t n = 0;
    u64 total = 0;
    for (auto& site : readback_sites) {
        const u64 count = site.count.exchange(0, std::memory_order_relaxed);
        if (count != 0) {
            rows[n++] = {site.key.load(), site.caller.load(), count, site.last_address.load(),
                         site.gpu_bytes.load()};
            total += count;
        }
    }
    if (total == 0) {
        return;
    }
    std::sort(rows.begin(), rows.begin() + n, [](const Row& a, const Row& b) { return a.count > b.count; });
    static constexpr const char* Reasons[] = {"host copy", "page > max", "near GPU", "other"};
    std::printf("Readback sites (write faults, this window): %llu", (unsigned long long)total);
    for (std::size_t i = 0; i < std::min<std::size_t>(n, 10); ++i) {
        const u64 site = rows[i].key & ~(u64{3} << 56);
        std::printf("%s %s (from +%#llx) [%s, %llu GPU B] %llu at %#llx", i ? ";" : ":",
                    site == 1 ? "?" : SiteName(site).c_str(), (unsigned long long)rows[i].caller,
                    Reasons[(rows[i].key >> 56) & 3], (unsigned long long)rows[i].gpu_bytes,
                    (unsigned long long)rows[i].count, (unsigned long long)rows[i].address);
    }
    std::printf("\n");
}
} // namespace

void PageManager::ReportFaultSites() {
    ReportReadbackSites();
    struct Row {
        u64 rip, caller, count, address;
    };
    std::array<Row, 512> rows{};
    std::size_t n = 0;
    u64 total = 0;
    for (auto& site : fault_sites) {
        const u64 count = site.count.exchange(0, std::memory_order_relaxed);
        if (count != 0) {
            rows[n++] = {site.rip.load(), site.caller.load(), count, site.last_address.load()};
            total += count;
        }
    }
    if (total == 0) {
        return;
    }
    ReportImageFaultSites();
    std::sort(rows.begin(), rows.begin() + n, [](const Row& a, const Row& b) { return a.count > b.count; });
    std::printf("Write fault sites (guest code, this window): %llu faults", (unsigned long long)total);
    for (std::size_t i = 0; i < std::min<std::size_t>(n, 12); ++i) {
        std::printf("%s %s (from +%#llx) %llu at %#llx", i ? ";" : ":",
                    SiteName(rows[i].rip).c_str(), (unsigned long long)rows[i].caller,
                    (unsigned long long)rows[i].count, (unsigned long long)rows[i].address);
    }
    std::printf("\n");
}

namespace {
// ufc3 (BB_WRITE_STEP): see PageManager::Impl::ArmStep.
struct StepSlot {
    std::atomic<u64> tid{0};
    u32 count = 0;
    VAddr pages[2]{};
    std::array<u8, TRACKER_BYTES_PER_PAGE> before[2];
};
std::array<StepSlot, 64> step_slots;

// ufc3 (BB_WRITE_WINDOW): open pages and their contents when opened.
struct WriteWindow {
    VAddr page;
    std::array<u8, TRACKER_BYTES_PER_PAGE> before;
    u64 audit_id = 0;
};
std::mutex windows_mutex;
std::vector<std::unique_ptr<WriteWindow>> open_windows;
std::atomic<u32> open_window_count{0};
thread_local bool want_window = false, fault_from_guest = false;
constexpr std::size_t MaxWindows = 512;

bool WindowAuditEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_WRITE_WINDOW_DIAG");
        const bool on = env && env[0] == '1';
        if (on) {
            std::printf("Write window audit: on; 1024 active/recent snapshots, at most 64 reports "
                        "per event kind; observations do not establish CPU/GPU ownership "
                        "(BB_WRITE_WINDOW_DIAG)\n");
        }
        return on;
    }();
    return enabled;
}

struct WindowAuditRecord {
    u64 id, rip, caller, tid;
    VAddr page;
    enum Phase { Open, Closing, Closed } phase = Open;
    // Baseline at open, replaced by the final snapshot at close.
    std::array<u8, TRACKER_BYTES_PER_PAGE> snapshot;
};
std::mutex window_audit_mutex;
std::deque<WindowAuditRecord> window_audit;
u64 window_audit_serial = 0;
u32 window_audit_reopens = 0, window_audit_readbacks = 0, window_audit_skips = 0;

// Called while windows_mutex is held, before the page is made writable. The audit's lock is
// never held while acquiring windows_mutex, page locks, or the buffer cache's range lock.
u64 AuditWindowOpen(const WriteWindow& window, u64 tid) {
    if (!WindowAuditEnabled()) {
        return 0;
    }
    std::scoped_lock lk{window_audit_mutex};
    for (const auto& record : window_audit) {
        if (record.page == window.page && record.phase == WindowAuditRecord::Closing &&
            window_audit_reopens < 64) {
            ++window_audit_reopens;
            std::printf("Write window audit: reopen-during-close page=%#llx old_id=%llu "
                        "new_id=%llu opener=%#llx tid=%llu\n",
                        (unsigned long long)window.page, (unsigned long long)record.id,
                        (unsigned long long)(window_audit_serial + 1),
                        (unsigned long long)current_fault_rip, (unsigned long long)tid);
        }
    }
    if (window_audit.size() >= 1024) {
        // Retain live windows even when a long-lived one is older than the recent history.
        const auto old = std::find_if(window_audit.begin(), window_audit.end(), [](const auto& r) {
            return r.phase == WindowAuditRecord::Closed;
        });
        if (old == window_audit.end()) {
            return 0; // bounded diagnostics even with concurrent submitters
        }
        window_audit.erase(old);
    }
    const u64 id = ++window_audit_serial;
    window_audit.push_back({id, current_fault_rip, current_fault_caller, tid, window.page,
                            WindowAuditRecord::Open, window.before});
    return id;
}

void AuditWindowPhase(u64 id, const u8* after = nullptr) {
    if (!id) {
        return;
    }
    std::scoped_lock lk{window_audit_mutex};
    for (auto& record : window_audit) {
        if (record.id == id) {
            record.phase = after ? WindowAuditRecord::Closed : WindowAuditRecord::Closing;
            if (after) {
                std::memcpy(record.snapshot.data(), after, PM_PAGE_SIZE);
            }
            break;
        }
    }
}
} // namespace

void PageManager::DiagnoseWindowWrite(VAddr address, std::span<const u8> data, bool upload_skip) {
    if (!WindowAuditEnabled() || data.empty()) {
        return;
    }
    std::scoped_lock lk{window_audit_mutex};
    u32& reports = upload_skip ? window_audit_skips : window_audit_readbacks;
    if (reports >= 64) {
        return;
    }
    // Newest matching snapshot per page. Closed records are historical correlation only:
    // subsequent legitimate CPU/GPU writes or reuse of the mapping can change these bytes.
    std::vector<VAddr> seen_pages;
    for (auto it = window_audit.rbegin(); it != window_audit.rend(); ++it) {
        const auto& r = *it;
        const VAddr start = std::max(address, r.page);
        const VAddr end = std::min<VAddr>(address + data.size(), r.page + PM_PAGE_SIZE);
        if (start >= end || std::find(seen_pages.begin(), seen_pages.end(), r.page) != seen_pages.end()) {
            continue;
        }
        seen_pages.push_back(r.page);
        std::array<u8, PM_PAGE_SIZE> current;
        runtime_memory_read_backing(r.page, current.data(), PM_PAGE_SIZE);
        u64 changed = 0, replaced = 0;
        VAddr first = end;
        for (VAddr at = start; at < end; ++at) {
            const u64 offset = at - r.page;
            const bool differs = current[offset] != data[at - address];
            changed += current[offset] != r.snapshot[offset];
            replaced += differs;
            if (first == end && (upload_skip ||
                                (differs && current[offset] != r.snapshot[offset]))) {
                first = at;
            }
        }
        if (first == end) {
            continue;
        }
        const u64 offset = (first - r.page) & ~u64{7};
        u64 baseline = 0, guest = 0, incoming = 0;
        std::memcpy(&baseline, r.snapshot.data() + offset, 8);
        std::memcpy(&guest, current.data() + offset, 8);
        std::memcpy(&incoming, data.data() + (first - address), std::min<u64>(8, end - first));
        std::printf("Write window audit: %s id=%llu age=%llu phase=%s page=%#llx "
                    "range=%#llx+%llu opener=%#llx caller=+%#llx tid=%llu "
                    "changed_since_snapshot=%llu differs_from_payload=%llu "
                    "word=%#llx baseline=%#llx guest=%#llx payload_at=%#llx payload=%#llx\n",
                    upload_skip ? "upload-skip" : "readback-overwrite",
                    (unsigned long long)r.id, (unsigned long long)(window_audit_serial - r.id),
                    r.phase == WindowAuditRecord::Open ? "open" :
                    r.phase == WindowAuditRecord::Closing ? "closing" : "closed",
                    (unsigned long long)r.page, (unsigned long long)start,
                    (unsigned long long)(end - start), (unsigned long long)r.rip,
                    (unsigned long long)r.caller, (unsigned long long)r.tid,
                    (unsigned long long)changed, (unsigned long long)replaced,
                    (unsigned long long)(r.page + offset), (unsigned long long)baseline,
                    (unsigned long long)guest, (unsigned long long)first,
                    (unsigned long long)incoming);
        if (++reports >= 64) {
            break;
        }
    }
}

void PageManager::DumpWindowFault(void* context) {
    if (!WindowAuditEnabled()) {
        return;
    }
    const auto reg = [&](Common::FaultRegister r) { return Common::GetFaultRegister(context, r); };
    const u64 r14 = reg(Common::FaultRegister::R14);
    std::fprintf(stderr, "Write window audit fault: rip=%#llx r12=%#llx r13=%#llx r14=%#llx "
                         "r15=%#llx\n",
                 (unsigned long long)reg(Common::FaultRegister::Rip),
                 (unsigned long long)reg(Common::FaultRegister::R12),
                 (unsigned long long)reg(Common::FaultRegister::R13), (unsigned long long)r14,
                 (unsigned long long)reg(Common::FaultRegister::R15));
    if (reg(Common::FaultRegister::Rip) != GuestImage + 0x1262a2a) {
        return;
    }
    const VAddr head = r14 + 0x18;
    u64 value = 0;
    runtime_memory_read_backing(head, &value, sizeof(value));
    std::fprintf(stderr, "Write window audit fault: allocator head=%#llx value=%#llx\n",
                 (unsigned long long)head, (unsigned long long)value);
    // A fatal fault may have interrupted an audit on this thread. Never wait for its lock.
    std::unique_lock lk{window_audit_mutex, std::try_to_lock};
    if (!lk.owns_lock()) {
        std::fprintf(stderr, "Write window audit fault: history busy\n");
        return;
    }
    for (auto it = window_audit.rbegin(); it != window_audit.rend(); ++it) {
        if (head >= it->page && head + 8 <= it->page + PM_PAGE_SIZE) {
            std::memcpy(&value, it->snapshot.data() + head - it->page, 8);
            std::fprintf(stderr, "Write window audit fault: head page window id=%llu age=%llu "
                                 "phase=%u snapshot_head=%#llx opener=%#llx caller=+%#llx\n",
                         (unsigned long long)it->id,
                         (unsigned long long)(window_audit_serial - it->id), unsigned(it->phase),
                         (unsigned long long)value, (unsigned long long)it->rip,
                         (unsigned long long)it->caller);
            return;
        }
    }
    std::fprintf(stderr, "Write window audit fault: no retained window for allocator head page\n");
}

struct PageManager::Impl {
    struct PageState {
        u8 num_write_watchers : 7;
        // At the moment only buffer cache can request read watchers.
        // And buffers cannot overlap, thus only 1 can exist per page.
        u8 num_read_watchers : 1;

        Core::MemoryPermission WritePerm() const noexcept {
            return num_write_watchers == 0 ? Core::MemoryPermission::Write
                                           : Core::MemoryPermission::None;
        }

        Core::MemoryPermission ReadPerm() const noexcept {
            return num_read_watchers == 0 ? Core::MemoryPermission::Read
                                          : Core::MemoryPermission::None;
        }

        Core::MemoryPermission Perms() const noexcept {
            return ReadPerm() | WritePerm();
        }

        template <s32 delta, bool is_read>
        u8 AddDelta() {
            if constexpr (is_read) {
                if constexpr (delta == 1) {
                    return ++num_read_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_read_watchers > 0, "Not enough watchers");
                    return --num_read_watchers;
                } else {
                    return num_read_watchers;
                }
            } else {
                if constexpr (delta == 1) {
                    return ++num_write_watchers;
                } else if (delta == -1) {
                    ASSERT_MSG(num_write_watchers > 0, "Not enough watchers");
                    return --num_write_watchers;
                } else {
                    return num_write_watchers;
                }
            }
        }
    };

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;

    Impl() = default;
    virtual ~Impl() = default;

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        NoteFaultSite(context, addr);
        // bbport: the draw recording thread handles its faults inline too (vk_draw_pipe.h).
        const auto is_gpu_thread = rasterizer->IsGpuSideThread();
        if (is_gpu_thread) {
            BbStats::gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
        }
        if (Common::IsWriteError(context)) {
            BbStats::Timer timer{BbStats::t_write_faults};
            const u64 rip = Common::GetFaultRegister(context, Common::FaultRegister::Rip);
            // Only stores of the game's own code are single-stepped (the per-frame writes beside
            // GPU labels); bulk copies by the host's memcpy (asset loading) keep the normal path.
            bool step = false;
            const bool guest_store = rip >= GuestImage && rip < GuestImageEnd;
            fault_from_guest = guest_store;
            want_window = false;
            const bool windows = WriteWindowsEnabled() && !is_gpu_thread;
            bool handled = rasterizer->OnWriteFault(addr, is_gpu_thread, rip,
                                                    guest_store || windows ? &step : nullptr);
            if (handled && step && want_window) {
                // ufc3 (BB_WRITE_WINDOW): the page stays open until the next submission.
                want_window = false;
                if (!OpenWindow(addr)) {
                    handled = rasterizer->OnWriteFault(addr, is_gpu_thread, rip);
                }
            } else if (handled && step && !ArmStep(context, addr)) {
                // ufc3 (BB_WRITE_STEP): the write runs with its page open for one instruction.
                handled = rasterizer->OnWriteFault(addr, is_gpu_thread, rip);
            }
            current_fault_rip = 0;
            return handled;
        } else {
            BbStats::read_faults.fetch_add(1, std::memory_order_relaxed);
            BbStats::Timer timer{BbStats::t_read_faults};
            return rasterizer->ReadMemory(addr, 8, is_gpu_thread);
        }
        return false;
    }

    // ufc3 (BB_WRITE_STEP): threads single-stepping a write on a GPU-written page. The page is
    // open (read/write) for that one instruction; the trap compares it with the copy taken
    // before, hands the changed words to the buffer cache, and re-applies the protection the
    // page's watchers ask for. A table instead of thread_local (signal context). Two pages per
    // thread: an instruction may fault on a second page while stepping.
    inline static Impl* self = nullptr;

    static u64 StepTid() {
#ifdef _WIN32
        return GetCurrentThreadId();
#else
        return u64(syscall(SYS_gettid));
#endif
    }

    static StepSlot* FindStepSlot(u64 tid, bool claim) {
        for (auto& slot : step_slots) {
            if (slot.tid.load(std::memory_order_acquire) == tid) {
                return &slot;
            }
        }
        if (!claim) {
            return nullptr;
        }
        for (auto& slot : step_slots) {
            u64 expected = 0;
            if (slot.tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
                slot.count = 0;
                return &slot;
            }
        }
        return nullptr;
    }

    static bool ArmStep(void* context, VAddr addr) {
        if (!self) {
            return false;
        }
        const VAddr page = addr & ~VAddr(PM_PAGE_SIZE - 1);
        StepSlot* slot = FindStepSlot(StepTid(), true);
        if (!slot) {
            return false;
        }
        bool open = false;
        for (u32 i = 0; i < slot->count; ++i) {
            open |= slot->pages[i] == page;
        }
        if (!open) {
            if (slot->count >= 2) {
                return false;
            }
            runtime_memory_read_backing(page, slot->before[slot->count].data(), PM_PAGE_SIZE);
            slot->pages[slot->count++] = page;
            self->Protect(page, PM_PAGE_SIZE, Core::MemoryPermission::ReadWrite);
        }
        Common::SetTrapFlag(context, true);
        return true;
    }

    static bool OpenWindow(VAddr addr) {
        if (!self) {
            return false;
        }
        const VAddr page = addr & ~VAddr(PM_PAGE_SIZE - 1);
        std::scoped_lock wl{windows_mutex};
        for (const auto& window : open_windows) {
            if (window->page == page) {
                return true; // opened by another thread meanwhile: the write is retried
            }
        }
        if (open_windows.size() >= MaxWindows) {
            return false;
        }
        auto window = std::make_unique<WriteWindow>();
        window->page = page;
        runtime_memory_read_backing(page, window->before.data(), PM_PAGE_SIZE);
        window->audit_id = AuditWindowOpen(*window, StepTid());
        open_windows.push_back(std::move(window));
        open_window_count.store(u32(open_windows.size()), std::memory_order_release);
        self->Protect(page, PM_PAGE_SIZE, Core::MemoryPermission::ReadWrite);
        return true;
    }

    static void CloseWindows() {
        if (!self || open_window_count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::vector<std::unique_ptr<WriteWindow>> closing;
        {
            std::scoped_lock wl{windows_mutex};
            closing.swap(open_windows);
            for (const auto& window : closing) {
                AuditWindowPhase(window->audit_id);
            }
            open_window_count.store(0, std::memory_order_release);
        }
        std::array<u8, TRACKER_BYTES_PER_PAGE> after;
        for (const auto& window : closing) {
            const VAddr page = window->page;
            {
                // Protected first: a write after this faults (a new window), none is lost.
                const size_t page_index = page >> PM_PAGE_BITS;
                std::scoped_lock lk{self->locks[page_index / PAGES_PER_LOCK]};
                self->Protect(page, PM_PAGE_SIZE, self->cached_pages[page_index].Perms());
            }
            runtime_memory_read_backing(page, after.data(), PM_PAGE_SIZE);
            const auto& before = window->before;
            for (u64 at = 0; at < PM_PAGE_SIZE;) {
                if (std::memcmp(after.data() + at, before.data() + at, 4) == 0) {
                    at += 4;
                    continue;
                }
                u64 end = at + 4;
                while (end < PM_PAGE_SIZE &&
                       std::memcmp(after.data() + end, before.data() + end, 4) != 0) {
                    end += 4;
                }
                rasterizer->NoteSteppedWrite(page + at, std::vector<u8>(after.data() + at, after.data() + end));
                at = end;
            }
            AuditWindowPhase(window->audit_id, after.data());
        }
    }

    static bool OnStepTrap(void* context) {
        StepSlot* slot = FindStepSlot(StepTid(), false);
        if (!slot || !self) {
            return false;
        }
        Common::SetTrapFlag(context, false);
        std::array<u8, TRACKER_BYTES_PER_PAGE> after;
        for (u32 i = 0; i < slot->count; ++i) {
            const VAddr page = slot->pages[i];
            runtime_memory_read_backing(page, after.data(), PM_PAGE_SIZE);
            const auto& before = slot->before[i];
            for (u64 at = 0; at < PM_PAGE_SIZE;) {
                if (std::memcmp(after.data() + at, before.data() + at, 4) == 0) {
                    at += 4;
                    continue;
                }
                u64 end = at + 4;
                while (end < PM_PAGE_SIZE && std::memcmp(after.data() + end, before.data() + end, 4) != 0) {
                    end += 4;
                }
                rasterizer->NoteSteppedWrite(page + at, std::vector<u8>(after.data() + at, after.data() + end));
                at = end;
            }
            const size_t page_index = page >> PM_PAGE_BITS;
            std::scoped_lock lk{self->locks[page_index / PAGES_PER_LOCK]};
            self->Protect(page, PM_PAGE_SIZE, self->cached_pages[page_index].Perms());
        }
        slot->count = 0;
        slot->tid.store(0, std::memory_order_release);
        return true;
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect(range_begin << PM_PAGE_BITS, range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                Protect((range_begin << PM_PAGE_BITS), range_bytes, perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

#ifdef __linux__
// bbport: write tracking with userfaultfd write-protection instead of mprotect. mprotect takes
// the address space lock for writing and splits mappings; while guest threads fault on
// per-frame buffers and the GPU thread re-protects uploaded pages, every page fault in the
// process (copy threads included) waits for it. UFFDIO_WRITEPROTECT changes page table bits
// under the lock for reading. Read protection (readbacks) still uses mprotect: userfaultfd
// write-protection cannot deny reads. BB_UFFD=1.
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;
    std::mutex read_revoked_mutex;
    std::unordered_set<u64> read_revoked_pages; ///< 4 KiB pages denied reads with mprotect
    std::atomic<size_t> num_read_revoked{0};

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Guest memory is a shared memfd mapping: write-protection there needs the shmem
        // feature, and unpopulated pages must be protectable too.
        uffdio_api api{};
        api.api = UFFD_API;
        api.features =
            UFFD_FEATURE_THREAD_ID | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(uffd, UFFDIO_API, &api) != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            close(uffd);
            throw std::runtime_error("uffdio_api");
        }

        // Read faults (readbacks) still arrive as signals.
        Core::Signals::Instance()->RegisterAccessViolationHandler(
            GuestFaultSignalHandler, std::numeric_limits<u32>::min());

        ufd_thread = std::jthread([this](std::stop_token token) { UffdHandler(token); });
        std::printf("GPU: memory tracking with userfaultfd write-protection\n");
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        uffdio_register reg{};
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio register {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range{};
        range.start = address;
        range.len = size;
        ioctl(uffd, UFFDIO_UNREGISTER, &range);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        auto& address_space = Core::Memory::Instance()->GetAddressSpace();
        const bool allow_read = True(perms & Core::MemoryPermission::Read);
        const bool allow_write = True(perms & Core::MemoryPermission::Write);
        const u64 first = address >> 12, last = (address + size + 4095) >> 12;
        if (!allow_read) {
            // Readbacks: deny all access with mprotect.
            address_space.Protect(address, size, Core::MemoryPermission::None);
            std::scoped_lock lk{read_revoked_mutex};
            for (u64 page = first; page < last; ++page) {
                read_revoked_pages.insert(page);
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            return;
        }
        if (num_read_revoked.load(std::memory_order_acquire) != 0) {
            std::scoped_lock lk{read_revoked_mutex};
            bool restore = false;
            for (u64 page = first; page < last; ++page) {
                restore |= read_revoked_pages.erase(page) != 0;
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            if (restore) {
                address_space.Protect(address, size, Core::MemoryPermission::ReadWrite);
            }
        }
        // Counted like AddressSpace::Protect (BB_FRAME_LOG protect_ms).
        BbStats::Timer timer{BbStats::t_protect};
        BbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
        BbStats::protect_pages.fetch_add(last - first, std::memory_order_relaxed);
        if (!allow_write) {
            BbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
            BbStats::protect_revoke_pages.fetch_add(last - first, std::memory_order_relaxed);
        }
        uffdio_writeprotect wp{};
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio writeprotect {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void UffdHandler(std::stop_token token) {
        Common::SetCurrentThreadName("bb:Uffd");
        while (!token.stop_requested()) {
            pollfd pollfd{};
            pollfd.fd = uffd;
            pollfd.events = POLLIN;
            // Short timeout so the stop request is seen.
            const int pollres = poll(&pollfd, 1, 100);
            if (pollres <= 0 || !(pollfd.revents & POLLIN)) {
                continue;
            }
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret != sizeof(msg)) {
                continue;
            }
            if (msg.event != UFFD_EVENT_PAGEFAULT) {
                continue;
            }
            const VAddr addr = msg.arg.pagefault.address;
            const auto ptid = msg.arg.pagefault.feat.ptid;
            {
                BbStats::Timer timer{BbStats::t_write_faults};
                rasterizer->OnWriteFault(addr, rasterizer->IsGpuSideThreadId(ptid));
            }
            // Protect() clears with DONTWAKE (it may run for pages nobody waits on).
            uffdio_range wake{};
            wake.start = addr & ~u64(PM_PAGE_SIZE - 1);
            wake.len = PM_PAGE_SIZE;
            ioctl(uffd, UFFDIO_WAKE, &wake);
        }
    }
};
#endif // __linux__

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        self = this;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        // bbport BB_GUEST_IN_PLACE: no page protection; the pages are marked in the runtime's write
        // watch instead, and the writers that check it tell the GPU side (WriteTracking).
        if (!VideoCore::WriteTracking()) {
            runtime_memory_set_write_watch(address, size, !True(perms & Core::MemoryPermission::Write));
            if (!VideoCore::WriteVerify()) {
                return;
            }
        }
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        impl.Protect(address, size, perms);
    }

};

bool PageManager::OnStepTrap(void* context) {
    return Impl::OnStepTrap(context);
}

bool PageManager::WriteWindowsEnabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_WRITE_WINDOW");
        const bool on = env && env[0] == '1';
        if (on) {
            std::printf("GPU: CPU writes beside GPU data open their page until the next submission "
                        "(BB_WRITE_WINDOW)\n");
        }
        return on;
    }();
    return enabled;
}

bool PageManager::FaultFromGuest() {
    return fault_from_guest;
}

void PageManager::RequestWindow() {
    want_window = true;
}

void PageManager::CloseWriteWindows() {
    Impl::CloseWindows();
}

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef __linux__
    // bbport: dma-buf guest memory (BB_GUEST_GPU_MEMORY=1) cannot be write-protected by userfaultfd
    // (anonymous and shmem only): its registration would fail and writes go unseen.
    const char* guest_gpu_memory = std::getenv("BB_GUEST_GPU_MEMORY");
    const bool dma_buf_guest = guest_gpu_memory && guest_gpu_memory[0] == '1';
    if (std::getenv("BB_UFFD") && std::getenv("BB_UFFD")[0] == '1' && dma_buf_guest) {
        std::printf("GPU: userfaultfd off with BB_GUEST_GPU_MEMORY=1 (dma-buf memory)\n");
    } else if (std::getenv("BB_UFFD") && std::getenv("BB_UFFD")[0] == '1') {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

template <bool track>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, false>(addr, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
