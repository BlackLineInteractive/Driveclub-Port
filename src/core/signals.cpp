#include "core/signals.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <execinfo.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "common/log.h"

namespace Core::Signals {

namespace {

struct PatchArea {
    VAddr code_begin, code_end;
    VAddr next, end;
};

std::mutex g_patch_lock;
std::vector<PatchArea> g_areas;
AddressDescriber g_describer = nullptr;
std::atomic<u64> g_patched_sites{0};

constexpr u64 kHostPage = 0x1000;

// Decoded `extrq xmm, imm8 len, imm8 idx`: 66 [REX] 0F 78 /0 ib ib, register form.
struct Extrq {
    int length; // bytes of the instruction
    int xmm;    // 0..15
    int len;    // field length in bits (0 means 64)
    int idx;    // field start bit
};

bool DecodeExtrq(const u8* p, Extrq& out) {
    if (p[0] != 0x66) {
        return false;
    }
    int i = 1;
    int rex_b = 0;
    if ((p[i] & 0xF0) == 0x40) {
        rex_b = p[i] & 1;
        ++i;
    }
    if (p[i] != 0x0F || p[i + 1] != 0x78) {
        return false;
    }
    const u8 modrm = p[i + 2];
    if ((modrm >> 6) != 3 || ((modrm >> 3) & 7) != 0) {
        return false;
    }
    out.xmm = (modrm & 7) | (rex_b << 3);
    out.len = p[i + 3] & 0x3F;
    out.idx = p[i + 4] & 0x3F;
    out.length = i + 5;
    return true;
}

// psllq/psrlq xmmN, imm8: 66 [41] 0F 73 /6 or /2 ib
u8* EmitShift(u8* c, int xmm, int ext, int amount) {
    *c++ = 0x66;
    if (xmm >= 8) {
        *c++ = 0x41;
    }
    *c++ = 0x0F;
    *c++ = 0x73;
    *c++ = static_cast<u8>(0xC0 | (ext << 3) | (xmm & 7));
    *c++ = static_cast<u8>(amount);
    return c;
}

u8* EmitJmp(u8* c, VAddr target) {
    const s64 rel = static_cast<s64>(target) - static_cast<s64>(reinterpret_cast<VAddr>(c) + 5);
    *c++ = 0xE9;
    const s32 rel32 = static_cast<s32>(rel);
    std::memcpy(c, &rel32, 4);
    return c + 4;
}

// Replaces the EXTRQ at `site` with a jump to an SSE2 equivalent:
//   psllq xmm, 64 - idx - len   ; drop bits above the field
//   psrlq xmm, 64 - len         ; move the field to bit 0, zero-extended
// The upper qword is undefined after EXTRQ, so shifting it too is fine.
bool PatchExtrq(VAddr site) {
    Extrq e;
    if (!DecodeExtrq(reinterpret_cast<const u8*>(site), e)) {
        return false;
    }
    PatchArea* area = nullptr;
    for (PatchArea& a : g_areas) {
        if (site >= a.code_begin && site < a.code_end) {
            area = &a;
        }
    }
    if (!area || area->next + 32 > area->end) {
        DC_ERROR("no trampoline space for EXTRQ at %#llx", site);
        return false;
    }

    const int len = e.len == 0 ? 64 : e.len;
    u8* tramp = reinterpret_cast<u8*>(area->next);
    u8* c = tramp;
    c = EmitShift(c, e.xmm, 6, 64 - e.idx - len);
    c = EmitShift(c, e.xmm, 2, 64 - len);
    c = EmitJmp(c, site + e.length);
    area->next = AlignUp(reinterpret_cast<VAddr>(c), 16);

    // jmp tramp + NOP padding, written while the code page is briefly writable.
    u8 bytes[8];
    const s32 rel32 = static_cast<s32>(static_cast<s64>(reinterpret_cast<VAddr>(tramp)) -
                                       static_cast<s64>(site + 5));
    bytes[0] = 0xE9;
    std::memcpy(bytes + 1, &rel32, 4);
    for (int i = 5; i < e.length; ++i) {
        bytes[i] = 0x90;
    }

    const VAddr page_begin = AlignDown(site, kHostPage);
    const VAddr page_end = AlignUp(site + e.length, kHostPage);
    void* pages = reinterpret_cast<void*>(page_begin);
    mprotect(pages, page_end - page_begin, PROT_READ | PROT_WRITE | PROT_EXEC);
    std::memcpy(reinterpret_cast<void*>(site), bytes, e.length);
    mprotect(pages, page_end - page_begin, PROT_READ | PROT_EXEC);

    ++g_patched_sites;
    return true;
}

void Describe(const char* what, u64 rip, u64 fault, u64 rsp) {
    const std::string where = g_describer ? g_describer(rip) : std::string();
    DC_ERROR("%s at rip %#llx (%s), fault address %#llx", what, rip, where.c_str(), fault);
    // Host frames (our code and system libraries).
    void* frames[32];
    const int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    if (!g_describer) {
        return;
    }
    // Guest code is usually built without frame pointers, so scan the stack
    // for values pointing into module images: likely return addresses.
    const u64* stack = reinterpret_cast<const u64*>(rsp);
    int shown = 0;
    for (int i = 0; i < 512 && shown < 16; ++i) {
        const std::string s = g_describer(stack[i]);
        if (s.find('+') != std::string::npos) {
            DC_ERROR("  stack[%3d] %s", i, s.c_str());
            ++shown;
        }
    }
}

// Prints printable ASCII runs found on the stack: the game formats its fatal
// error messages into a stack buffer right before breaking.
void DumpStackStrings(u64 rsp) {
    const char* p = reinterpret_cast<const char*>(rsp);
    int run = 0;
    for (int i = 0; i < 0x3000; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        if ((c >= 0x20 && c < 0x7F) || c == '\n' || c == '\t') {
            ++run;
            continue;
        }
        if (c == 0 && run >= 12) {
            DC_ERROR("  message: %.*s", run, p + i - run);
        }
        run = 0;
    }
}

[[noreturn]] void Die(int sig) {
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

void Handler(int sig, siginfo_t* info, void* raw) {
    auto* uc = static_cast<ucontext_t*>(raw);
    const u64 rip = uc->uc_mcontext->__ss.__rip;
    const u64 rsp = uc->uc_mcontext->__ss.__rsp;

    if (sig == SIGILL) {
        std::scoped_lock lk(g_patch_lock);
        const u8* code = reinterpret_cast<const u8*>(rip);
        // Another thread may have patched this site while we were faulting.
        if (code[0] == 0xE9 || PatchExtrq(rip)) {
            return; // re-executes the now-patched site
        }
        Describe("illegal instruction", rip, rip, rsp);
        DC_ERROR("  bytes: %02x %02x %02x %02x %02x %02x %02x %02x", code[0], code[1], code[2],
                 code[3], code[4], code[5], code[6], code[7]);
        Die(sig);
    }

    // `int 0x41` is the PS4 debugger break the game uses on fatal errors.
    const u8* code = reinterpret_cast<const u8*>(rip);
    if (sig == SIGSEGV && code[0] == 0xCD && code[1] == 0x41) {
        Describe("guest debug break (int 0x41)", rip, 0, rsp);
        // Driveclub's fatal handler formats into [caller rsp + 0x30].
        DC_ERROR("  fatal message: %.512s", reinterpret_cast<const char*>(rsp + 8 + 0x30));
        DumpStackStrings(rsp);
        Die(sig);
    }

    Describe(sig == SIGSEGV ? "segmentation fault" : "bus error", rip,
             reinterpret_cast<u64>(info->si_addr), rsp);
    Die(sig);
}

} // namespace

void Install(AddressDescriber describer) {
    g_describer = describer;
    struct sigaction sa{};
    sa.sa_sigaction = Handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

void AddPatchArea(VAddr code_begin, VAddr code_end, VAddr area, u64 area_size) {
    std::scoped_lock lk(g_patch_lock);
    g_areas.push_back({code_begin, code_end, area, area + area_size});
}

} // namespace Core::Signals
