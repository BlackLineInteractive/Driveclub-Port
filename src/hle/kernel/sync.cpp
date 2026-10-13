// libkernel semaphores and event queues.

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>

#include "common/log.h"
#include "hle/hle.h"
#include "hle/kernel/equeue.h"
#include "hle/kernel/kernel.h"

namespace Hle::Kernel {

// ----------------------------------------------------------- event queue ----

void EventQueue::Add(u64 ident, s16 filter, void* udata) {
    std::scoped_lock lk(lock);
    for (Entry& e : events) {
        if (e.ev.ident == ident && e.ev.filter == filter) {
            e.ev.udata = udata;
            return;
        }
    }
    events.push_back({{ident, filter, 0, 0, 0, udata}, false});
}

bool EventQueue::Remove(u64 ident, s16 filter) {
    std::scoped_lock lk(lock);
    for (auto it = events.begin(); it != events.end(); ++it) {
        if (it->ev.ident == ident && it->ev.filter == filter) {
            events.erase(it);
            return true;
        }
    }
    return false;
}

bool EventQueue::Trigger(u64 ident, s16 filter, s64 data) {
    {
        std::scoped_lock lk(lock);
        Entry* found = nullptr;
        for (Entry& e : events) {
            if (e.ev.ident == ident && e.ev.filter == filter) {
                found = &e;
            }
        }
        if (!found) {
            return false;
        }
        found->fired = true;
        found->ev.data = data;
    }
    cv.notify_all();
    return true;
}

int EventQueue::Wait(GuestKevent* out, int max, const u32* timeout_us) {
    std::unique_lock lk(lock);
    auto ready = [&] {
        for (const Entry& e : events) {
            if (e.fired) {
                return true;
            }
        }
        return false;
    };
    if (!timeout_us) {
        cv.wait(lk, ready);
    } else if (!cv.wait_for(lk, std::chrono::microseconds(*timeout_us), ready)) {
        return 0;
    }
    int n = 0;
    for (Entry& e : events) {
        if (e.fired && n < max) {
            out[n++] = e.ev;
            e.fired = false; // EV_CLEAR semantics
        }
    }
    return n;
}

namespace {

s32 sceKernelCreateEqueue(EventQueue** eq, const char* name) {
    if (!eq) {
        return OrbisError(EINVAL);
    }
    *eq = new EventQueue(name ? name : "");
    return ORBIS_OK;
}

s32 sceKernelDeleteEqueue(EventQueue* eq) {
    delete eq;
    return ORBIS_OK;
}

s32 sceKernelWaitEqueue(EventQueue* eq, GuestKevent* ev, int num, int* out, const u32* timeout) {
    if (!eq) {
        return OrbisError(EBADF);
    }
    const int n = eq->Wait(ev, num, timeout);
    if (out) {
        *out = n;
    }
    return n == 0 && timeout ? OrbisError(ETIMEDOUT) : ORBIS_OK;
}

s32 sceKernelAddUserEvent(EventQueue* eq, int id) {
    eq->Add(static_cast<u64>(id), kEvfiltUser, nullptr);
    return ORBIS_OK;
}

s32 sceKernelAddUserEventEdge(EventQueue* eq, int id) {
    return sceKernelAddUserEvent(eq, id);
}

s32 sceKernelTriggerUserEvent(EventQueue* eq, int id, void* udata) {
    eq->Add(static_cast<u64>(id), kEvfiltUser, udata);
    eq->Trigger(static_cast<u64>(id), kEvfiltUser, 0);
    return ORBIS_OK;
}

s32 sceKernelDeleteUserEvent(EventQueue* eq, int id) {
    return eq->Remove(static_cast<u64>(id), kEvfiltUser) ? ORBIS_OK : OrbisError(ENOENT);
}

int sceKernelGetEventFilter(const GuestKevent* ev) { return ev->filter; }
u64 sceKernelGetEventId(const GuestKevent* ev) { return ev->ident; }
s64 sceKernelGetEventData(const GuestKevent* ev) { return ev->data; }
void* sceKernelGetEventUserData(const GuestKevent* ev) { return ev->udata; }

// ------------------------------------------------------------ semaphores ----

struct Sema {
    std::mutex lock;
    std::condition_variable cv;
    int count;
    int max;
    std::string name;
    bool deleted = false;

    // Waits for `need` units; timeout_us null = forever. Returns errno.
    int Wait(int need, u32* timeout_us) {
        std::unique_lock lk(lock);
        auto ok = [&] { return count >= need || deleted; };
        if (!timeout_us) {
            cv.wait(lk, ok);
        } else {
            const auto start = std::chrono::steady_clock::now();
            const bool got = cv.wait_for(lk, std::chrono::microseconds(*timeout_us), ok);
            const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count();
            *timeout_us = spent >= *timeout_us ? 0 : static_cast<u32>(*timeout_us - spent);
            if (!got) {
                return ETIMEDOUT;
            }
        }
        if (deleted) {
            return EACCES;
        }
        count -= need;
        return 0;
    }

    int Signal(int n) {
        {
            std::scoped_lock lk(lock);
            if (count + n > max) {
                return EINVAL;
            }
            count += n;
        }
        cv.notify_all();
        return 0;
    }
};

s32 sceKernelCreateSema(Sema** sem, const char* name, u32 attr, int init, int max, const void*) {
    if (!sem || init < 0 || max <= 0 || init > max) {
        return OrbisError(EINVAL);
    }
    auto* s = new Sema();
    s->count = init;
    s->max = max;
    s->name = name ? name : "";
    *sem = s;
    return ORBIS_OK;
}

s32 sceKernelDeleteSema(Sema* sem) {
    if (!sem) {
        return OrbisError(ESRCH);
    }
    {
        std::scoped_lock lk(sem->lock);
        sem->deleted = true;
    }
    sem->cv.notify_all();
    // Waiters may still touch it; leak rather than race (rare, small).
    return ORBIS_OK;
}

s32 sceKernelWaitSema(Sema* sem, int need, u32* timeout) {
    return sem ? OrbisError(sem->Wait(need, timeout)) : OrbisError(ESRCH);
}

s32 sceKernelSignalSema(Sema* sem, int count) {
    return sem ? OrbisError(sem->Signal(count)) : OrbisError(ESRCH);
}

s32 sceKernelPollSema(Sema* sem, int need) {
    if (!sem) {
        return OrbisError(ESRCH);
    }
    std::scoped_lock lk(sem->lock);
    if (sem->count < need) {
        return OrbisError(EBUSY);
    }
    sem->count -= need;
    return ORBIS_OK;
}

s32 sceKernelCancelSema(Sema* sem, int count, int* waiters) {
    if (waiters) {
        *waiters = 0;
    }
    if (sem && count >= 0) {
        std::scoped_lock lk(sem->lock);
        sem->count = count;
    }
    return ORBIS_OK;
}

// POSIX sem_t is a 16-byte struct on FreeBSD; we keep our pointer in it.
Sema*& SemSlot(void* sem) {
    return *static_cast<Sema**>(sem);
}

int posix_sem_init(void* sem, int pshared, u32 value) {
    auto* s = new Sema();
    s->count = static_cast<int>(value);
    s->max = 0x7FFFFFFF;
    SemSlot(sem) = s;
    return 0;
}

int posix_sem_destroy(void* sem) {
    delete SemSlot(sem);
    SemSlot(sem) = nullptr;
    return 0;
}

int posix_sem_wait(void* sem) {
    const int err = SemSlot(sem)->Wait(1, nullptr);
    return err ? FailErrno(err) : 0;
}

int posix_sem_trywait(void* sem) {
    Sema* s = SemSlot(sem);
    std::scoped_lock lk(s->lock);
    if (s->count < 1) {
        return FailErrno(EAGAIN);
    }
    --s->count;
    return 0;
}

int posix_sem_post(void* sem) {
    const int err = SemSlot(sem)->Signal(1);
    return err ? FailErrno(err) : 0;
}

int posix_sem_getvalue(void* sem, int* value) {
    Sema* s = SemSlot(sem);
    std::scoped_lock lk(s->lock);
    *value = s->count;
    return 0;
}

} // namespace

void RegisterSync() {
    constexpr const char* k = "libkernel";
    DC_HLE(k, sceKernelCreateEqueue);
    DC_HLE(k, sceKernelDeleteEqueue);
    DC_HLE(k, sceKernelWaitEqueue);
    DC_HLE(k, sceKernelAddUserEvent);
    DC_HLE(k, sceKernelAddUserEventEdge);
    DC_HLE(k, sceKernelTriggerUserEvent);
    DC_HLE(k, sceKernelDeleteUserEvent);
    DC_HLE(k, sceKernelGetEventFilter);
    DC_HLE(k, sceKernelGetEventId);
    DC_HLE(k, sceKernelGetEventData);
    DC_HLE(k, sceKernelGetEventUserData);
    DC_HLE(k, sceKernelCreateSema);
    DC_HLE(k, sceKernelDeleteSema);
    DC_HLE(k, sceKernelWaitSema);
    DC_HLE(k, sceKernelSignalSema);
    DC_HLE(k, sceKernelPollSema);
    DC_HLE(k, sceKernelCancelSema);
    for (const char* lib : {"libkernel", "libScePosix"}) {
        DC_HLE_AS(lib, "sem_init", posix_sem_init);
        DC_HLE_AS(lib, "sem_destroy", posix_sem_destroy);
        DC_HLE_AS(lib, "sem_wait", posix_sem_wait);
        DC_HLE_AS(lib, "sem_trywait", posix_sem_trywait);
        DC_HLE_AS(lib, "sem_post", posix_sem_post);
        DC_HLE_AS(lib, "sem_getvalue", posix_sem_getvalue);
    }
}

} // namespace Hle::Kernel
