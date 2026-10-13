// libkernel: process, module and TLS services.

#include <cerrno>
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
    const int ret = Core::Runtime::StartModule(name.c_str(), args, argp);
    if (result) {
        *result = ret;
    }
    DC_INFO("sceKernelLoadStartModule(%s) -> handle %d (ret=%d)", path, HandleOf(m), ret);
    return HandleOf(m);
}

// Module description the guest's C++ unwinder uses to find .eh_frame.
struct ModuleSegmentInfo {
    u64 address;
    u32 size;
    s32 prot;
};

struct ModuleInfoEx {
    u64 st_size;
    char name[256];
    s32 id;
    u32 tls_index;
    u64 tls_init_addr;
    u32 tls_init_size;
    u32 tls_size;
    u32 tls_offset;
    u32 tls_align;
    u64 init_proc_addr;
    u64 fini_proc_addr;
    u64 reserved1;
    u64 reserved2;
    u64 eh_frame_hdr_addr;
    u64 eh_frame_addr;
    u32 eh_frame_hdr_size;
    u32 eh_frame_size;
    ModuleSegmentInfo segments[4];
    u32 segment_count;
};
static_assert(sizeof(ModuleInfoEx) == 424);

s32 sceKernelGetModuleInfoFromAddr(u64 addr, int flags, ModuleInfoEx* info) {
    if (!info) {
        return Kernel::OrbisError(EFAULT);
    }
    for (const auto& mp : Linker::Instance()->Modules()) {
        const auto* m = mp.get();
        if (addr < m->Base() || addr >= m->Base() + m->Size()) {
            continue;
        }
        std::memset(info, 0, sizeof(*info));
        info->st_size = sizeof(*info);
        std::strncpy(info->name, (m->Name() + (m->IsExecutable() ? ".bin" : ".prx")).c_str(),
                     sizeof(info->name) - 1);
        info->id = HandleOf(m);
        info->tls_index = m->tls_index;
        info->tls_init_addr = m->HasTls() ? m->TlsImage() : 0;
        info->tls_init_size = static_cast<u32>(m->TlsFileSize());
        info->tls_size = static_cast<u32>(m->TlsMemSize());
        info->tls_align = static_cast<u32>(m->TlsAlign());
        info->init_proc_addr = m->InitAddr();
        info->fini_proc_addr = m->FiniAddr();
        info->eh_frame_hdr_addr = m->EhFrameHdr();
        info->eh_frame_addr = m->EhFrame();
        info->eh_frame_hdr_size = static_cast<u32>(m->EhFrameHdrSize());
        info->eh_frame_size = static_cast<u32>(m->EhFrameSize());
        for (const auto& ph : m->LoadSegments()) {
            if (info->segment_count == 4) {
                break;
            }
            info->segments[info->segment_count++] = {m->Base() + ph.p_vaddr,
                                                     static_cast<u32>(ph.p_memsz),
                                                     static_cast<s32>(ph.p_flags)};
        }
        return Kernel::ORBIS_OK;
    }
    return Kernel::OrbisError(ESRCH);
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
    DC_HLE("libkernel", sceKernelGetModuleInfoFromAddr);
}

} // namespace Hle
