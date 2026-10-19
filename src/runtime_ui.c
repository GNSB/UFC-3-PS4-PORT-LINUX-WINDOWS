/* UFC-57 (ufc3): back-pressure from the renderer to the UI's snapshot builder.
 *
 * The UI Update thread copies every node of a UI scene into a snapshot (+0x1a3d100, one
 * 0x1e8-byte block per node from the FE heap of memory.cfg, 320 MiB as on the PS4) once per
 * game tick, and queues it for the render thread; the render thread's UI draw takes every
 * queued snapshot (+0x1a3be50) and the next build frees them. Nothing in the game makes the
 * builder wait for the renderer: the main loop only waits for the UI Update job. On the PS4 the
 * render thread never stops for long; here it does (readbacks served by the GPU command thread,
 * seconds on GitHub #16), and the builds made meanwhile ran the FE heap dry: the builder stores
 * the NULL copy and the next build writes through it (+0x1a3d204, Career after a title).
 *
 * The builder takes the scene's lock through pthread_mutex_lock from one call site (+0x1a3d140).
 * There, before the lock, a scene that has already been built `budget` times since the guest's
 * last flip waits for the next flip (at most BB_UI_BACKPRESSURE_MS, then one build goes through).
 * The game itself is not changed. BB_UI_BACKPRESSURE=0 turns it off;
 * BB_UI_BACKPRESSURE_BUILDS (default 8) and BB_UI_BACKPRESSURE_MS (default 250) tune it. */
#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUILD_LOCK_CALL 0x1a3d134 /* mov rdi,[r13+0x48]; add rdi,0x10; mov [rbp-0x58],rdi; call pthread_mutex_lock */
static const unsigned char build_lock_call[]={0x49,0x8b,0x7d,0x48,0x48,0x83,0xc7,0x10,0x48,0x89,0x7d,0xa8,0xe8,0xfb,0xcd,0x3b,0x00};

uintptr_t runtime_ui_build_site;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flipped;
static uint64_t flips, waiting;
static unsigned budget=8;
static uint64_t max_wait_ns=250000000;
typedef struct { const void *key; uint64_t flip; unsigned builds; } Scene;
static Scene scenes[16];
static uint64_t waits, timeouts, wait_ns, longest_ns;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(platform_wait_clock(),&t);
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
static unsigned env_number(const char *name, unsigned fallback, unsigned low, unsigned high) {
    const char *value=getenv(name);
    if (!value || !*value) return fallback;
    const unsigned long n=strtoul(value,NULL,10);
    return n<low ? low : n>high ? high : (unsigned)n;
}
void runtime_ui_backpressure_init(const unsigned char *image, uint64_t size) {
    const char *env=getenv("BB_UI_BACKPRESSURE");
    if (env && env[0]=='0') { printf("UI back-pressure (UFC-57): off (BB_UI_BACKPRESSURE=0)\n"); return; }
    if (size<BUILD_LOCK_CALL+sizeof(build_lock_call) || memcmp(image+BUILD_LOCK_CALL,build_lock_call,sizeof(build_lock_call))) return;
    budget=env_number("BB_UI_BACKPRESSURE_BUILDS",8,1,1000);
    max_wait_ns=(uint64_t)env_number("BB_UI_BACKPRESSURE_MS",250,1,10000)*1000000;
    platform_cond_init_waitclock(&flipped);
    runtime_ui_build_site=(uintptr_t)image+BUILD_LOCK_CALL+sizeof(build_lock_call);
    printf("UI back-pressure (UFC-57): on; a UI scene built %u times without a flip waits up to %" PRIu64 " ms for the next one\n",
           budget,max_wait_ns/1000000);
}
/* The GPU library: the guest handed a frame to VideoOut (its UI draw has run). */
void runtime_ui_guest_flip(void) {
    if (!runtime_ui_build_site) return;
    pthread_mutex_lock(&lock);
    ++flips;
    if (waiting) pthread_cond_broadcast(&flipped);
    pthread_mutex_unlock(&lock);
}
/* posix_mutex_lock called from the snapshot builder; `key` is the scene's lock. */
void runtime_ui_before_build(const void *key) {
    pthread_mutex_lock(&lock);
    if (!flips) { pthread_mutex_unlock(&lock); return; } /* nothing presented yet: no renderer to wait for */
    Scene *s=NULL;
    for (size_t i=0;i<sizeof(scenes)/sizeof(*scenes) && !s;++i)
        if (scenes[i].key==key || !scenes[i].key) s=&scenes[i];
    if (!s) s=&scenes[(uintptr_t)key/64%(sizeof(scenes)/sizeof(*scenes))];
    if (s->key!=key || s->flip!=flips) { s->key=key; s->flip=flips; s->builds=0; }
    if (++s->builds<=budget) { pthread_mutex_unlock(&lock); return; }
    const uint64_t start=now_ns(), seen=flips, deadline=start+max_wait_ns;
    const struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
    int timed_out=0;
    ++waiting;
    while (flips==seen && !timed_out) timed_out=pthread_cond_timedwait(&flipped,&lock,&end)==ETIMEDOUT && flips==seen;
    --waiting;
    const uint64_t ns=now_ns()-start;
    ++waits; timeouts+=timed_out; wait_ns+=ns;
    if (ns>longest_ns) longest_ns=ns;
    /* A flip came: this build is the first since it. None came: the next build waits again. */
    if (timed_out) s->builds=budget; else { s->flip=flips; s->builds=1; }
    const int report=waits==1 || !(waits&255);
    const uint64_t n=waits, t=timeouts, longest=longest_ns;
    pthread_mutex_unlock(&lock);
    runtime_wait_note(1,ns);
    if (report)
        printf("UI back-pressure (UFC-57): %" PRIu64 " UI builds waited for the renderer (%" PRIu64 " found no flip in time), longest %.1f ms\n",
               n,t,longest/1e6);
}
void runtime_ui_report(void) {
    if (!runtime_ui_build_site) return;
    pthread_mutex_lock(&lock);
    printf("Runtime: UI back-pressure (UFC-57): %" PRIu64 " waits, %" PRIu64 " without a flip in time, %.1f ms in all, longest %.1f ms\n",
           waits,timeouts,wait_ns/1e6,longest_ns/1e6);
    pthread_mutex_unlock(&lock);
}
