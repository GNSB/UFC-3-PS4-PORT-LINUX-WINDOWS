/* UFC-58 (ufc3): back-pressure between the takers and the retirer of the appearance results.
 *
 * Create fighter crashes with a null write at +0x402882 (GitHub #26): the publisher of the
 * fighter's appearance (skinning/morph) results, +0x402710 on RenderThread, takes a node from a
 * pool of four (this+0x1a0 free list, then the cursor this+0x1a8..+0x1b0, stride 0x38) and uses
 * NULL without checking when none is left. Before that it recycles every published result nobody
 * took (node+0x30 == 0). A result stays held from the moment the character's update job takes it
 * (+0x403210, sets node+0x30) until the component update retires it (+0x4032a0 frees every node up
 * to the sequence the take stored in the context, +0x168). So the pool runs dry only when four
 * results were taken with no retire in between: the takes ran ahead of the retirer. Nothing in the
 * game makes a take wait for the retire; on the PS4 they keep pace, in the port the threads drift
 * apart (the render side processes queued sim steps late, the update that retires is skipped).
 *
 * The three of them take the pool's lock (this+0xd0) through the game's spin-then-lock wrapper
 * (+0x443f0 -> +0x44470 -> scePthreadMutexTrylock / scePthreadMutexLock). There, before the lock,
 * the runtime tells them apart by their return addresses on the stack and reads the pool:
 *  - a take that would leave the publisher no node waits until the retirer frees some;
 *  - a publish that would find no node waits for the retirer too (the last moment before the crash).
 * They only wait when the retirer was seen running on another thread (else the wait could not
 * end), at most BB_APPEARANCE_BACKPRESSURE_MS (default 250); after a wait that ran out, that pool
 * goes on without waiting until it has room again. The game is not changed: no node is added, no
 * null is skipped. BB_APPEARANCE_BACKPRESSURE=0 turns it off. */
#define _GNU_SOURCE
#include "runtime.h"
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Return addresses (image offsets) of the lock wrapper and of the pool's three callers. */
#define SPIN_TRY_RET 0x444cc  /* +0x44470: call scePthreadMutexTrylock */
#define SPIN_LOCK_RET 0x444a4 /* +0x44470: call scePthreadMutexLock */
#define WRAPPER_RET 0x44420   /* +0x443f0: call +0x44470 (the last try is a tail jump instead) */
#define PUBLISH_RET 0x4027a1  /* +0x402710: lock this+0xd0 (r14), then the pool allocation */
#define TAKE_RET 0x403239     /* +0x403210: lock this+0xd0 (r15), take the newest result */
#define RETIRE_RET 0x4032c9   /* +0x4032a0: lock this+0xd0 (rbx), retire up to a sequence */
/* +0x44470 keeps rbp, r14, rbx and 0x20 bytes below its return address; +0x443f0 keeps rbp, r15,
 * r14, rbx and one more slot. From the wrapper's return-address slot Z: [Z+0x40] is +0x44470's
 * return address and, when that is +0x443f0, [Z+0x70] is the pool caller's. */
#define TO_SPIN_CALLER 8
#define TO_WRAPPER_CALLER 14
#define POOL_LOCK 0xd0
#define NODE_SIZE 0x38
#define POOL_NODES 4
#define TEXT_END 0x23e884c /* end of the eboot's code */

static const struct { uint64_t offset; unsigned char bytes[16]; unsigned size; } code[] = {
    {0x44470, {0x55,0x48,0x89,0xe5,0x41,0x56,0x53,0x48,0x83,0xec,0x20}, 11},       /* frame of +0x44470 */
    {0x4449f, {0xe8,0xbc,0x5e,0xdb,0x01}, 5},                                     /* call scePthreadMutexLock */
    {0x444c7, {0xe8,0xb4,0x5e,0xdb,0x01}, 5},                                     /* call scePthreadMutexTrylock */
    {0x443f0, {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x53,0x50}, 10},           /* frame of +0x443f0 */
    {0x4441b, {0xe8,0x50,0x00,0x00,0x00}, 5},                                     /* call +0x44470 */
    {0x44443, {0xe9,0x28,0x00,0x00,0x00}, 5},                                     /* jmp +0x44470 */
    {0x402798, {0x4c,0x0f,0xaf,0xe0,0xe8,0x4f,0x1c,0xc4,0xff}, 9},                /* publish: call +0x443f0 */
    {0x40322a, {0x4c,0x8d,0xbb,0xd0,0x00,0x00,0x00,0x4c,0x89,0xff,0xe8,0xb7,0x11,0xc4,0xff}, 15}, /* take */
    {0x4032ba, {0x49,0x8d,0x9f,0xd0,0x00,0x00,0x00,0x48,0x89,0xdf,0xe8,0x27,0x11,0xc4,0xff}, 15}, /* retire */
    {0x40287f, {0x45,0x31,0xc0,0x4d,0x89,0x10}, 6},                               /* the null write */
    {0x403269, {0xc6,0x40,0x30,0x01}, 4},                                         /* take marks node+0x30 */
};

uintptr_t runtime_appearance_try_site, runtime_appearance_lock_site;
static uintptr_t base;
static uint64_t max_wait_ns = 250000000;
enum { PUBLISH = RUNTIME_APPEARANCE_PUBLISH, TAKE = RUNTIME_APPEARANCE_TAKE, RETIRE = RUNTIME_APPEARANCE_RETIRE, ROLES };
static const char *const role_name[ROLES] = {"publish", "take", "retire"};
typedef struct {
    uintptr_t pool;
    uint32_t tid[ROLES];
    char thread[ROLES][16];
    int described, gave_up;
} Pool;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Pool pools[16];
static uint64_t waits[ROLES], timeouts[ROLES], wait_ns, longest_ns, gave_up_skips;

static unsigned env_number(const char *name, unsigned fallback, unsigned low, unsigned high) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    const unsigned long n = strtoul(value, NULL, 10);
    return n < low ? low : n > high ? high : (unsigned)n;
}
void runtime_appearance_init(const unsigned char *image, uint64_t size) {
    for (size_t i = 0; i < sizeof(code) / sizeof(*code); ++i)
        if (size < code[i].offset + code[i].size || memcmp(image + code[i].offset, code[i].bytes, code[i].size)) return;
    const char *env = getenv("BB_APPEARANCE_BACKPRESSURE");
    if (env && env[0] == '0') { printf("Appearance back-pressure (UFC-58): off (BB_APPEARANCE_BACKPRESSURE=0)\n"); return; }
    max_wait_ns = (uint64_t)env_number("BB_APPEARANCE_BACKPRESSURE_MS", 250, 1, 10000) * 1000000;
    base = (uintptr_t)image;
    runtime_appearance_try_site = base + SPIN_TRY_RET;
    runtime_appearance_lock_site = base + SPIN_LOCK_RET;
    printf("Appearance back-pressure (UFC-58): on; takes and publishes that would exhaust the appearance pool wait up to %" PRIu64
           " ms for its retire\n", max_wait_ns / 1000000);
}

/* What the pool holds now, read without its lock (the waits only need an estimate; every link is
 * checked to stay inside the pool's nodes, else the pool is left alone). */
typedef struct { unsigned active, taken, room; int would_take; } State;
static int pool_state(uintptr_t pool, State *s) {
    const volatile uint64_t *f = (const volatile uint64_t *)pool;
    if (f[0x1b8 / 8] != NODE_SIZE) return 0;
    const uintptr_t end = f[0x1b0 / 8], cursor = f[0x1a8 / 8], sentinel = pool + 0x188;
    uintptr_t node = f[0x188 / 8];
    s->active = s->taken = 0;
    while (node != sentinel) {
        if (s->active >= POOL_NODES || node >= end || end - node > POOL_NODES * NODE_SIZE || (end - node) % NODE_SIZE) return 0;
        ++s->active;
        s->taken += *(const volatile uint8_t *)(node + 0x30) != 0;
        node = *(const volatile uint64_t *)node;
    }
    /* Nodes the publisher can get: the ones nobody took (it recycles them first), the free list's
     * head and the unused cursor. */
    s->room = s->active - s->taken + (f[0x1a0 / 8] != 0) + (cursor != end);
    const uintptr_t newest = f[0x190 / 8];
    s->would_take = s->active && newest != sentinel && !*(const volatile uint8_t *)(newest + 0x30) &&
                    (int64_t)*(const volatile uint64_t *)(newest + 0x10) > (int64_t)f[0x180 / 8];
    return 1;
}
/* A take must leave the publisher a node; a publish needs one. */
static int must_wait(int role, uintptr_t pool) {
    State s;
    if (!pool_state(pool, &s)) return 0;
    return role == TAKE ? s.would_take && s.room <= 1 : s.room == 0;
}
static Pool *find_pool(uintptr_t pool) {
    Pool *free_slot = NULL;
    for (size_t i = 0; i < sizeof(pools) / sizeof(*pools); ++i) {
        if (pools[i].pool == pool) return &pools[i];
        if (!pools[i].pool && !free_slot) free_slot = &pools[i];
    }
    if (!free_slot) free_slot = &pools[pool / 64 % (sizeof(pools) / sizeof(*pools))];
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->pool = pool;
    return free_slot;
}
static uint64_t now_ns(void) { return platform_monotonic_ns(); }

void runtime_appearance_pool_lock(int role, uintptr_t pool) {
    const uint32_t self = platform_thread_id();
    pthread_mutex_lock(&lock);
    Pool *p = find_pool(pool);
    if (p->tid[role] != self) {
        p->tid[role] = self;
        platform_thread_get_name(p->thread[role], sizeof(p->thread[role]));
    }
    /* Which thread does what, once per pool (takes run as jobs: their thread may vary). */
    if (!p->described && p->tid[PUBLISH] && p->tid[TAKE] && p->tid[RETIRE]) {
        p->described = 1;
        printf("Appearance back-pressure (UFC-58): pool %#" PRIxPTR ": publish on thread %u %s, take on %u %s, retire on %u %s\n",
               pool, p->tid[PUBLISH], p->thread[PUBLISH], p->tid[TAKE], p->thread[TAKE], p->tid[RETIRE], p->thread[RETIRE]);
    }
    if (role == RETIRE || !p->tid[RETIRE] || p->tid[RETIRE] == self) { pthread_mutex_unlock(&lock); return; }
    if (!must_wait(role, pool)) { p->gave_up = 0; pthread_mutex_unlock(&lock); return; }
    if (p->gave_up) { ++gave_up_skips; pthread_mutex_unlock(&lock); return; }
    pthread_mutex_unlock(&lock);

    const uint64_t start = now_ns(), deadline = start + max_wait_ns;
    int waiting = 1;
    while (waiting && now_ns() < deadline) {
        platform_sleep_ns(500000);
        waiting = must_wait(role, pool);
    }
    const uint64_t ns = now_ns() - start;
    pthread_mutex_lock(&lock);
    p = find_pool(pool);
    p->gave_up = waiting;
    ++waits[role]; timeouts[role] += waiting; wait_ns += ns;
    if (ns > longest_ns) longest_ns = ns;
    const uint64_t n = waits[TAKE] + waits[PUBLISH], t = timeouts[TAKE] + timeouts[PUBLISH], longest = longest_ns;
    pthread_mutex_unlock(&lock);
    runtime_wait_note(1, ns);
    if (n == 1 || !(n & 255) || waiting)
        printf("Appearance back-pressure (UFC-58): a %s waited %.1f ms for the retire%s (%" PRIu64 " waits, %" PRIu64
               " ran out, longest %.1f ms)\n", role_name[role], ns / 1e6, waiting ? " and ran out" : "", n, t, longest / 1e6);
}
/* scePthreadMutexTrylock/Lock called from the game's lock wrapper (`site`): if the wrapper was
 * called by the publisher, a take or the retire of an appearance pool, `mutex` is the pool's
 * this+0xd0. The wrapper's return-address slot is found on this thread's stack above here. */
__attribute__((noinline)) void runtime_appearance_before_lock(const void *mutex, uintptr_t site) {
    const uintptr_t *sp = (const uintptr_t *)__builtin_frame_address(0);
    for (int i = 0; i < 64; ++i) {
        if (sp[i] != site) continue;
        uintptr_t caller = sp[i + TO_SPIN_CALLER];
        if (caller == base + WRAPPER_RET) caller = sp[i + TO_WRAPPER_CALLER];
        const int role = caller == base + PUBLISH_RET ? PUBLISH : caller == base + TAKE_RET ? TAKE
                       : caller == base + RETIRE_RET ? RETIRE : -1;
        if (role >= 0) runtime_appearance_pool_lock(role, (uintptr_t)mutex - POOL_LOCK);
        /* The wrapper's slot (its caller is game code): any other lock of the game, done. */
        if (role >= 0 || caller == base + WRAPPER_RET || (caller >= base && caller < base + TEXT_END)) return;
    }
}
void runtime_appearance_report(void) {
    if (!base) return;
    pthread_mutex_lock(&lock);
    printf("Runtime: appearance back-pressure (UFC-58): %" PRIu64 " takes and %" PRIu64 " publishes waited for the retire "
           "(%" PRIu64 "/%" PRIu64 " ran out), %.1f ms in all, longest %.1f ms; %" PRIu64 " went on after a wait ran out\n",
           waits[TAKE], waits[PUBLISH], timeouts[TAKE], timeouts[PUBLISH], wait_ns / 1e6, longest_ns / 1e6, gave_up_skips);
    pthread_mutex_unlock(&lock);
}
