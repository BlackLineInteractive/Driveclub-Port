#include "core/tls.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <architecture/i386/table.h>
#include <i386/user_ldt.h>
#include <pthread.h>
#include <sys/mman.h>

#include "common/log.h"

// Low memory for the LDT segments: a descriptor base is only 32 bits wide.
// Reserved at a fixed address by -segaddr (CMakeLists.txt) so nothing else
// lands there first.
asm(".zerofill GUEST_TCB,GUEST_TCB,__guest_tcb,0x3FC000");

namespace Core::Tls {

namespace {

constexpr uintptr_t kLdtRegionBase = 0x4000;
constexpr size_t kLdtRegionSize = 0x3FC000;
constexpr size_t kLdtBlockSize = 0x1000;
constexpr u16 kLdtIndexBase = 8; // first user LDT slot macOS hands out
constexpr u16 kLdtCount = kLdtRegionSize / kLdtBlockSize;
constexpr size_t kMaxModules = 64;

struct ThreadState {
    Tcb* tcb;        // real TCB, inside the guest-visible allocation
    void* block;     // malloc'd [static TLS][Tcb]
    u16 ldt_index;
};

std::mutex g_lock;
std::vector<ModuleTls> g_modules(kMaxModules + 1);
u32 g_max_index = 0;
std::vector<u16> g_free_ldt;
pthread_key_t g_state_key;

u64 StaticSize() {
    const ModuleTls& exe = g_modules[1];
    return exe.memsz ? AlignUp(exe.memsz, std::max<u64>(exe.align, 16)) : 0;
}

void InitOnce() {
    static std::once_flag once;
    std::call_once(once, [] {
        mmap(reinterpret_cast<void*>(kLdtRegionBase), kLdtRegionSize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        for (u16 i = 0; i < kLdtCount; ++i) {
            g_free_ldt.push_back(static_cast<u16>(kLdtIndexBase + kLdtCount - 1 - i));
        }
        pthread_key_create(&g_state_key, nullptr);
    });
}

// Points FS at an LDT segment whose first bytes are a copy of `tcb`.
u16 InstallLdt(Tcb* tcb) {
    u16 index;
    {
        std::scoped_lock lk(g_lock);
        if (g_free_ldt.empty()) {
            DC_ERROR("out of LDT slots for guest threads");
            std::abort();
        }
        index = g_free_ldt.back();
        g_free_ldt.pop_back();
    }

    const uintptr_t addr = kLdtRegionBase + (index - kLdtIndexBase) * kLdtBlockSize;
    std::memcpy(reinterpret_cast<void*>(addr), tcb, sizeof(Tcb));

    ldt_entry ldt{};
    ldt.data = {
        .limit00 = static_cast<u16>(kLdtBlockSize - 1),
        .base00 = static_cast<u16>(addr),
        .base16 = static_cast<u8>(addr >> 16),
        .type = DESC_DATA_WRITE,
        .dpl = 3,
        .present = 1,
        .limit16 = 0,
        .stksz = DESC_DATA_32B,
        .granular = DESC_GRAN_BYTE,
        .base24 = static_cast<u8>(addr >> 24),
    };
    if (i386_set_ldt(index, &ldt, 1) != index) {
        DC_ERROR("i386_set_ldt(%u) failed", index);
        std::abort();
    }
    const sel_t selector{.rpl = USER_PRIV, .ti = SEL_LDT, .index = index};
    asm volatile("mov %0, %%fs" ::"r"(selector));
    return index;
}

void* AllocModuleBlock(u32 index) {
    const ModuleTls& m = g_modules[index];
    void* block = std::aligned_alloc(std::max<u64>(m.align, 16), AlignUp(m.memsz, 16));
    std::memcpy(block, reinterpret_cast<void*>(m.image), m.filesz);
    std::memset(static_cast<u8*>(block) + m.filesz, 0, m.memsz - m.filesz);
    return block;
}

} // namespace

void RegisterModule(u32 index, const ModuleTls& tls) {
    if (index == 0 || index > kMaxModules) {
        DC_ERROR("TLS index %u out of range", index);
        return;
    }
    std::scoped_lock lk(g_lock);
    g_modules[index] = tls;
    g_max_index = std::max(g_max_index, index);
}

Tcb* SetupThread() {
    InitOnce();

    const u64 static_size = StaticSize();
    u8* block = static_cast<u8*>(std::aligned_alloc(64, AlignUp(static_size + sizeof(Tcb), 64)));
    auto* tcb = reinterpret_cast<Tcb*>(block + static_size);
    std::memset(tcb, 0, sizeof(Tcb));
    tcb->self = tcb;

    // Static block of the executable sits right below the TCB.
    if (static_size) {
        const ModuleTls& exe = g_modules[1];
        u8* exe_block = block;
        std::memcpy(exe_block, reinterpret_cast<void*>(exe.image), exe.filesz);
        std::memset(exe_block + exe.filesz, 0, static_size - exe.filesz);
    }

    auto** dtv = static_cast<void**>(std::calloc(kMaxModules + 2, sizeof(void*)));
    dtv[1] = reinterpret_cast<void*>(static_cast<uintptr_t>(kMaxModules));
    if (static_size) {
        dtv[2] = block;
    }
    tcb->dtv = dtv;

    auto* state = new ThreadState{tcb, block, 0};
    state->ldt_index = InstallLdt(tcb);
    pthread_setspecific(g_state_key, state);
    return tcb;
}

void TeardownThread() {
    auto* state = static_cast<ThreadState*>(pthread_getspecific(g_state_key));
    if (!state) {
        return;
    }
    for (u32 i = 2; i <= kMaxModules; ++i) {
        std::free(state->tcb->dtv[1 + i]);
    }
    std::free(state->tcb->dtv);
    std::free(state->block);
    {
        std::scoped_lock lk(g_lock);
        g_free_ldt.push_back(state->ldt_index);
    }
    pthread_setspecific(g_state_key, nullptr);
    delete state;
}

Tcb* Current() {
    auto* state = static_cast<ThreadState*>(pthread_getspecific(g_state_key));
    return state ? state->tcb : nullptr;
}

void* GetAddr(const TlsIndex* index) {
    Tcb* tcb = Current();
    if (!tcb || index->module == 0 || index->module > kMaxModules) {
        DC_ERROR("__tls_get_addr: bad module %llu", index->module);
        std::abort();
    }
    void*& slot = tcb->dtv[1 + index->module];
    if (!slot) {
        slot = AllocModuleBlock(static_cast<u32>(index->module));
    }
    return static_cast<u8*>(slot) + index->offset;
}

} // namespace Core::Tls
