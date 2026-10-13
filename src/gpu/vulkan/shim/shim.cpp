// Driveclub Native: implementations behind the shim headers (settings, log,
// memory, signals, window, Liverpool) the vendored shadPS4 GPU code links
// against. Built with the shadPS4 include root; reaches our engine only
// through dc_host.h.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <SDL3/SDL.h>
#include <fmt/format.h>

#if defined(__linux__)
#include <unistd.h>
#endif

#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "dc_host.h"
#include "sdl_window.h"
#include "video_core/amdgpu/liverpool.h"

// ================= settings =================

namespace {

bool EnvFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

} // namespace

EmulatorSettingsImpl::EmulatorSettingsImpl() {
    if (const char* v = std::getenv("DC_VK_READBACKS")) { // 0 off, 1 relaxed, 2 precise
        readbacks_mode = static_cast<u32>(std::clamp(std::atoi(v), 0, 2));
    }
    direct_memory_access = EnvFlag("DC_VK_DMA");
    dump_shaders = EnvFlag("DC_VK_DUMP_SHADERS");
    pipeline_cache = EnvFlag("DC_VK_PIPELINE_CACHE");
    validation = EnvFlag("DC_VK_VALIDATION");
    markers = EnvFlag("DC_VK_MARKERS");
    if (const char* v = std::getenv("DC_VK_GPU")) { // physical device index
        gpu_id = std::atoi(v);
    }
}

EmulatorSettingsImpl* EmulatorSettingsImpl::GetInstance() {
    static EmulatorSettingsImpl instance;
    return &instance;
}

// ================= logging =================

namespace Common::Log {

namespace {

Level DefaultLevel() {
    const char* v = std::getenv("DC_VK_LOG"); // trace|debug|info|warning|error
    if (!v) {
        return Level::Info;
    }
    const std::string s = v;
    if (s == "trace") return Level::Trace;
    if (s == "debug") return Level::Debug;
    if (s == "warning") return Level::Warning;
    if (s == "error") return Level::Error;
    return Level::Info;
}

std::array<Level, NUM_LOG_CLASSES> MakeLevels() {
    std::array<Level, NUM_LOG_CLASSES> levels;
    levels.fill(DefaultLevel());
    return levels;
}

} // namespace

std::array<Level, NUM_LOG_CLASSES> g_class_levels = MakeLevels();

void Setup(std::string_view) {}
void Switch(std::string_view, bool) {}
void Shutdown() {}
void Flush() {
    std::fflush(stderr);
}
void Terminate() {}
void UpdateSinks() {}
void UpdateLogLevels(std::string_view) {}
void UpdateLogFlushLevel(std::string_view) {}

void VLog(Class log_class, Level level, const char* file, int line, const char* func,
          fmt::string_view format, fmt::format_args args) {
    static constexpr const char* kTags[] = {"trace", "debug", "info", "warn", "error", "error"};
    const std::string msg = fmt::vformat(format, args);
    std::fprintf(stderr, "[%s] vk %.*s: %s\n", kTags[std::min<int>(int(level), 5)],
                 int(NameOf(log_class).size()), NameOf(log_class).data(), msg.c_str());
}

} // namespace Common::Log

// ================= memory =================

namespace Core {

void AddressSpace::Protect(VAddr virtual_addr, size_t size, MemoryPermission perms) {
    DcHost::Protect(virtual_addr, size, True(perms & MemoryPermission::Read),
                    True(perms & MemoryPermission::Write));
}

u64 MemoryManager::ClampRangeSize(VAddr virtual_addr, u64 size) {
    static constexpr u64 MinSizeToClamp = 1_GB;
    if (size < MinSizeToClamp) {
        return size;
    }
    u64 clamped = 0;
    DcHost::Mapping m;
    while (clamped < size && DcHost::Query(virtual_addr + clamped, &m)) {
        clamped = m.start + m.size - virtual_addr;
    }
    return std::min(clamped, size);
}

void MemoryManager::CopySparseMemory(VAddr virtual_addr, u8* dest, u64 size) {
    u8* const view = DcHost::DirectView();
    while (size) {
        DcHost::Mapping m;
        if (!DcHost::Query(virtual_addr, &m)) {
            // Hole: zero up to the next 16 KB page.
            const u64 n = std::min<u64>(size, 16_KB - (virtual_addr & (16_KB - 1)));
            std::memset(dest, 0, n);
            size -= n;
            virtual_addr += n;
            dest += n;
            continue;
        }
        const u64 n = std::min<u64>(size, m.start + m.size - virtual_addr);
        const u8* src = m.direct && view ? view + m.phys + (virtual_addr - m.start)
                                         : reinterpret_cast<const u8*>(virtual_addr);
        std::memcpy(dest, src, n);
        size -= n;
        virtual_addr += n;
        dest += n;
    }
}

bool MemoryManager::TryWriteBacking(void* address, const void* data, u64 size) {
    auto virtual_addr = reinterpret_cast<VAddr>(address);
    const auto* src = static_cast<const u8*>(data);
    u8* const view = DcHost::DirectView();
    while (size) {
        DcHost::Mapping m;
        if (!DcHost::Query(virtual_addr, &m)) {
            return false;
        }
        const u64 n = std::min<u64>(size, m.start + m.size - virtual_addr);
        u8* dst = m.direct && view ? view + m.phys + (virtual_addr - m.start)
                                   : reinterpret_cast<u8*>(virtual_addr);
        std::memcpy(dst, src, n);
        size -= n;
        virtual_addr += n;
        src += n;
    }
    return true;
}

// ================= signals =================

namespace {

bool DispatchFault(void* context, void* fault_address) {
    return Signals::Instance()->DispatchAccessViolation(context, fault_address);
}

} // namespace

SignalDispatch::SignalDispatch() {
    DcHost::SetFaultHandler(DispatchFault);
}

SignalDispatch::~SignalDispatch() {
    DcHost::SetFaultHandler(nullptr);
}

void SignalDispatch::RemoveHandlers() {
    DcHost::SetFaultHandler(nullptr);
}

bool SignalDispatch::DispatchAccessViolation(void* context, void* fault_address) const {
    for (const auto& entry : access_violation_handlers) {
        if (entry.handler(context, fault_address)) {
            return true;
        }
    }
    return false;
}

bool SignalDispatch::DispatchIllegalInstruction(void* context) const {
    for (const auto& entry : illegal_instruction_handlers) {
        if (entry.handler(context)) {
            return true;
        }
    }
    return false;
}

} // namespace Core

// ================= window =================

namespace Frontend {

WindowSDL::WindowSDL(SDL_Window* window_, void* metal_layer) : window{window_} {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    SetSize(w, h);
    window_info.render_surface_scale = SDL_GetWindowPixelDensity(window);
#if defined(_WIN32)
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__APPLE__)
    window_info.type = WindowSystemType::Metal;
    window_info.render_surface = metal_layer;
#else
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (std::strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        window_info.render_surface =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    } else {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection =
            SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
        window_info.render_surface = reinterpret_cast<void*>(
            SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    }
#endif
    (void)metal_layer;
}

} // namespace Frontend

// ================= Liverpool =================
// shadPS4's PM4 front end (liverpool.cpp) is not built: ours feeds the
// rasterizer. The struct stays as the register file and GPU-thread lock.

namespace AmdGpu {

Liverpool::Liverpool() : guest_markers_enabled{EmulatorSettings.IsVkGuestMarkersEnabled()} {}

Liverpool::~Liverpool() = default;

void Liverpool::DcSetGpuThread() {
    gpu_id = std::this_thread::get_id();
#ifdef __linux__
    gpu_tid = static_cast<u32>(gettid());
#endif
}

} // namespace AmdGpu
