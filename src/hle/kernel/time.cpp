// libkernel time and sleep.

#include <cerrno>
#include <ctime>

#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <x86intrin.h>

#include "hle/hle.h"
#include "hle/kernel/kernel.h"

namespace Hle::Kernel {

namespace {

const u64 g_start_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

u64 TscFrequency() {
    static const u64 freq = [] {
        u64 f = 0;
        size_t size = sizeof(f);
        if (sysctlbyname("machdep.tsc.frequency", &f, &size, nullptr, 0) != 0 || f == 0) {
            f = 1000000000; // fall back to 1 GHz rather than divide by zero
        }
        return f;
    }();
    return freq;
}

// FreeBSD clock ids -> host clocks.
bool HostClock(int guest, clockid_t* out) {
    switch (guest) {
    case 0:  // CLOCK_REALTIME
    case 9:  // CLOCK_REALTIME_PRECISE
    case 10: // CLOCK_REALTIME_FAST
    case 13: // CLOCK_SECOND
        *out = CLOCK_REALTIME;
        return true;
    case 4:  // CLOCK_MONOTONIC
    case 11: // CLOCK_MONOTONIC_PRECISE
    case 12: // CLOCK_MONOTONIC_FAST
    case 5:  // CLOCK_UPTIME
    case 7:  // CLOCK_UPTIME_PRECISE
    case 8:  // CLOCK_UPTIME_FAST
        *out = CLOCK_MONOTONIC;
        return true;
    case 14: // CLOCK_THREAD_CPUTIME_ID
        *out = CLOCK_THREAD_CPUTIME_ID;
        return true;
    case 15: // CLOCK_PROCESS_CPUTIME_ID
        *out = CLOCK_PROCESS_CPUTIME_ID;
        return true;
    default:
        return false;
    }
}

int posix_clock_gettime(int clock, timespec* ts) {
    clockid_t host;
    if (!HostClock(clock, &host)) {
        return FailErrno(EINVAL);
    }
    return clock_gettime(host, ts) == 0 ? 0 : FailErrno(errno);
}

int posix_clock_getres(int clock, timespec* ts) {
    clockid_t host;
    if (!HostClock(clock, &host)) {
        return FailErrno(EINVAL);
    }
    return clock_getres(host, ts) == 0 ? 0 : FailErrno(errno);
}

s32 sceKernelClockGettime(int clock, timespec* ts) {
    clockid_t host;
    if (!HostClock(clock, &host)) {
        return OrbisError(EINVAL);
    }
    return clock_gettime(host, ts) == 0 ? ORBIS_OK : OrbisError(errno);
}

int posix_gettimeofday(timeval* tv, void* tz) {
    gettimeofday(tv, nullptr);
    if (tz) {
        static_cast<int*>(tz)[0] = 0; // minutes west
        static_cast<int*>(tz)[1] = 0; // dst
    }
    return 0;
}

s32 sceKernelGettimeofday(timeval* tv) {
    gettimeofday(tv, nullptr);
    return ORBIS_OK;
}

s32 sceKernelGettimezone(int* tz) {
    struct timeval tv;
    struct timezone host;
    gettimeofday(&tv, &host);
    tz[0] = host.tz_minuteswest;
    tz[1] = host.tz_dsttime;
    return ORBIS_OK;
}

u64 sceKernelGetProcessTime() {
    return (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - g_start_ns) / 1000;
}

u64 sceKernelGetProcessTimeCounter() {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - g_start_ns;
}

u64 sceKernelGetProcessTimeCounterFrequency() {
    return 1000000000;
}

u64 sceKernelReadTsc() {
    return __rdtsc();
}

u64 sceKernelGetTscFrequency() {
    return TscFrequency();
}

s32 sceKernelUsleep(u32 usec) {
    usleep(usec);
    return ORBIS_OK;
}

s32 sceKernelSleep(u32 sec) {
    sleep(sec);
    return ORBIS_OK;
}

s32 sceKernelNanosleep(const timespec* req, timespec* rem) {
    return nanosleep(req, rem) == 0 ? ORBIS_OK : OrbisError(errno);
}

int posix_nanosleep(const timespec* req, timespec* rem) {
    return nanosleep(req, rem) == 0 ? 0 : FailErrno(errno);
}

int posix_usleep(u32 usec) {
    return usleep(usec);
}

u32 posix_sleep(u32 sec) {
    return sleep(sec);
}

time_t posix_time(time_t* t) {
    return time(t);
}

int posix_getrusage(int who, rusage* usage) {
    return getrusage(who, usage) == 0 ? 0 : FailErrno(errno);
}

tm* lc_gmtime(const time_t* t) {
    return gmtime(t);
}

tm* lc_localtime(const time_t* t) {
    return localtime(t);
}

int lc_gmtime_s(tm* result, const time_t* t) {
    return gmtime_r(t, result) ? 0 : EINVAL;
}

time_t lc_mktime(tm* t) {
    return mktime(t);
}

} // namespace

void RegisterTime() {
    constexpr const char* k = "libkernel";
    DC_HLE(k, sceKernelClockGettime);
    DC_HLE(k, sceKernelGettimeofday);
    DC_HLE(k, sceKernelGettimezone);
    DC_HLE(k, sceKernelGetProcessTime);
    DC_HLE(k, sceKernelGetProcessTimeCounter);
    DC_HLE(k, sceKernelGetProcessTimeCounterFrequency);
    DC_HLE(k, sceKernelReadTsc);
    DC_HLE(k, sceKernelGetTscFrequency);
    DC_HLE(k, sceKernelUsleep);
    DC_HLE(k, sceKernelSleep);
    DC_HLE(k, sceKernelNanosleep);
    for (const char* lib : {"libkernel", "libScePosix"}) {
        DC_HLE_AS(lib, "clock_gettime", posix_clock_gettime);
        DC_HLE_AS(lib, "clock_getres", posix_clock_getres);
        DC_HLE_AS(lib, "gettimeofday", posix_gettimeofday);
        DC_HLE_AS(lib, "nanosleep", posix_nanosleep);
        DC_HLE_AS(lib, "usleep", posix_usleep);
        DC_HLE_AS(lib, "sleep", posix_sleep);
        DC_HLE_AS(lib, "time", posix_time);
        DC_HLE_AS(lib, "getrusage", posix_getrusage);
    }
    for (const char* lib : {"libkernel", "libSceLibcInternal", "libc"}) {
        DC_HLE_AS(lib, "gmtime", lc_gmtime);
        DC_HLE_AS(lib, "gmtime_s", lc_gmtime_s);
        DC_HLE_AS(lib, "localtime", lc_localtime);
        DC_HLE_AS(lib, "mktime", lc_mktime);
    }
}

} // namespace Hle::Kernel
