// libkernel threads and synchronization (scePthread* and the POSIX names).
//
// Guest pthread objects are FreeBSD-style: every pthread_*_t is a pointer.
// We allocate the host object on init and store our pointer in the guest's
// slot. A null slot (or 1 for the adaptive mutex initializer) is a static
// initializer and is turned into a real object on first use.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include "common/log.h"
#include "core/tls.h"
#include "hle/hle.h"
#include "hle/kernel/kernel.h"

namespace Hle::Kernel {

namespace {

// ---------------------------------------------------------------- mutex ----

enum GuestMutexType : int {
    kMutexErrorCheck = 1,
    kMutexRecursive = 2,
    kMutexNormal = 3,
    kMutexAdaptive = 4,
};

struct MutexAttr {
    int type = kMutexErrorCheck;
    int protocol = 0;
};

struct Mutex {
    pthread_mutex_t m;
    std::string name;

    explicit Mutex(int type, const char* n = nullptr) : name(n ? n : "") {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        int host = PTHREAD_MUTEX_ERRORCHECK;
        if (type == kMutexRecursive) {
            host = PTHREAD_MUTEX_RECURSIVE;
        } else if (type == kMutexNormal || type == kMutexAdaptive) {
            host = PTHREAD_MUTEX_NORMAL;
        }
        pthread_mutexattr_settype(&a, host);
        pthread_mutex_init(&m, &a);
        pthread_mutexattr_destroy(&a);
    }
    ~Mutex() { pthread_mutex_destroy(&m); }
};

// Resolves a guest slot that may still hold a static initializer.
template <typename T, typename Make>
T* Resolve(T** slot, Make make) {
    T* cur = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (reinterpret_cast<uintptr_t>(cur) > 1) {
        return cur;
    }
    T* fresh = make(reinterpret_cast<uintptr_t>(cur));
    if (__atomic_compare_exchange_n(slot, &cur, fresh, false, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE)) {
        return fresh;
    }
    delete fresh;
    return cur;
}

Mutex* GetMutex(Mutex** slot) {
    if (!slot) {
        return nullptr;
    }
    return Resolve(slot, [](uintptr_t init) {
        return new Mutex(init == 1 ? kMutexAdaptive : kMutexErrorCheck);
    });
}

int posix_pthread_mutexattr_init(MutexAttr** attr) {
    *attr = new MutexAttr();
    return 0;
}

int posix_pthread_mutexattr_destroy(MutexAttr** attr) {
    delete *attr;
    *attr = nullptr;
    return 0;
}

int posix_pthread_mutexattr_settype(MutexAttr** attr, int type) {
    if (type < kMutexErrorCheck || type > kMutexAdaptive) {
        return EINVAL;
    }
    (*attr)->type = type;
    return 0;
}

int posix_pthread_mutexattr_gettype(MutexAttr** attr, int* type) {
    *type = (*attr)->type;
    return 0;
}

int posix_pthread_mutexattr_setprotocol(MutexAttr** attr, int protocol) {
    (*attr)->protocol = protocol;
    return 0;
}

int posix_pthread_mutexattr_setpshared(MutexAttr**, int) {
    return 0;
}

int MutexInit(Mutex** mutex, MutexAttr** attr, const char* name) {
    const int type = (attr && *attr) ? (*attr)->type : kMutexErrorCheck;
    *mutex = new Mutex(type, name);
    return 0;
}

int posix_pthread_mutex_init(Mutex** mutex, MutexAttr** attr) {
    return MutexInit(mutex, attr, nullptr);
}

int posix_pthread_mutex_destroy(Mutex** mutex) {
    if (reinterpret_cast<uintptr_t>(*mutex) > 1) {
        delete *mutex;
    }
    *mutex = nullptr;
    return 0;
}

int posix_pthread_mutex_lock(Mutex** mutex) {
    return pthread_mutex_lock(&GetMutex(mutex)->m);
}

int posix_pthread_mutex_trylock(Mutex** mutex) {
    return pthread_mutex_trylock(&GetMutex(mutex)->m);
}

int posix_pthread_mutex_unlock(Mutex** mutex) {
    return pthread_mutex_unlock(&GetMutex(mutex)->m);
}

// macOS has no pthread_mutex_timedlock: poll with short sleeps.
int TimedLock(Mutex* m, u64 timeout_us) {
    const u64 deadline = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + timeout_us * 1000;
    for (;;) {
        const int r = pthread_mutex_trylock(&m->m);
        if (r != EBUSY) {
            return r;
        }
        if (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= deadline) {
            return ETIMEDOUT;
        }
        usleep(50);
    }
}

s32 scePthreadMutexattrInit(MutexAttr** a) { return OrbisError(posix_pthread_mutexattr_init(a)); }
s32 scePthreadMutexattrDestroy(MutexAttr** a) { return OrbisError(posix_pthread_mutexattr_destroy(a)); }
s32 scePthreadMutexattrSettype(MutexAttr** a, int t) { return OrbisError(posix_pthread_mutexattr_settype(a, t)); }
s32 scePthreadMutexattrGettype(MutexAttr** a, int* t) { return OrbisError(posix_pthread_mutexattr_gettype(a, t)); }
s32 scePthreadMutexattrSetprotocol(MutexAttr** a, int p) { return OrbisError(posix_pthread_mutexattr_setprotocol(a, p)); }
s32 scePthreadMutexInit(Mutex** m, MutexAttr** a, const char* name) { return OrbisError(MutexInit(m, a, name)); }
s32 scePthreadMutexDestroy(Mutex** m) { return OrbisError(posix_pthread_mutex_destroy(m)); }
s32 scePthreadMutexLock(Mutex** m) { return OrbisError(posix_pthread_mutex_lock(m)); }
s32 scePthreadMutexTrylock(Mutex** m) { return OrbisError(posix_pthread_mutex_trylock(m)); }
s32 scePthreadMutexUnlock(Mutex** m) { return OrbisError(posix_pthread_mutex_unlock(m)); }
s32 scePthreadMutexTimedlock(Mutex** m, u32 usec) { return OrbisError(TimedLock(GetMutex(m), usec)); }

// ----------------------------------------------------------------- cond ----

struct CondAttr {
    int clock = CLOCK_REALTIME;
};

struct Cond {
    pthread_cond_t c;
    int clock = CLOCK_REALTIME;

    Cond() { pthread_cond_init(&c, nullptr); }
    ~Cond() { pthread_cond_destroy(&c); }
};

Cond* GetCond(Cond** slot) {
    return Resolve(slot, [](uintptr_t) { return new Cond(); });
}

int posix_pthread_condattr_init(CondAttr** a) {
    *a = new CondAttr();
    return 0;
}

int posix_pthread_condattr_destroy(CondAttr** a) {
    delete *a;
    *a = nullptr;
    return 0;
}

int posix_pthread_condattr_setclock(CondAttr** a, int clock) {
    (*a)->clock = clock;
    return 0;
}

int posix_pthread_cond_init(Cond** cond, CondAttr** attr) {
    auto* c = new Cond();
    if (attr && *attr) {
        c->clock = (*attr)->clock;
    }
    *cond = c;
    return 0;
}

int posix_pthread_cond_destroy(Cond** cond) {
    if (*cond) {
        delete *cond;
    }
    *cond = nullptr;
    return 0;
}

int posix_pthread_cond_wait(Cond** cond, Mutex** mutex) {
    return pthread_cond_wait(&GetCond(cond)->c, &GetMutex(mutex)->m);
}

int WaitRelative(Cond* c, Mutex* m, u64 ns) {
    const timespec rel{static_cast<time_t>(ns / 1000000000), static_cast<long>(ns % 1000000000)};
    return pthread_cond_timedwait_relative_np(&c->c, &m->m, &rel);
}

// Guest clock ids (FreeBSD): 0 realtime, 4 monotonic.
u64 NowNs(int guest_clock) {
    return clock_gettime_nsec_np(guest_clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC);
}

int posix_pthread_cond_timedwait(Cond** cond, Mutex** mutex, const timespec* abstime) {
    Cond* c = GetCond(cond);
    const u64 target = u64(abstime->tv_sec) * 1000000000 + u64(abstime->tv_nsec);
    const u64 now = NowNs(c->clock == CLOCK_REALTIME ? 0 : 4);
    return WaitRelative(c, GetMutex(mutex), target > now ? target - now : 0);
}

int posix_pthread_cond_signal(Cond** cond) {
    return pthread_cond_signal(&GetCond(cond)->c);
}

int posix_pthread_cond_broadcast(Cond** cond) {
    return pthread_cond_broadcast(&GetCond(cond)->c);
}

s32 scePthreadCondattrInit(CondAttr** a) { return OrbisError(posix_pthread_condattr_init(a)); }
s32 scePthreadCondattrDestroy(CondAttr** a) { return OrbisError(posix_pthread_condattr_destroy(a)); }
s32 scePthreadCondInit(Cond** c, CondAttr** a, const char*) { return OrbisError(posix_pthread_cond_init(c, a)); }
s32 scePthreadCondDestroy(Cond** c) { return OrbisError(posix_pthread_cond_destroy(c)); }
s32 scePthreadCondWait(Cond** c, Mutex** m) { return OrbisError(posix_pthread_cond_wait(c, m)); }
s32 scePthreadCondSignal(Cond** c) { return OrbisError(posix_pthread_cond_signal(c)); }
s32 scePthreadCondBroadcast(Cond** c) { return OrbisError(posix_pthread_cond_broadcast(c)); }
s32 scePthreadCondTimedwait(Cond** c, Mutex** m, u32 usec) {
    return OrbisError(WaitRelative(GetCond(c), GetMutex(m), u64(usec) * 1000));
}

// --------------------------------------------------------------- rwlock ----

struct RwLock {
    pthread_rwlock_t l;
    RwLock() { pthread_rwlock_init(&l, nullptr); }
    ~RwLock() { pthread_rwlock_destroy(&l); }
};

RwLock* GetRwLock(RwLock** slot) {
    return Resolve(slot, [](uintptr_t) { return new RwLock(); });
}

int posix_pthread_rwlock_init(RwLock** l, void*) {
    *l = new RwLock();
    return 0;
}

int posix_pthread_rwlock_destroy(RwLock** l) {
    delete *l;
    *l = nullptr;
    return 0;
}

int posix_pthread_rwlock_rdlock(RwLock** l) { return pthread_rwlock_rdlock(&GetRwLock(l)->l); }
int posix_pthread_rwlock_wrlock(RwLock** l) { return pthread_rwlock_wrlock(&GetRwLock(l)->l); }
int posix_pthread_rwlock_tryrdlock(RwLock** l) { return pthread_rwlock_tryrdlock(&GetRwLock(l)->l); }
int posix_pthread_rwlock_trywrlock(RwLock** l) { return pthread_rwlock_trywrlock(&GetRwLock(l)->l); }
int posix_pthread_rwlock_unlock(RwLock** l) { return pthread_rwlock_unlock(&GetRwLock(l)->l); }

s32 scePthreadRwlockInit(RwLock** l, void* a, const char*) { return OrbisError(posix_pthread_rwlock_init(l, a)); }
s32 scePthreadRwlockDestroy(RwLock** l) { return OrbisError(posix_pthread_rwlock_destroy(l)); }
s32 scePthreadRwlockRdlock(RwLock** l) { return OrbisError(posix_pthread_rwlock_rdlock(l)); }
s32 scePthreadRwlockWrlock(RwLock** l) { return OrbisError(posix_pthread_rwlock_wrlock(l)); }
s32 scePthreadRwlockTryrdlock(RwLock** l) { return OrbisError(posix_pthread_rwlock_tryrdlock(l)); }
s32 scePthreadRwlockTrywrlock(RwLock** l) { return OrbisError(posix_pthread_rwlock_trywrlock(l)); }
s32 scePthreadRwlockUnlock(RwLock** l) { return OrbisError(posix_pthread_rwlock_unlock(l)); }

// --------------------------------------------------------------- thread ----

constexpr size_t kMinStack = 512 * 1024;
constexpr int kMaxKeys = 256;

struct ThreadAttr {
    size_t stack_size = 0x10000;
    int detach = 0; // PTHREAD_CREATE_JOINABLE
    int priority = 700;
    int policy = 1;
    int inherit = 4;
    u64 affinity = 0x7F;
    size_t guard = 0x1000;
};

using GuestEntry = void* (*)(void*);
using GuestDestructor = void (*)(void*);

struct Thread {
    pthread_t host{};
    GuestEntry entry = nullptr;
    void* arg = nullptr;
    std::string name;
    ThreadAttr attr;
    int id = 0;
    void* key_values[kMaxKeys] = {};
};

std::atomic<int> g_next_thread_id{1};
thread_local Thread* t_self = nullptr;

struct KeySlot {
    std::atomic<bool> used{false};
    GuestDestructor destructor = nullptr;
};
KeySlot g_keys[kMaxKeys];

Thread* Self() {
    if (!t_self) {
        // A thread we did not create (the guest main thread).
        t_self = new Thread();
        t_self->host = pthread_self();
        t_self->name = "main";
        t_self->id = g_next_thread_id++;
    }
    return t_self;
}

// POSIX key destructors, run while guest TLS is still alive.
void RunKeyDestructors(Thread* t) {
    for (int round = 0; round < 4; ++round) {
        bool any = false;
        for (int k = 0; k < kMaxKeys; ++k) {
            void* v = t->key_values[k];
            if (v && g_keys[k].used && g_keys[k].destructor) {
                t->key_values[k] = nullptr;
                g_keys[k].destructor(v);
                any = true;
            }
        }
        if (!any) {
            break;
        }
    }
}

void* HostThreadStart(void* raw) {
    auto* t = static_cast<Thread*>(raw);
    t_self = t;
    t->host = pthread_self();
    pthread_setname_np(t->name.c_str());
    Core::Tls::Tcb* tcb = Core::Tls::SetupThread();
    tcb->thread = t;

    void* ret = t->entry(t->arg);

    RunKeyDestructors(t);
    Core::Tls::TeardownThread();
    return ret;
}

int posix_pthread_attr_init(ThreadAttr** a) {
    *a = new ThreadAttr();
    return 0;
}

int posix_pthread_attr_destroy(ThreadAttr** a) {
    delete *a;
    *a = nullptr;
    return 0;
}

int posix_pthread_attr_setstacksize(ThreadAttr** a, size_t s) {
    (*a)->stack_size = s;
    return 0;
}

int posix_pthread_attr_getstacksize(ThreadAttr** a, size_t* s) {
    *s = (*a)->stack_size;
    return 0;
}

int posix_pthread_attr_setdetachstate(ThreadAttr** a, int d) {
    (*a)->detach = d;
    return 0;
}

int posix_pthread_attr_getdetachstate(ThreadAttr** a, int* d) {
    *d = (*a)->detach;
    return 0;
}

int posix_pthread_attr_setguardsize(ThreadAttr** a, size_t g) {
    (*a)->guard = g;
    return 0;
}

int posix_pthread_attr_setinheritsched(ThreadAttr** a, int i) {
    (*a)->inherit = i;
    return 0;
}

int posix_pthread_attr_setschedpolicy(ThreadAttr** a, int p) {
    (*a)->policy = p;
    return 0;
}

struct GuestSchedParam {
    int priority;
};

int posix_pthread_attr_setschedparam(ThreadAttr** a, const GuestSchedParam* p) {
    (*a)->priority = p->priority;
    return 0;
}

int posix_pthread_attr_getschedparam(ThreadAttr** a, GuestSchedParam* p) {
    p->priority = (*a)->priority;
    return 0;
}

s32 scePthreadAttrSetaffinity(ThreadAttr** a, u64 mask) {
    (*a)->affinity = mask;
    return ORBIS_OK;
}

s32 scePthreadAttrGetaffinity(ThreadAttr** a, u64* mask) {
    *mask = (*a)->affinity;
    return ORBIS_OK;
}

int CreateThread(Thread** out, ThreadAttr** attr, GuestEntry entry, void* arg, const char* name) {
    auto* t = new Thread();
    t->entry = entry;
    t->arg = arg;
    t->name = name ? name : "guest";
    t->id = g_next_thread_id++;
    if (attr && *attr) {
        t->attr = **attr;
    }

    pthread_attr_t host_attr;
    pthread_attr_init(&host_attr);
    pthread_attr_setstacksize(&host_attr,
                              AlignUp(std::max(t->attr.stack_size, kMinStack), 0x4000));
    if (t->attr.detach) {
        pthread_attr_setdetachstate(&host_attr, PTHREAD_CREATE_DETACHED);
    }
    *out = t; // visible before the thread runs, like on FreeBSD
    const int r = pthread_create(&t->host, &host_attr, HostThreadStart, t);
    pthread_attr_destroy(&host_attr);
    if (r != 0) {
        *out = nullptr;
        delete t;
        return r;
    }
    DC_INFO("thread created: %s (id %d)", t->name.c_str(), t->id);
    return 0;
}

int posix_pthread_create(Thread** out, ThreadAttr** attr, GuestEntry entry, void* arg) {
    return CreateThread(out, attr, entry, arg, nullptr);
}

int posix_pthread_create_name_np(Thread** out, ThreadAttr** attr, GuestEntry entry, void* arg,
                                 const char* name) {
    return CreateThread(out, attr, entry, arg, name);
}

int posix_pthread_join(Thread* t, void** ret) {
    return pthread_join(t->host, ret);
}

int posix_pthread_detach(Thread* t) {
    return pthread_detach(t->host);
}

[[noreturn]] void posix_pthread_exit(void* value) {
    Thread* t = Self();
    RunKeyDestructors(t);
    Core::Tls::TeardownThread();
    pthread_exit(value);
}

Thread* posix_pthread_self() {
    return Self();
}

int posix_pthread_equal(Thread* a, Thread* b) {
    return a == b;
}

int posix_pthread_yield() {
    sched_yield();
    return 0;
}

int posix_sched_yield() {
    sched_yield();
    return 0;
}

int posix_pthread_key_create(int* key, GuestDestructor destructor) {
    for (int k = 0; k < kMaxKeys; ++k) {
        bool expected = false;
        if (g_keys[k].used.compare_exchange_strong(expected, true)) {
            g_keys[k].destructor = destructor;
            *key = k;
            return 0;
        }
    }
    return EAGAIN;
}

int posix_pthread_key_delete(int key) {
    if (key < 0 || key >= kMaxKeys) {
        return EINVAL;
    }
    g_keys[key].used = false;
    return 0;
}

void* posix_pthread_getspecific(int key) {
    return (key >= 0 && key < kMaxKeys) ? Self()->key_values[key] : nullptr;
}

int posix_pthread_setspecific(int key, const void* value) {
    if (key < 0 || key >= kMaxKeys) {
        return EINVAL;
    }
    Self()->key_values[key] = const_cast<void*>(value);
    return 0;
}

// FreeBSD pthread_once_t: { int state; pthread_mutex_t mutex; }
struct GuestOnce {
    int state;
    void* mutex;
};

int posix_pthread_once(GuestOnce* once, void (*init)()) {
    static std::mutex lock;
    static pthread_cond_t done = PTHREAD_COND_INITIALIZER;
    enum { kNever = 0, kDone = 1, kRunning = 2 };

    if (__atomic_load_n(&once->state, __ATOMIC_ACQUIRE) == kDone) {
        return 0;
    }
    std::unique_lock lk(lock);
    while (once->state == kRunning) {
        pthread_cond_wait(&done, lk.mutex()->native_handle());
    }
    if (once->state == kDone) {
        return 0;
    }
    once->state = kRunning;
    lk.unlock();
    init();
    lk.lock();
    __atomic_store_n(&once->state, kDone, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&done);
    return 0;
}

s32 scePthreadAttrInit(ThreadAttr** a) { return OrbisError(posix_pthread_attr_init(a)); }
s32 scePthreadAttrDestroy(ThreadAttr** a) { return OrbisError(posix_pthread_attr_destroy(a)); }
s32 scePthreadAttrSetstacksize(ThreadAttr** a, size_t s) { return OrbisError(posix_pthread_attr_setstacksize(a, s)); }
s32 scePthreadAttrGetstacksize(ThreadAttr** a, size_t* s) { return OrbisError(posix_pthread_attr_getstacksize(a, s)); }
s32 scePthreadAttrSetdetachstate(ThreadAttr** a, int d) { return OrbisError(posix_pthread_attr_setdetachstate(a, d)); }
s32 scePthreadAttrGetdetachstate(ThreadAttr** a, int* d) { return OrbisError(posix_pthread_attr_getdetachstate(a, d)); }
s32 scePthreadAttrSetguardsize(ThreadAttr** a, size_t g) { return OrbisError(posix_pthread_attr_setguardsize(a, g)); }
s32 scePthreadAttrSetinheritsched(ThreadAttr** a, int i) { return OrbisError(posix_pthread_attr_setinheritsched(a, i)); }
s32 scePthreadAttrSetschedpolicy(ThreadAttr** a, int p) { return OrbisError(posix_pthread_attr_setschedpolicy(a, p)); }
s32 scePthreadAttrSetschedparam(ThreadAttr** a, const GuestSchedParam* p) { return OrbisError(posix_pthread_attr_setschedparam(a, p)); }
s32 scePthreadAttrGetschedparam(ThreadAttr** a, GuestSchedParam* p) { return OrbisError(posix_pthread_attr_getschedparam(a, p)); }

s32 scePthreadAttrGet(Thread* t, ThreadAttr** a) {
    **a = t->attr;
    return ORBIS_OK;
}

s32 scePthreadCreate(Thread** out, ThreadAttr** attr, GuestEntry entry, void* arg,
                     const char* name) {
    return OrbisError(CreateThread(out, attr, entry, arg, name));
}

s32 scePthreadJoin(Thread* t, void** ret) { return OrbisError(posix_pthread_join(t, ret)); }
s32 scePthreadDetach(Thread* t) { return OrbisError(posix_pthread_detach(t)); }
[[noreturn]] void scePthreadExit(void* value) { posix_pthread_exit(value); }
Thread* scePthreadSelf() { return Self(); }
int scePthreadEqual(Thread* a, Thread* b) { return a == b; }
void scePthreadYield() { sched_yield(); }
int scePthreadGetthreadid() { return Self()->id; }

s32 scePthreadRename(Thread* t, const char* name) {
    t->name = name ? name : "";
    if (t == Self()) {
        pthread_setname_np(t->name.c_str());
    }
    return ORBIS_OK;
}

s32 scePthreadGetname(Thread* t, char* name) {
    std::strncpy(name, t->name.c_str(), 32);
    name[31] = '\0';
    return ORBIS_OK;
}

// Scheduling hints: recorded, not enforced. macOS has no hard CPU affinity.
s32 scePthreadSetprio(Thread* t, int prio) {
    t->attr.priority = prio;
    return ORBIS_OK;
}

s32 scePthreadGetprio(Thread* t, int* prio) {
    *prio = t->attr.priority;
    return ORBIS_OK;
}

s32 scePthreadSetaffinity(Thread* t, u64 mask) {
    t->attr.affinity = mask;
    return ORBIS_OK;
}

s32 scePthreadGetaffinity(Thread* t, u64* mask) {
    *mask = t->attr.affinity;
    return ORBIS_OK;
}

s32 scePthreadSetschedparam(Thread* t, int policy, const GuestSchedParam* p) {
    t->attr.policy = policy;
    t->attr.priority = p->priority;
    return ORBIS_OK;
}

s32 scePthreadGetschedparam(Thread* t, int* policy, GuestSchedParam* p) {
    *policy = t->attr.policy;
    p->priority = t->attr.priority;
    return ORBIS_OK;
}

s32 scePthreadKeyCreate(int* key, GuestDestructor d) { return OrbisError(posix_pthread_key_create(key, d)); }
s32 scePthreadKeyDelete(int key) { return OrbisError(posix_pthread_key_delete(key)); }
void* scePthreadGetspecific(int key) { return posix_pthread_getspecific(key); }
s32 scePthreadSetspecific(int key, const void* v) { return OrbisError(posix_pthread_setspecific(key, v)); }
s32 scePthreadOnce(GuestOnce* once, void (*init)()) { return OrbisError(posix_pthread_once(once, init)); }

} // namespace

void RegisterPthread() {
    // POSIX names exist in both libkernel and libScePosix.
    for (const char* lib : {"libkernel", "libScePosix"}) {
        DC_HLE_AS(lib, "pthread_mutexattr_init", posix_pthread_mutexattr_init);
        DC_HLE_AS(lib, "pthread_mutexattr_destroy", posix_pthread_mutexattr_destroy);
        DC_HLE_AS(lib, "pthread_mutexattr_settype", posix_pthread_mutexattr_settype);
        DC_HLE_AS(lib, "pthread_mutexattr_gettype", posix_pthread_mutexattr_gettype);
        DC_HLE_AS(lib, "pthread_mutexattr_setprotocol", posix_pthread_mutexattr_setprotocol);
        DC_HLE_AS(lib, "pthread_mutexattr_setpshared", posix_pthread_mutexattr_setpshared);
        DC_HLE_AS(lib, "pthread_mutex_init", posix_pthread_mutex_init);
        DC_HLE_AS(lib, "pthread_mutex_destroy", posix_pthread_mutex_destroy);
        DC_HLE_AS(lib, "pthread_mutex_lock", posix_pthread_mutex_lock);
        DC_HLE_AS(lib, "pthread_mutex_trylock", posix_pthread_mutex_trylock);
        DC_HLE_AS(lib, "pthread_mutex_unlock", posix_pthread_mutex_unlock);
        DC_HLE_AS(lib, "pthread_condattr_init", posix_pthread_condattr_init);
        DC_HLE_AS(lib, "pthread_condattr_destroy", posix_pthread_condattr_destroy);
        DC_HLE_AS(lib, "pthread_condattr_setclock", posix_pthread_condattr_setclock);
        DC_HLE_AS(lib, "pthread_cond_init", posix_pthread_cond_init);
        DC_HLE_AS(lib, "pthread_cond_destroy", posix_pthread_cond_destroy);
        DC_HLE_AS(lib, "pthread_cond_wait", posix_pthread_cond_wait);
        DC_HLE_AS(lib, "pthread_cond_timedwait", posix_pthread_cond_timedwait);
        DC_HLE_AS(lib, "pthread_cond_signal", posix_pthread_cond_signal);
        DC_HLE_AS(lib, "pthread_cond_broadcast", posix_pthread_cond_broadcast);
        DC_HLE_AS(lib, "pthread_rwlock_init", posix_pthread_rwlock_init);
        DC_HLE_AS(lib, "pthread_rwlock_destroy", posix_pthread_rwlock_destroy);
        DC_HLE_AS(lib, "pthread_rwlock_rdlock", posix_pthread_rwlock_rdlock);
        DC_HLE_AS(lib, "pthread_rwlock_wrlock", posix_pthread_rwlock_wrlock);
        DC_HLE_AS(lib, "pthread_rwlock_tryrdlock", posix_pthread_rwlock_tryrdlock);
        DC_HLE_AS(lib, "pthread_rwlock_trywrlock", posix_pthread_rwlock_trywrlock);
        DC_HLE_AS(lib, "pthread_rwlock_unlock", posix_pthread_rwlock_unlock);
        DC_HLE_AS(lib, "pthread_attr_init", posix_pthread_attr_init);
        DC_HLE_AS(lib, "pthread_attr_destroy", posix_pthread_attr_destroy);
        DC_HLE_AS(lib, "pthread_attr_setstacksize", posix_pthread_attr_setstacksize);
        DC_HLE_AS(lib, "pthread_attr_getstacksize", posix_pthread_attr_getstacksize);
        DC_HLE_AS(lib, "pthread_attr_setdetachstate", posix_pthread_attr_setdetachstate);
        DC_HLE_AS(lib, "pthread_attr_getdetachstate", posix_pthread_attr_getdetachstate);
        DC_HLE_AS(lib, "pthread_attr_setguardsize", posix_pthread_attr_setguardsize);
        DC_HLE_AS(lib, "pthread_attr_setinheritsched", posix_pthread_attr_setinheritsched);
        DC_HLE_AS(lib, "pthread_attr_setschedpolicy", posix_pthread_attr_setschedpolicy);
        DC_HLE_AS(lib, "pthread_attr_setschedparam", posix_pthread_attr_setschedparam);
        DC_HLE_AS(lib, "pthread_attr_getschedparam", posix_pthread_attr_getschedparam);
        DC_HLE_AS(lib, "pthread_create", posix_pthread_create);
        DC_HLE_AS(lib, "pthread_create_name_np", posix_pthread_create_name_np);
        DC_HLE_AS(lib, "pthread_join", posix_pthread_join);
        DC_HLE_AS(lib, "pthread_detach", posix_pthread_detach);
        DC_HLE_AS(lib, "pthread_exit", posix_pthread_exit);
        DC_HLE_AS(lib, "pthread_self", posix_pthread_self);
        DC_HLE_AS(lib, "pthread_equal", posix_pthread_equal);
        DC_HLE_AS(lib, "pthread_yield", posix_pthread_yield);
        DC_HLE_AS(lib, "sched_yield", posix_sched_yield);
        DC_HLE_AS(lib, "pthread_key_create", posix_pthread_key_create);
        DC_HLE_AS(lib, "pthread_key_delete", posix_pthread_key_delete);
        DC_HLE_AS(lib, "pthread_getspecific", posix_pthread_getspecific);
        DC_HLE_AS(lib, "pthread_setspecific", posix_pthread_setspecific);
        DC_HLE_AS(lib, "pthread_once", posix_pthread_once);
    }

    constexpr const char* k = "libkernel";
    DC_HLE(k, scePthreadMutexattrInit);
    DC_HLE(k, scePthreadMutexattrDestroy);
    DC_HLE(k, scePthreadMutexattrSettype);
    DC_HLE(k, scePthreadMutexattrGettype);
    DC_HLE(k, scePthreadMutexattrSetprotocol);
    DC_HLE(k, scePthreadMutexInit);
    DC_HLE(k, scePthreadMutexDestroy);
    DC_HLE(k, scePthreadMutexLock);
    DC_HLE(k, scePthreadMutexTrylock);
    DC_HLE(k, scePthreadMutexUnlock);
    DC_HLE(k, scePthreadMutexTimedlock);
    DC_HLE(k, scePthreadCondattrInit);
    DC_HLE(k, scePthreadCondattrDestroy);
    DC_HLE(k, scePthreadCondInit);
    DC_HLE(k, scePthreadCondDestroy);
    DC_HLE(k, scePthreadCondWait);
    DC_HLE(k, scePthreadCondTimedwait);
    DC_HLE(k, scePthreadCondSignal);
    DC_HLE(k, scePthreadCondBroadcast);
    DC_HLE(k, scePthreadRwlockInit);
    DC_HLE(k, scePthreadRwlockDestroy);
    DC_HLE(k, scePthreadRwlockRdlock);
    DC_HLE(k, scePthreadRwlockWrlock);
    DC_HLE(k, scePthreadRwlockTryrdlock);
    DC_HLE(k, scePthreadRwlockTrywrlock);
    DC_HLE(k, scePthreadRwlockUnlock);
    DC_HLE(k, scePthreadAttrInit);
    DC_HLE(k, scePthreadAttrDestroy);
    DC_HLE(k, scePthreadAttrSetstacksize);
    DC_HLE(k, scePthreadAttrGetstacksize);
    DC_HLE(k, scePthreadAttrSetdetachstate);
    DC_HLE(k, scePthreadAttrGetdetachstate);
    DC_HLE(k, scePthreadAttrSetguardsize);
    DC_HLE(k, scePthreadAttrSetinheritsched);
    DC_HLE(k, scePthreadAttrSetschedpolicy);
    DC_HLE(k, scePthreadAttrSetschedparam);
    DC_HLE(k, scePthreadAttrGetschedparam);
    DC_HLE(k, scePthreadAttrSetaffinity);
    DC_HLE(k, scePthreadAttrGetaffinity);
    DC_HLE(k, scePthreadAttrGet);
    DC_HLE(k, scePthreadCreate);
    DC_HLE(k, scePthreadJoin);
    DC_HLE(k, scePthreadDetach);
    DC_HLE(k, scePthreadExit);
    DC_HLE(k, scePthreadSelf);
    DC_HLE(k, scePthreadEqual);
    DC_HLE(k, scePthreadYield);
    DC_HLE(k, scePthreadGetthreadid);
    DC_HLE(k, scePthreadRename);
    DC_HLE(k, scePthreadGetname);
    DC_HLE(k, scePthreadSetprio);
    DC_HLE(k, scePthreadGetprio);
    DC_HLE(k, scePthreadSetaffinity);
    DC_HLE(k, scePthreadGetaffinity);
    DC_HLE(k, scePthreadSetschedparam);
    DC_HLE(k, scePthreadGetschedparam);
    DC_HLE(k, scePthreadKeyCreate);
    DC_HLE(k, scePthreadKeyDelete);
    DC_HLE(k, scePthreadGetspecific);
    DC_HLE(k, scePthreadSetspecific);
    DC_HLE(k, scePthreadOnce);
}

} // namespace Hle::Kernel
