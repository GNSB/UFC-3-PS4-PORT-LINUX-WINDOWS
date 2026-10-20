#define _GNU_SOURCE
#include "runtime.h"
#include <stdlib.h>

/* The settings menu of the GPU library restarts through probe.c, which tests do not link. */
void runtime_restart(void) { abort(); }
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

typedef int (ABI *Register)(GuestCallback);
typedef int (ABI *RegisterCxa)(void (ABI *)(void *), void *, void *);
typedef int (ABI *Acquire)(uint64_t *);
typedef void (ABI *Guard)(uint64_t *);
typedef int32_t (ABI *AttrInit)(void **);
typedef int32_t (ABI *AttrType)(void **, int);
typedef int32_t (ABI *MutexInit)(void **, void **, const char *);
typedef int32_t (ABI *HandleOp)(void **);
typedef int32_t (ABI *Allocate)(int64_t, int64_t, uint64_t, uint64_t, int, int64_t *);
typedef int32_t (ABI *Map)(void **, uint64_t, int, int, int64_t, uint64_t);
typedef int32_t (ABI *Unmap)(void *, uint64_t);
typedef int32_t (ABI *Release)(uint64_t, uint64_t);
#define GET(type, name) ((type)runtime_resolve(name, 0))
typedef void *(ABI *TlsAddress)(const uint64_t *);
typedef void *(ABI *ThreadSelf)(void);
typedef int32_t (ABI *ThreadGetAttr)(void *,void **);
typedef int32_t (ABI *ThreadAffinity)(void **,uint64_t *);
static void *main_identity;
static void *tls_worker(void *unused) {
    (void)unused;
    const uint64_t index[]={2,0};
    unsigned char *p=GET(TlsAddress,"vNe1w4diLCs#libkernel")(index);
    assert(p[0]==42 && p[1]==17 && p[31]==0);
    p[0]=99;
    assert(GET(ThreadSelf,"aI+OeCz8xrQ#libkernel")()!=main_identity);
    return NULL;
}
static void libc_support(void) {
    static const unsigned char initial[]={42,17};
    runtime_set_libc_tls(initial,sizeof(initial),32);
    const uint64_t index[]={2,0}, last[]={2,31};
    TlsAddress tls=GET(TlsAddress,"vNe1w4diLCs#libkernel");
    unsigned char *p=tls(index);
    assert(p[0]==42 && p[1]==17 && p[2]==0 && p[31]==0);
    assert(tls(last)==p+31 && tls(index)==p);
    p[0]=23;
    main_identity=GET(ThreadSelf,"aI+OeCz8xrQ#libkernel")();
    pthread_t worker;
    assert(pthread_create(&worker,NULL,tls_worker,NULL)==0);
    assert(pthread_join(worker,NULL)==0);
    assert(p[0]==23 && initial[0]==42);
    void *attr=NULL;
    AttrInit init=GET(AttrInit,"nsYoNRywwNg#libkernel");
    ThreadGetAttr get=GET(ThreadGetAttr,"x1X76arYMxU#libkernel");
    ThreadAffinity affinity=GET(ThreadAffinity,"8+s5BzZjxSg#libkernel");
    HandleOp destroy=GET(HandleOp,"62KCwEMmzcM#libkernel");
    uint64_t mask=0;
    assert((uint32_t)init(NULL)==0x80020016);
    assert(init(&attr)==0 && get(main_identity,&attr)==0);
    assert(affinity(&attr,&mask)==0 && mask==0x7f);
    assert((uint32_t)get(&mask,&attr)==0x80020003);
    assert((uint32_t)affinity(&attr,NULL)==0x80020016);
    assert(destroy(&attr)==0 && !attr);
    assert((uint32_t)affinity(&attr,&mask)==0x80020016);
    uint64_t param[8]={64};
    runtime_set_procparam(param);
    assert(GET(ThreadSelf,"959qrazPIrg#libkernel")()==param);
    typedef void (ABI *SetHeap)(void **);
    void *heap[10]={0};
    GET(SetHeap,"p5EcQeEeJAE#libkernel")(heap);
    assert(runtime_application_heap_api()==heap);
    GET(SetHeap,"p5EcQeEeJAE#libkernel")(NULL);
    assert(runtime_application_heap_api()==NULL);
    runtime_set_procparam(NULL);
}
static int order[512], count;
static Register register_plain;
static ABI void first(void) { order[count++]=1; }
static ABI void second(void) { order[count++]=2; }
static ABI void with_arg(void *p) { order[count++]=(int)(uintptr_t)p; }
static ABI void add_handler(void) { order[count++]=4; assert(register_plain(first)==0); }

static void exit_handlers(void) {
    register_plain=GET(Register,"8G2LB+A3rzg#libc");
    RegisterCxa cxa=GET(RegisterCxa,"tsvEmnenz48#libc");
    assert(register_plain && cxa);
    assert(register_plain(first)==0);
    assert(cxa(with_arg,(void *)3,(void *)11)==0);
    assert(register_plain(second)==0);
    runtime_finalize((void *)11);
    assert(count==1 && order[0]==3);
    runtime_finalize((void *)11); assert(count==1);
    runtime_finalize(NULL);
    assert(count==3 && order[1]==2 && order[2]==1);
    runtime_finalize(NULL); assert(count==3);
    assert(register_plain(add_handler)==0);
    runtime_finalize(NULL);
    assert(count==5 && order[3]==4 && order[4]==1);
    /* Exercise reallocating the registry and strict LIFO ordering. */
    count=0;
    for (uintptr_t i=0; i<200; ++i) assert(cxa(with_arg,(void *)i,(void *)12)==0);
    runtime_finalize((void *)12);
    assert(count==200);
    for (int i=0; i<200; ++i) assert(order[i]==199-i);
}
static void guards(void) {
    uint64_t state=0;
    Acquire acquire=GET(Acquire,"3GPpjQdAMTw#libc");
    Guard release=GET(Guard,"9rAeANT2tyE#libc"), abort_guard=GET(Guard,"2emaaluWzUw#libc");
    assert(acquire(&state)==1 && state==(UINT64_C(1)<<32));
    abort_guard(&state); assert(state==0);
    assert(acquire(&state)==1);
    release(&state); assert(state==1);
    assert(acquire(&state)==0);
}
static void mutexes(void) {
    AttrInit attr_init=GET(AttrInit,"F8bUHwAG284#libkernel");
    AttrType type=GET(AttrType,"iMp8QpE+XO4#libkernel");
    HandleOp attr_destroy=GET(HandleOp,"smWEktiyyG0#libkernel");
    MutexInit init=GET(MutexInit,"cmo1RIYva9o#libkernel");
    HandleOp lock=GET(HandleOp,"9UK1vLZQft4#libkernel"), unlock=GET(HandleOp,"tn3VlD0hG60#libkernel");
    HandleOp trylock=GET(HandleOp,"upoVrzMHFeE#libkernel"), destroy=GET(HandleOp,"2Of0f+3mhhE#libkernel");
    void *attr=NULL, *mutex=NULL;
    assert((uint32_t)attr_init(NULL)==0x80020016);
    assert(attr_init(&attr)==0);
    assert((uint32_t)type(&attr,99)==0x80020016);
    assert(type(&attr,2)==0 && init(&mutex,&attr,"recursive-test")==0);
    assert(attr_destroy(&attr)==0 && attr==NULL);
    assert(lock(&mutex)==0 && trylock(&mutex)==0);
    assert((uint32_t)destroy(&mutex)==0x80020010);
    assert(unlock(&mutex)==0 && unlock(&mutex)==0 && destroy(&mutex)==0);
    assert((uint32_t)lock(&mutex)==0x80020016);
    mutex=NULL;
    assert(lock(&mutex)==0); /* static initialization */
    assert((uint32_t)lock(&mutex)==0x8002000b);
    assert((uint32_t)trylock(&mutex)==0x80020010);
    assert(unlock(&mutex)==0 && destroy(&mutex)==0);
}
static void posix_mutexes(void) {
    AttrInit init=GET(AttrInit,"dQHWEsJtoE4#libScePosix");
    AttrType type=GET(AttrType,"mDmgMOGVUqg#libScePosix");
    HandleOp destroy_attr=GET(HandleOp,"HF7lK46xzjY#libScePosix");
    typedef int32_t (ABI *PosixInit)(void **,void **);
    PosixInit mutex_init=GET(PosixInit,"ttHNfU+qDBU#libScePosix");
    HandleOp lock=GET(HandleOp,"7H0iTOciTLo#libScePosix");
    HandleOp unlock=GET(HandleOp,"2Z+PpY6CaJg#libScePosix");
    HandleOp trylock=GET(HandleOp,"K-jXhbt2gn4#libScePosix");
    HandleOp destroy=GET(HandleOp,"ltCfaGr2JGE#libScePosix");
    void *a=NULL,*m=NULL;
    assert(init(NULL)==22 && init(&a)==0);
    assert(type(&a,99)==22 && type(&a,2)==0);
    assert(mutex_init(&m,&a)==0 && destroy_attr(&a)==0);
    assert(lock(&m)==0 && trylock(&m)==0 && destroy(&m)==16);
    assert(unlock(&m)==0 && unlock(&m)==0 && destroy(&m)==0);
    m=NULL;
    assert(lock(&m)==0 && lock(&m)==11 && trylock(&m)==16);
    assert(unlock(&m)==0 && destroy(&m)==0);
    assert(!runtime_resolve("dQHWEsJtoE4#libc",0));
}
static void wall_time(void) {
    typedef int (ABI *GetTime)(int64_t *,int32_t *);
    GetTime get=GET(GetTime,"n88vx3C5nW8#libScePosix");
    struct { int64_t value[2]; uint64_t canary; } t={{0,0},0xabcdef};
    struct { int32_t value[2]; uint32_t canary; } z={{0,0},0x123456};
    time_t before=time(NULL);
    assert(get(t.value,z.value)==0);
    time_t after=time(NULL);
    assert(t.value[0]>=before && t.value[0]<=after);
    assert(t.value[1]>=0 && t.value[1]<1000000);
    assert(t.canary==0xabcdef && z.canary==0x123456);
    assert(get(NULL,NULL)==0 && get(NULL,z.value)==0);
    assert(!runtime_resolve("n88vx3C5nW8#libc",0));
}
static void localtime_conversion_abi(void) {
    typedef int32_t (ABI *Convert)(int64_t,int64_t,int64_t *,int32_t *,int32_t *);
    /* The game's generated import names contain this NID; the CTR fallback
     * table compiled into the runtime does not. Supply the same mapping. */
    char names[]="/tmp/ufc8-imports-XXXXXX";
    int fd=mkstemp(names); assert(fd>=0);
    FILE *file=fdopen(fd,"w"); assert(file);
    fputs("0NTHN1NKONI libkernel sceKernelConvertLocaltimeToUtc\n",file);
    assert(fclose(file)==0);
    int loaded=runtime_load_import_names(names);
    assert(unlink(names)==0 && loaded==1);
    Convert convert=GET(Convert,"0NTHN1NKONI#libkernel");
    assert(convert);
    const int64_t hints[]={UINT32_MAX,-1,0,1};
    for (unsigned i=0;i<sizeof(hints)/sizeof(hints[0]);++i) {
        struct { int64_t value; uint64_t canary; } utc={-1,0xabcdef};
        struct { int32_t value[2]; uint32_t canary; } tz={{INT32_MIN,INT32_MIN},0x123456};
        struct { int32_t value; uint32_t canary; } dst={INT32_MIN,0x654321};
        /* Match native libc's five-register call, including tm_isdst=-1 in ESI.
         * The old four-argument import writes to address 0xffffffff here. */
        assert(convert(1705320000,hints[i],&utc.value,tz.value,&dst.value)==0);
        assert(utc.value==INT64_C(1705320000)+(int64_t)tz.value[0]*60);
        /* The last output is the DST offset in seconds (shadPS4's dst_sec), not a flag. */
        assert(tz.value[1]!=INT32_MIN && dst.value==(tz.value[1] ? 3600 : 0));
        assert(utc.canary==0xabcdef && tz.canary==0x123456 && dst.canary==0x654321);
        assert(convert(1705320000,hints[i],&utc.value,NULL,NULL)==0);
        assert((uint32_t)convert(1705320000,hints[i],NULL,NULL,NULL)==0x80020016);
    }
}
static void direct_memory(void) {
    Allocate alloc=GET(Allocate,"rTXw65xmLIA#libkernel");
    Map map=GET(Map,"L-Q3LEjIbgA#libkernel");
    Unmap unmap=GET(Unmap,"cQke9UuBQOk#libkernel");
    Release release=GET(Release,"MBuItvba6z8#libkernel");
    int64_t a=-1,b=-1,c=-1;
    const uint64_t pool=536870912, length=16384;
    assert((uint32_t)alloc(0,pool,1,0,0,&a)==0x80020016 && a==-1);
    assert((uint32_t)alloc(0,8192,length,0,0,&a)==0x80020023);
    assert(alloc(0,pool,length,2097152,0,&a)==0 && a==0);
    assert(alloc(0,pool,length,2097152,0,&b)==0 && b==2097152);
    assert(alloc(0,pool,length,16384,0,&c)==0 && c==16384);
    void *x=NULL,*y=NULL,*invalid=NULL;
    assert((uint32_t)map(&invalid,length*2,3,0,b,0)==0x80020016);
    assert(map(&x,length,3,0,a,2097152)==0 && !((uintptr_t)x%2097152));
    assert(map(&y,length,3,0,a,0)==0);
    assert(*(uint64_t *)x==0);
    *(uint64_t *)x=UINT64_C(0xabcdef0123456789);
    assert(*(uint64_t *)y==UINT64_C(0xabcdef0123456789)); /* real shared backing */
    assert((uint32_t)unmap((char *)x+4096,length)==0x80020016); /* unaligned */
    assert(unmap(x,length)==0 && unmap(y,length)==0);
    assert(release(a,length)==0 && release(b,length)==0 && release(c,length)==0);
    assert(alloc(0,pool,length,0,0,&a)==0 && a==0);
    x=NULL; assert(map(&x,length,3,0,a,0)==0);
    assert(*(uint64_t *)x==0); /* released allocation does not leak previous data */
    assert(release(a,length)==0); /* release also unmaps owned mapping */
}
/* UFC-69: the extent is per thread and cleared before completion callbacks. */
extern void runtime_memory_set_host_copy_hook(void (*hook)(uintptr_t, uint64_t), const uint32_t *);
extern int runtime_memory_host_copy_extent(uintptr_t *, uintptr_t *);
static unsigned host_copy_callbacks;
static void host_copy_finished(uintptr_t address, uint64_t size) {
    uintptr_t begin=0,end=0;
    assert(!runtime_memory_host_copy_extent(&begin,&end));
    assert(address && size);
    ++host_copy_callbacks;
}
static void *host_copy_other_thread(void *unused) {
    (void)unused;
    uintptr_t begin=0,end=0;
    assert(!runtime_memory_host_copy_extent(&begin,&end));
    return NULL;
}
static void host_copy_scope(void) {
    static const uint32_t open_windows=1;
    const char *env=getenv("BB_HOST_COPY_EXTENTS");
    const int enabled=!(env && env[0]=='0');
    runtime_memory_set_host_copy_hook(host_copy_finished,&open_windows);
    uintptr_t begin=0,end=0;
    assert(!runtime_memory_host_copy_extent(&begin,&end));
    runtime_memory_host_copy_begin(0x1000,32);
    assert(runtime_memory_host_copy_extent(&begin,&end)==enabled);
    if (enabled) assert(begin==0x1000 && end==0x1020);
    pthread_t thread;
    assert(pthread_create(&thread,NULL,host_copy_other_thread,NULL)==0);
    assert(pthread_join(thread,NULL)==0);
    runtime_memory_host_copy_end(0x1000,32);
    assert(host_copy_callbacks==(unsigned)enabled);
    assert(!runtime_memory_host_copy_extent(&begin,&end));
    runtime_memory_host_copy_begin(0x1000,0);
    assert(!runtime_memory_host_copy_extent(&begin,&end));
    runtime_memory_host_copy_end(0x1000,0);
}
static void memory_primitives(void) {
    typedef void *(ABI *Set)(void *,int,size_t);
    typedef void *(ABI *Copy)(void *,const void *,size_t);
    typedef int (ABI *Compare)(const void *,const void *,size_t);
    char a[16],b[16];
    assert(GET(Set,"8zTFvBIAIN8#libc")(a,0x5a,sizeof(a))==a);
    assert(GET(Copy,"Q3VBxCXhUHs#libc")(b,a,sizeof(a))==b);
    assert(GET(Compare,"DfivPArhucg#libc")(a,b,sizeof(a))==0);
    memcpy(a,"abcdef",7);
    GET(Copy,"+P6FRGH4LfA#libc")(a+1,a,6);
    assert(memcmp(a,"aabcdef",7)==0);
    typedef size_t (ABI *Length)(const char *);
    Length length=GET(Length,"j4ViWNHEgww#libc");
    assert(length("")==0 && length("abc\0def")==3 && length("\xff\x80")==2);
}

static HandleOp rw_read, rw_write, rw_tryread, rw_trywrite, rw_unlock, rw_destroy;
typedef struct { int64_t seconds, nanoseconds; } TestTime;
typedef int32_t (ABI *TimedLock)(void **, const TestTime *);
static void rw_setup(void) {
    rw_read=GET(HandleOp,"Ox9i0c7L5w0#libkernel"); rw_write=GET(HandleOp,"mqdNorrB+gI#libkernel");
    rw_tryread=GET(HandleOp,"XD3mDeybCnk#libkernel"); rw_trywrite=GET(HandleOp,"bIHoZCTomsI#libkernel");
    rw_unlock=GET(HandleOp,"+L98PIbGttk#libkernel"); rw_destroy=GET(HandleOp,"BB+kb08Tl9A#libkernel");
    assert(rw_read && rw_write && rw_tryread && rw_trywrite && rw_unlock && rw_destroy);
}
static void rw_lifecycle(void) {
    rw_setup();
    MutexInit init=GET(MutexInit,"6ULAa0fq4jA#libkernel");
    void *lock;
    assert((uint32_t)init(NULL,NULL,NULL)==0x80020016);
    assert(init(&lock,NULL,"rw-test")==0);
    assert(rw_read(&lock)==0 && rw_read(&lock)==0);
    assert((uint32_t)rw_trywrite(&lock)==0x80020010);
    assert((uint32_t)rw_write(&lock)==0x8002000b);
    assert((uint32_t)rw_destroy(&lock)==0x80020010);
    assert(rw_unlock(&lock)==0 && rw_unlock(&lock)==0);
    assert((uint32_t)rw_unlock(&lock)==0x80020001);
    assert(rw_write(&lock)==0);
    assert((uint32_t)rw_tryread(&lock)==0x80020010);
    assert((uint32_t)rw_read(&lock)==0x8002000b);
    assert(rw_unlock(&lock)==0 && rw_destroy(&lock)==0);
    assert((uintptr_t)lock==1);
    assert((uint32_t)rw_read(&lock)==0x80020016);
    assert((uint32_t)rw_destroy(&lock)==0x80020016);
    lock=NULL;
    assert(rw_destroy(&lock)==0);
    assert(rw_read(&lock)==0 && rw_unlock(&lock)==0 && rw_destroy(&lock)==0);
    lock=(void *)(uintptr_t)0xdead;
    assert((uint32_t)rw_tryread(&lock)==0x80020016);
}
typedef struct {
    void *lock;
    pthread_barrier_t ready, release, writer_ready;
    _Atomic int value;
} RwFixture;
static void barrier(pthread_barrier_t *b) {
    int e=pthread_barrier_wait(b); assert(e==0 || e==PTHREAD_BARRIER_SERIAL_THREAD);
}
static void *reader_worker(void *context) {
    RwFixture *f=context;
    assert(rw_read(&f->lock)==0);
    barrier(&f->ready); barrier(&f->release);
    assert(atomic_load(&f->value)==0);
    assert(rw_unlock(&f->lock)==0); return NULL;
}
static void *writer_worker(void *context) {
    RwFixture *f=context;
    assert((uint32_t)rw_trywrite(&f->lock)==0x80020010);
    barrier(&f->writer_ready);
    assert(rw_write(&f->lock)==0);
    atomic_store(&f->value,1);
    assert(rw_unlock(&f->lock)==0); return NULL;
}
static void *holding_writer(void *context) {
    RwFixture *f=context;
    assert(rw_write(&f->lock)==0);
    barrier(&f->ready); barrier(&f->release);
    assert(rw_unlock(&f->lock)==0); return NULL;
}
static void fixture_init(RwFixture *f) {
    assert(pthread_barrier_init(&f->ready,NULL,2)==0);
    assert(pthread_barrier_init(&f->release,NULL,2)==0);
    assert(pthread_barrier_init(&f->writer_ready,NULL,2)==0);
}
static void fixture_destroy(RwFixture *f) {
    assert(rw_destroy(&f->lock)==0);
    assert(pthread_barrier_destroy(&f->ready)==0);
    assert(pthread_barrier_destroy(&f->release)==0);
    assert(pthread_barrier_destroy(&f->writer_ready)==0);
}
static void rw_concurrency(void) {
    rw_setup();
    RwFixture f={0}; fixture_init(&f);
    pthread_t reader,writer;
    assert(rw_read(&f.lock)==0);
    assert(pthread_create(&reader,NULL,reader_worker,&f)==0);
    barrier(&f.ready); /* two threads now hold read locks at once */
    assert(rw_unlock(&f.lock)==0);
    assert(pthread_create(&writer,NULL,writer_worker,&f)==0);
    barrier(&f.writer_ready);
    assert((uint32_t)rw_destroy(&f.lock)==0x80020010);
    assert(atomic_load(&f.value)==0);
    barrier(&f.release);
    assert(pthread_join(reader,NULL)==0 && pthread_join(writer,NULL)==0);
    assert(atomic_load(&f.value)==1);
    fixture_destroy(&f);
}
static void rw_timeouts(void) {
    rw_setup();
    TimedLock read_timed=GET(TimedLock,"iPtZRWICjrM#libkernel"), write_timed=GET(TimedLock,"adh--6nIqTk#libkernel");
    RwFixture f={0}; fixture_init(&f);
    pthread_t writer; assert(pthread_create(&writer,NULL,holding_writer,&f)==0);
    barrier(&f.ready);
    TestTime expired={0,0},invalid={0,1000000000};
    assert((uint32_t)read_timed(&f.lock,&expired)==0x8002003c);
    assert((uint32_t)write_timed(&f.lock,&expired)==0x8002003c);
    assert((uint32_t)read_timed(&f.lock,&invalid)==0x80020016);
    assert((uint32_t)rw_unlock(&f.lock)==0x80020001);
    barrier(&f.release); assert(pthread_join(writer,NULL)==0);
    assert(read_timed(&f.lock,&invalid)==0); /* available lock ignores deadline */
    assert(rw_unlock(&f.lock)==0);
    fixture_destroy(&f);
}
/* UFC-57: a UI scene built more than the budget since the last guest flip waits for the next
 * flip, or for the timeout (and then the following build waits again). */
static uint64_t ms_since(const struct timespec *t0) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)((t.tv_sec-t0->tv_sec)*1000+(t.tv_nsec-t0->tv_nsec)/1000000);
}
static void *late_flip(void *arg) { (void)arg; usleep(10000); runtime_ui_guest_flip(); return NULL; }
static void ui_backpressure(void) {
    static const unsigned char call[]={0x49,0x8b,0x7d,0x48,0x48,0x83,0xc7,0x10,0x48,0x89,0x7d,0xa8,0xe8,0xfb,0xcd,0x3b,0x00};
    const uint64_t size=0x1a3d200;
    unsigned char *image=calloc(1,size); assert(image);
    runtime_ui_backpressure_init(image,size); assert(!runtime_ui_build_site); /* another executable */
    memcpy(image+0x1a3d134,call,sizeof(call));
    setenv("BB_UI_BACKPRESSURE_BUILDS","2",1); setenv("BB_UI_BACKPRESSURE_MS","50",1);
    runtime_ui_backpressure_init(image,size); assert(runtime_ui_build_site==(uintptr_t)image+0x1a3d145);
    int scene, other; struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC,&t0);
    for (int i=0;i<10;++i) runtime_ui_before_build(&scene); /* no flip yet: no renderer to wait for */
    runtime_ui_guest_flip();
    runtime_ui_before_build(&scene); runtime_ui_before_build(&scene); runtime_ui_before_build(&other);
    assert(ms_since(&t0)<40);
    runtime_ui_before_build(&scene); assert(ms_since(&t0)>=45); /* third build: the 50 ms timeout */
    clock_gettime(CLOCK_MONOTONIC,&t0);
    runtime_ui_before_build(&scene); assert(ms_since(&t0)>=45); /* still no flip: waits again */
    pthread_t flipper; assert(pthread_create(&flipper,NULL,late_flip,NULL)==0);
    clock_gettime(CLOCK_MONOTONIC,&t0);
    runtime_ui_before_build(&scene); assert(ms_since(&t0)<40); /* the flip ended the wait */
    assert(pthread_join(flipper,NULL)==0);
    runtime_ui_before_build(&scene); assert(ms_since(&t0)<40); /* first build since that flip... */
    runtime_ui_before_build(&scene); assert(ms_since(&t0)>=45); /* ...then the budget again */
    runtime_ui_build_site=0; free(image);
}
/* UFC-58: a take that would leave the appearance pool's publisher no node, or a publish that would
 * find none, waits until the retire (seen on another thread) frees nodes, at most the timeout;
 * after a wait that ran out that pool goes on without waiting until it has room again. */
static _Alignas(8) unsigned char appearance_obj[0x200], appearance_nodes[4*0x38];
static void appearance_link(int count, int taken) { /* the oldest `count` nodes listed, the first `taken` taken */
    const uintptr_t sentinel=(uintptr_t)appearance_obj+0x188;
    uint64_t *obj=(uint64_t *)appearance_obj;
    uintptr_t prev=sentinel;
    for (int i=0;i<count;++i) {
        unsigned char *node=appearance_nodes+i*0x38;
        memcpy(node+8,&prev,8);
        const uint64_t seq=10+i; memcpy(node+0x10,&seq,8);
        node[0x30]=i<taken;
        const uintptr_t next=i+1<count ? (uintptr_t)(node+0x38) : sentinel;
        memcpy(node,&next,8);
        prev=(uintptr_t)node;
    }
    obj[0x188/8]=count ? (uintptr_t)appearance_nodes : sentinel;
    obj[0x190/8]=count ? prev : sentinel;
    obj[0x180/8]=taken ? 10+taken-1 : 0; /* last sequence taken */
    obj[0x198/8]=(uint64_t)count;
    obj[0x1a0/8]=0; /* free list */
    obj[0x1a8/8]=(uintptr_t)appearance_nodes+(uint64_t)count*0x38; /* cursor */
    obj[0x1b0/8]=(uintptr_t)appearance_nodes+4*0x38; /* end */
    obj[0x1b8/8]=0x38;
}
static void *appearance_retire(void *arg) {
    if (arg) usleep(20000);
    runtime_appearance_pool_lock(RUNTIME_APPEARANCE_RETIRE,(uintptr_t)appearance_obj);
    if (arg) appearance_link(1,1); /* everything before the newest taken result retired */
    return NULL;
}
static void appearance_backpressure(void) {
    const uintptr_t pool=(uintptr_t)appearance_obj;
    struct timespec t0; pthread_t retirer;
    appearance_link(4,3); /* three held, the newest untaken: taking it leaves the publisher nothing */
    clock_gettime(CLOCK_MONOTONIC,&t0);
    runtime_appearance_pool_lock(RUNTIME_APPEARANCE_TAKE,pool); assert(ms_since(&t0)<40); /* no retire seen yet */
    runtime_appearance_pool_lock(RUNTIME_APPEARANCE_RETIRE,pool);
    runtime_appearance_pool_lock(RUNTIME_APPEARANCE_TAKE,pool); assert(ms_since(&t0)<40); /* retire on this thread */
    assert(pthread_create(&retirer,NULL,appearance_retire,NULL)==0); assert(pthread_join(retirer,NULL)==0);
    appearance_link(4,2);
    runtime_appearance_pool_lock(RUNTIME_APPEARANCE_TAKE,pool); assert(ms_since(&t0)<40); /* two held: room enough */
    appearance_link(4,3);
    assert(pthread_create(&retirer,NULL,appearance_retire,(void *)1)==0);
    runtime_appearance_pool_lock(RUNTIME_APPEARANCE_TAKE,pool); /* waits for that retire */
    assert(pthread_join(retirer,NULL)==0);
    assert(ms_since(&t0)>=15 && ms_since(&t0)<200);
    appearance_link(4,4); /* nothing left for the publisher, and no retire comes: the timeout */
    clock_gettime(CLOCK_MONOTONIC,&t0);
    runtime_appearance_pool_lock(RUNTIME_APPEARANCE_PUBLISH,pool); assert(ms_since(&t0)>=240);
    clock_gettime(CLOCK_MONOTONIC,&t0);
    runtime_appearance_pool_lock(RUNTIME_APPEARANCE_PUBLISH,pool); assert(ms_since(&t0)<40); /* gave up on this pool */
    appearance_link(2,1);
    runtime_appearance_pool_lock(RUNTIME_APPEARANCE_PUBLISH,pool); /* room again: waits come back */
    appearance_link(4,4);
    assert(pthread_create(&retirer,NULL,appearance_retire,(void *)1)==0);
    clock_gettime(CLOCK_MONOTONIC,&t0);
    runtime_appearance_pool_lock(RUNTIME_APPEARANCE_PUBLISH,pool);
    assert(pthread_join(retirer,NULL)==0);
    assert(ms_since(&t0)>=15 && ms_since(&t0)<200);
}
int main(int argc,char **argv) {
    runtime_start(0); assert(runtime_resolve("bzQExy189ZI#libc",0)==0);
    runtime_start(1);
    assert(runtime_resolve("bzQExy189ZI#wrong#module",0)==0);
    assert(runtime_resolve("unknown",0)==0);
    assert(runtime_resolve("8zTFvBIAIN8#libc",1)==0);
    assert(runtime_resolve("f7uOxY9mM1U#libkernel",0)==0);
    uint64_t *canary=(uint64_t *)runtime_resolve("f7uOxY9mM1U#libkernel",1);
    assert(canary && *canary && !(*canary & 255));
    if (argc>1 && !strcmp(argv[1],"--guard-recursion")) {
        uint64_t guard=0; Acquire acquire=GET(Acquire,"3GPpjQdAMTw#libc");
        acquire(&guard); acquire(&guard); return 99;
    }
    if (argc>1 && !strcmp(argv[1],"--bad-tls")) {
        static const char data[4]={0};
        runtime_set_libc_tls(data,4,8);
        const uint64_t index[]={2,8};
        GET(TlsAddress,"vNe1w4diLCs#libkernel")(index); return 99;
    }
    if (argc>1 && !strcmp(argv[1],"--stack-failure")) {
        GET(GuestCallback,"Ou3iL1abvng#libkernel")(); return 99;
    }
    if (argc>1 && !strcmp(argv[1],"--rwlock-lifecycle")) { rw_lifecycle(); return 0; }
    if (argc>1 && !strcmp(argv[1],"--rwlock-concurrency")) { rw_concurrency(); return 0; }
    if (argc>1 && !strcmp(argv[1],"--rwlock-timeouts")) { rw_timeouts(); return 0; }
    libc_support(); posix_mutexes(); wall_time(); localtime_conversion_abi(); exit_handlers(); guards(); mutexes(); direct_memory(); host_copy_scope(); memory_primitives();
    assert(host_copy_callbacks==((getenv("BB_HOST_COPY_EXTENTS") && getenv("BB_HOST_COPY_EXTENTS")[0]=='0') ? 0u : 4u));
    runtime_memory_set_host_copy_hook(NULL,NULL);
    rw_lifecycle(); rw_concurrency(); rw_timeouts(); ui_backpressure(); appearance_backpressure();
    puts("PASS: callback lifecycle, guard ABI, mutex errors, shared direct memory, memory primitives, resolver scope, UI back-pressure, appearance back-pressure");
    return 0;
}
