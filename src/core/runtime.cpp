#include "core/runtime.h"

#include <cstdlib>
#include <mutex>
#include <set>
#include <string>

#include <pthread.h>

#include "common/log.h"
#include "core/loader/linker.h"
#include "core/tls.h"

namespace Core::Runtime {

namespace {

// Layout the PS4 kernel hands to the executable's entry point.
struct EntryParams {
    int argc;
    u32 padding;
    const char* argv[33];
};

constexpr size_t kMainStackSize = 16ull << 20;

// Sony modules libkernel would have started before the game's entry point.
constexpr const char* kPreloadedModules[] = {"libc", "libSceFios2"};

std::mutex g_start_lock;
std::set<std::string> g_started;
EntryParams g_params{};

void ProgramExit() {
    DC_INFO("guest called its exit function");
    std::exit(0);
}

[[noreturn]] void JumpToEntry(EntryParams* params, VAddr entry, void (*exit_fn)()) {
    // Mimic the PS4 kernel: the stack holds argc/argv-ish data on entry, is
    // misaligned by 8 like after a call, and rdi/rsi carry params/exit func.
    asm volatile("andq $-16, %%rsp\n"
                 "subq $8, %%rsp\n"
                 "pushq 8(%%rdi)\n"
                 "pushq 0(%%rdi)\n"
                 "jmp *%%rax\n"
                 :
                 : "D"(params), "S"(exit_fn), "a"(entry)
                 : "memory");
    __builtin_unreachable();
}

void* GuestMain(void*) {
    Tls::SetupThread();

    for (const char* name : kPreloadedModules) {
        StartModule(name);
    }

    auto* exe = Loader::Linker::Instance()->Executable();
    g_params.argc = 1;
    g_params.argv[0] = "/app0/eboot.bin";
    DC_INFO("jumping to entry point %#llx", exe->Entry());
    JumpToEntry(&g_params, exe->Entry(), &ProgramExit);
}

} // namespace

int StartModule(const char* name) {
    Loader::Module* m = Loader::Linker::Instance()->FindModule(name);
    if (!m) {
        DC_WARN("module_start: %s is not loaded", name);
        return -1;
    }
    {
        std::scoped_lock lk(g_start_lock);
        if (!g_started.insert(name).second) {
            return 0;
        }
    }
    if (!m->HasInit()) {
        return 0;
    }
    DC_INFO("starting module %s", name);
    using ModuleStart = int (*)(u64 args, const void* argp, void* param);
    const int ret = reinterpret_cast<ModuleStart>(m->InitAddr())(
        0, nullptr, reinterpret_cast<void*>(m->ProcParam()));
    DC_INFO("module %s started: %d", name, ret);
    return ret;
}

void StartGame(Loader::Linker&) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kMainStackSize);
    pthread_t thread;
    pthread_create(&thread, &attr, GuestMain, nullptr);
    pthread_attr_destroy(&attr);
    pthread_detach(thread);
}

} // namespace Core::Runtime
