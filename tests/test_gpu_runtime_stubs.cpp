// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <setjmp.h>
// Renderer tests have no guest process. Clock/host-thread services work; guest accesses abort.
extern "C" {
// Fallback host diagnostics; label-guard-test supplies its own versions.
__attribute__((weak)) void platform_describe_address(uintptr_t address, char* out, size_t size) {
    std::snprintf(out, size, "%#llx", (unsigned long long)address);
}
__attribute__((weak)) int platform_backtrace(void**, int) { return 0; }
__attribute__((weak)) void platform_thread_get_name(char* out, size_t size) {
    std::snprintf(out, size, "test");
}
__attribute__((weak)) uint32_t platform_thread_id() { return 1; }
thread_local sigjmp_buf* runtime_fault_recover = nullptr;
uint32_t runtime_disabled_optimizations = 0;
uint64_t runtime_experiment_bits = 0;
int runtime_guest_copy_range(uintptr_t*, uintptr_t*) { return 0; }
void runtime_ui_guest_flip() { std::abort(); }
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) { std::abort(); }
void runtime_thread_attach_host(const char*) {}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
void runtime_memory_read_backing(uintptr_t, void*, uint64_t) { std::abort(); }
const uint64_t* runtime_memory_generation() { static const uint64_t generation = 0; return &generation; }
void runtime_memory_note_write(uintptr_t, uint64_t) { std::abort(); }
void runtime_memory_set_note_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_cpu_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_host_copy_hook(void (*)(uintptr_t, uint64_t), const uint32_t*) {}
int runtime_memory_host_copy_enabled(void) { return 0; }
int runtime_guest_copy_range(uintptr_t*, uintptr_t*) { return 0; }
void runtime_memory_set_write_watch(uintptr_t, uint64_t, int) { std::abort(); }
void runtime_memory_set_guest_chunk_allocator(int (*)(uint64_t, uint64_t)) {}
void runtime_memory_set_guest_chunk_whole(int) {}
int runtime_memory_vma_info(uintptr_t, int*, int*, uintptr_t*) { std::abort(); }
int runtime_memory_direct_phys(uintptr_t, uint64_t*, uintptr_t*) { std::abort(); }
void runtime_wait_report(double) {}
int runtime_wait_trace_selected() { return 0; }
void runtime_trace_stamp(char* out, size_t size) { std::snprintf(out, size, "[test] "); }
void runtime_sleep_stats(uint64_t* calls, uint64_t* ns) { *calls = 0; *ns = 0; }
uint64_t runtime_heap_growths() { return 0; }
}

