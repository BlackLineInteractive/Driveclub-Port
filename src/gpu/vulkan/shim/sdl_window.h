// Driveclub Native: stand-in for shadPS4's SDL window. The Vulkan instance
// and swapchain only need the native surface handle and the size in pixels.

#pragma once

#include <atomic>

#include "common/types.h"

struct SDL_Window;

namespace Frontend {

enum class WindowSystemType : u8 {
    Headless,
    Windows,
    X11,
    Wayland,
    Metal,
};

struct WindowSystemInfo {
    void* display_connection = nullptr;
    void* render_surface = nullptr;
    float render_surface_scale = 1.0f;
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    // `metal_layer`: the CAMetalLayer of the window's SDL Metal view (macOS).
    WindowSDL(SDL_Window* window, void* metal_layer);

    s32 GetWidth() const { return width.load(std::memory_order_relaxed); }
    s32 GetHeight() const { return height.load(std::memory_order_relaxed); }
    WindowSystemInfo GetWindowInfo() const { return window_info; }
    SDL_Window* GetSdlWindow() const { return window; }

    void SetSize(s32 w, s32 h) {
        width.store(w, std::memory_order_relaxed);
        height.store(h, std::memory_order_relaxed);
    }

private:
    SDL_Window* window;
    std::atomic<s32> width{0};
    std::atomic<s32> height{0};
    WindowSystemInfo window_info{};
};

} // namespace Frontend
