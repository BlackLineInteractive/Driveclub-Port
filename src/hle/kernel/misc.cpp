// libkernel odds and ends: errno, signals, stack protector, process info,
// and the few libSceLibcInternal functions the game's own libc expects.

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

#include "common/log.h"
#include "core/loader/linker.h"
#include "hle/hle.h"
#include "hle/kernel/kernel.h"

namespace Hle::Kernel {

namespace {

thread_local int t_errno = 0;
thread_local int t_net_errno = 0;

// Data symbols the guest imports by address.
u64 g_stack_chk_guard = 0x2B992DDFA23249D6ull;
const char* g_progname = "eboot.bin";
u64 g_need_libc_internal = 1;
// Opaque stand-in for the PS4 libc FILE* stdout; only its address is used.
alignas(16) u8 g_stdout_file[256];

int* __error() {
    return &t_errno;
}

int* sceNetErrnoLoc() {
    return &t_net_errno;
}

[[noreturn]] void __stack_chk_fail() {
    DC_ERROR("guest stack smashing detected");
    std::abort();
}

[[noreturn]] void posix__exit(int code) {
    DC_INFO("guest _exit(%d)", code);
    std::_Exit(code);
}

void __cxa_finalize(void*) {}
void __pthread_cxa_finalize(void*) {}

// Signals: the game installs handlers for crash reporting; we keep ours.
void* posix_signal(int, void*) {
    return nullptr; // SIG_DFL
}

int posix_sigprocmask(int, const void*, void* old) {
    if (old) {
        std::memset(old, 0, 16); // FreeBSD sigset_t is 128 bits
    }
    return 0;
}

int posix_sigfillset(void* set) {
    std::memset(set, 0xFF, 16);
    return 0;
}

int posix_sigreturn(void*) {
    return 0;
}

int _is_signal_return(void*) {
    return 0;
}

int posix_getpid() {
    return getpid();
}

// The game's libc asks the kernel for a few sysctl values; none we provide yet.
int posix_sysctl(const int* name, u32 namelen, void*, u64*, const void*, u64) {
    DC_WARN("sysctl {%d, %d, ...} (len %u) not implemented", namelen > 0 ? name[0] : -1,
            namelen > 1 ? name[1] : -1, namelen);
    return FailErrno(ENOENT);
}

s32 sceKernelDebugRaiseException(u32 code, u32 arg) {
    DC_WARN("guest raised debug exception %#x (%#x)", code, arg);
    return ORBIS_OK;
}

s32 sceKernelDebugRaiseExceptionOnReleaseMode(u32 code, u32 arg) {
    return sceKernelDebugRaiseException(code, arg);
}

s32 sceKernelStopUnloadModule(s32, u64, const void*, u32, const void*, s32* result) {
    if (result) {
        *result = 0;
    }
    return ORBIS_OK;
}

s32 sceKernelIsNeoMode() {
    return 0; // base PS4
}

// libSceLibcInternal: plain C library functions, served by the host libc.
// Format strings and va_lists are ABI-compatible (both x86-64 System V).
void* lci_malloc(size_t size) { return std::malloc(size); }
void lci_free(void* p) { std::free(p); }
void* lci_memcpy(void* d, const void* s, size_t n) { return std::memcpy(d, s, n); }
void* lci_memset(void* d, int c, size_t n) { return std::memset(d, c, n); }
int lci_vsnprintf(char* buf, size_t n, const char* fmt, va_list ap) { return std::vsnprintf(buf, n, fmt, ap); }
int lci_vprintf(const char* fmt, va_list ap) { return std::vprintf(fmt, ap); }

int lci_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = std::vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int lci_fflush(void*) {
    return std::fflush(stdout);
}

int lci_rename(const char* from, const char* to) {
    DC_WARN("rename(%s, %s) from libSceLibcInternal ignored", from, to);
    return 0;
}

} // namespace

int* GuestErrno() {
    return &t_errno;
}

void RegisterMisc() {
    constexpr const char* k = "libkernel";
    DC_HLE(k, __error);
    DC_HLE(k, __stack_chk_fail);
    DC_HLE_AS(k, "__stack_chk_guard", g_stack_chk_guard);
    DC_HLE_AS(k, "__progname", g_progname);
    DC_HLE_AS(k, "_exit", posix__exit);
    DC_HLE(k, __cxa_finalize);
    DC_HLE(k, __pthread_cxa_finalize);
    DC_HLE(k, _is_signal_return);
    DC_HLE(k, sceKernelDebugRaiseException);
    DC_HLE(k, sceKernelDebugRaiseExceptionOnReleaseMode);
    DC_HLE(k, sceKernelStopUnloadModule);
    DC_HLE(k, sceKernelIsNeoMode);
    for (const char* lib : {"libkernel", "libScePosix"}) {
        DC_HLE_AS(lib, "signal", posix_signal);
        DC_HLE_AS(lib, "sigprocmask", posix_sigprocmask);
        DC_HLE_AS(lib, "_sigprocmask", posix_sigprocmask);
        DC_HLE_AS(lib, "sigfillset", posix_sigfillset);
        DC_HLE_AS(lib, "sigreturn", posix_sigreturn);
        DC_HLE_AS(lib, "getpid", posix_getpid);
        DC_HLE_AS(lib, "sysctl", posix_sysctl);
    }
    DC_HLE("libSceNet", sceNetErrnoLoc);

    constexpr const char* lci = "libSceLibcInternal";
    DC_HLE_AS(lci, "Need_sceLibcInternal", g_need_libc_internal);
    DC_HLE_AS(lci, "_Stdout", g_stdout_file);
    DC_HLE_AS(lci, "malloc", lci_malloc);
    DC_HLE_AS(lci, "free", lci_free);
    DC_HLE_AS(lci, "memcpy", lci_memcpy);
    DC_HLE_AS(lci, "memset", lci_memset);
    DC_HLE_AS(lci, "vsnprintf", lci_vsnprintf);
    DC_HLE_AS(lci, "vprintf", lci_vprintf);
    DC_HLE_AS(lci, "printf", lci_printf);
    DC_HLE_AS(lci, "fflush", lci_fflush);
    DC_HLE_AS(lci, "rename", lci_rename);
}

} // namespace Hle::Kernel
