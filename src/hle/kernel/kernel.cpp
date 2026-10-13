// libkernel: process, module and TLS services.

#include <cstring>
#include <filesystem>

#include "common/log.h"
#include "core/loader/linker.h"
#include "core/runtime.h"
#include "core/tls.h"
#include "hle/hle.h"
#include "hle/kernel/kernel.h"

namespace Hle {

namespace {

using Core::Loader::Linker;

void* __tls_get_addr(const Core::Tls::TlsIndex* index) {
    return Core::Tls::GetAddr(index);
}

void* sceKernelGetProcParam() {
    return reinterpret_cast<void*>(Linker::Instance()->Executable()->ProcParam());
}

// Module handles are 1-based indices into the linker's module list.
s32 HandleOf(const Core::Loader::Module* m) {
    const auto& mods = Linker::Instance()->Modules();
    for (size_t i = 0; i < mods.size(); ++i) {
        if (mods[i].get() == m) {
            return static_cast<s32>(i + 1);
        }
    }
    return -1;
}

// All game modules are preloaded and linked; "loading" one just runs its
// module_start the first time and returns its handle.
s32 sceKernelLoadStartModule(const char* path, u64 args, const void* argp, u32 flags,
                             const void* opt, s32* result) {
    const std::string name = std::filesystem::path(path).stem().string();
    auto* m = Linker::Instance()->FindModule(name);
    if (!m) {
        DC_WARN("sceKernelLoadStartModule(%s): not found", path);
        return static_cast<s32>(0x80020002); // ORBIS_KERNEL_ERROR_ENOENT
    }
    const int ret = Core::Runtime::StartModule(name.c_str());
    if (result) {
        *result = ret;
    }
    DC_INFO("sceKernelLoadStartModule(%s) -> handle %d", path, HandleOf(m));
    return HandleOf(m);
}

// libc hands libkernel its malloc API table; nothing in our runtime needs it.
s32 _sceKernelRtldSetApplicationHeapAPI(void* api) {
    return 0;
}

} // namespace

void RegisterKernel() {
    Kernel::RegisterPthread();
    Kernel::RegisterMemory();
    Kernel::RegisterTime();
    Kernel::RegisterMisc();
    Kernel::RegisterFile();
    Kernel::RegisterSync();
    DC_HLE("libkernel", _sceKernelRtldSetApplicationHeapAPI);
    DC_HLE("libkernel", __tls_get_addr);
    DC_HLE("libkernel", sceKernelGetProcParam);
    DC_HLE("libkernel", sceKernelLoadStartModule);
}

} // namespace Hle
