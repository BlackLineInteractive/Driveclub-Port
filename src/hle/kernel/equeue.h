#pragma once

// Kernel event queues (kqueue-like). Other HLE libraries (VideoOut flips,
// GPU end-of-pipe interrupts) register events on a guest queue and trigger
// them; the guest waits with sceKernelWaitEqueue.

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "common/types.h"

namespace Hle::Kernel {

// Guest struct kevent (FreeBSD amd64).
struct GuestKevent {
    u64 ident;
    s16 filter;
    u16 flags;
    u32 fflags;
    s64 data;
    void* udata;
};
static_assert(sizeof(GuestKevent) == 32);

constexpr s16 kEvfiltUser = -11;
constexpr s16 kEvfiltVideoOut = -13;
constexpr s16 kEvfiltGraphicsCore = -14;

class EventQueue {
public:
    explicit EventQueue(std::string name) : name(std::move(name)) {}

    void Add(u64 ident, s16 filter, void* udata);
    bool Remove(u64 ident, s16 filter);
    // Marks a registered event as fired; wakes waiters. Returns false if the
    // event is not registered on this queue.
    bool Trigger(u64 ident, s16 filter, s64 data);
    // Waits for fired events; timeout_us null = forever. Returns count.
    int Wait(GuestKevent* out, int max, const u32* timeout_us);

    const std::string& Name() const { return name; }

private:
    struct Entry {
        GuestKevent ev;
        bool fired = false;
    };
    std::string name;
    std::mutex lock;
    std::condition_variable cv;
    std::vector<Entry> events;
};

} // namespace Hle::Kernel
