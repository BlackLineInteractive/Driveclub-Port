// Driveclub Native: entry point.
// Usage: driveclub <game dir> [--load-only]
// Loads and links the game's modules, then brings up the window and the Metal
// presenter. Guest execution and the GPU frontend plug in behind this.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include "common/log.h"
#include "core/address_space.h"
#include "core/loader/linker.h"
#include "core/memory.h"
#include "core/runtime.h"
#include "core/signals.h"
#include "hle/hle.h"
#include "hle/kernel/kernel.h"
#include "gpu/metal/mtl_device.h"
#include "gpu/metal/mtl_presenter.h"

namespace {

constexpr int kWindowWidth = 1920;
constexpr int kWindowHeight = 1080;

int Run(int argc, char** argv) {
    if (argc < 2) {
        DC_ERROR("usage: %s <game dir> [--load-only]", argv[0]);
        return 1;
    }
    const bool load_only = argc > 2 && std::strcmp(argv[2], "--load-only") == 0;

    Core::AddressSpace::Init();
    Core::Memory::Init();
    Hle::RegisterAll();

    const char* home = std::getenv("HOME");
    Hle::Kernel::SetFileRoots(argv[1], std::filesystem::path(home ? home : "/tmp") /
                                           "Library/Application Support/DriveclubNative");

    Core::Loader::Linker linker;
    if (!linker.LoadGame(argv[1])) {
        return 1;
    }
    linker.Relocate();
    linker.PrintReport();
    if (load_only) {
        return 0;
    }

    Core::Signals::Install(
        [](u64 address) { return Core::Loader::Linker::Instance()->Describe(address); });
    Core::Runtime::StartGame(linker);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        DC_ERROR("SDL_Init: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window =
        SDL_CreateWindow("Driveclub Native", kWindowWidth, kWindowHeight,
                         SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        DC_ERROR("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    SDL_MetalView view = SDL_Metal_CreateView(window);

    Gpu::Metal::Device device;
    Gpu::Metal::PresenterConfig config{};
    Gpu::Metal::Presenter presenter(device, SDL_Metal_GetLayer(view), config);

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    presenter.Resize(w, h);

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto fps_window = start;
    int frames = 0;

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                presenter.Resize(ev.window.data1, ev.window.data2);
                break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_V) {
                    config.vsync = !config.vsync;
                    presenter.SetVsync(config.vsync);
                    DC_INFO("vsync: %d", config.vsync);
                }
                break;
            default:
                break;
            }
        }

        const float t = std::chrono::duration<float>(Clock::now() - start).count();
        presenter.PresentClear(0.5f + 0.5f * std::sin(t), 0.2f, 0.3f);

        ++frames;
        const auto now = Clock::now();
        if (now - fps_window >= std::chrono::seconds(1)) {
            const double secs = std::chrono::duration<double>(now - fps_window).count();
            char title[96];
            std::snprintf(title, sizeof(title), "Driveclub Native | %s | %.0f FPS",
                          device.Info().name.c_str(), frames / secs);
            SDL_SetWindowTitle(window, title);
            frames = 0;
            fps_window = now;
        }
    }

    SDL_Metal_DestroyView(view);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& e) {
        DC_ERROR("fatal: %s", e.what());
        return 1;
    }
}
