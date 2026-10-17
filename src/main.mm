// Driveclub Native: entry point.
// Usage: driveclub <game dir> [--load-only]
// Loads and links the game's modules, then brings up the window and the
// Metal renderer.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>

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
#include "hle/pad/pad.h"
#include "hle/video_out/video_out.h"
#include "gpu/liverpool/command_processor.h"
#include "gpu/metal/mtl_device.h"
#include "gpu/metal/mtl_presenter.h"
#include "gpu/metal/mtl_renderer.h"

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

    // SDL first, on the main thread: guest threads use its audio right away.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        DC_ERROR("SDL_Init: %s", SDL_GetError());
        return 1;
    }

    Core::Signals::Install(
        [](u64 address) { return Core::Loader::Linker::Instance()->Describe(address); });
    Core::Runtime::StartGame(linker);

    SDL_Window* window =
        SDL_CreateWindow("Driveclub Native", kWindowWidth, kWindowHeight,
                         SDL_WINDOW_METAL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                             (std::getenv("DC_HIDDEN") ? SDL_WINDOW_HIDDEN : 0)); // DC_HIDDEN: no window (frame dumps)
    if (!window) {
        DC_ERROR("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    SDL_MetalView view = SDL_Metal_CreateView(window);

    Gpu::Metal::PresenterConfig config{};
    auto device = std::make_unique<Gpu::Metal::Device>();
    auto presenter = std::make_unique<Gpu::Metal::Presenter>(*device, SDL_Metal_GetLayer(view), config);
    auto renderer = std::make_unique<Gpu::Metal::MetalRenderer>(*device);
    Gpu::Liverpool::Instance().SetRenderer(renderer.get());
    const std::string gpu_name = device->Info().name;

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    presenter->Resize(w, h);

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
                presenter->Resize(ev.window.data1, ev.window.data2);
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const bool pressed = (ev.type == SDL_EVENT_KEY_DOWN);
                switch (ev.key.key) {
                case SDLK_V:
                    if (pressed) {
                        config.vsync = !config.vsync;
                        presenter->SetVsync(config.vsync);
                        DC_INFO("vsync: %d", config.vsync);
                    }
                    break;
                case SDLK_RETURN:
                case SDLK_SPACE:
                case SDLK_K:
                    Hle::Pad::SetButton(Hle::Pad::kCross, pressed);
                    break;
                case SDLK_ESCAPE:
                case SDLK_TAB:
                    Hle::Pad::SetButton(Hle::Pad::kOptions, pressed);
                    break;
                case SDLK_L:
                    Hle::Pad::SetButton(Hle::Pad::kCircle, pressed);
                    break;
                case SDLK_J:
                    Hle::Pad::SetButton(Hle::Pad::kSquare, pressed);
                    break;
                case SDLK_I:
                    Hle::Pad::SetButton(Hle::Pad::kTriangle, pressed);
                    break;
                case SDLK_UP:
                case SDLK_W:
                    Hle::Pad::SetButton(Hle::Pad::kUp, pressed);
                    break;
                case SDLK_DOWN:
                case SDLK_S:
                    Hle::Pad::SetButton(Hle::Pad::kDown, pressed);
                    break;
                case SDLK_LEFT:
                case SDLK_A:
                    Hle::Pad::SetButton(Hle::Pad::kLeft, pressed);
                    break;
                case SDLK_RIGHT:
                case SDLK_D:
                    Hle::Pad::SetButton(Hle::Pad::kRight, pressed);
                    break;
                case SDLK_Q:
                    Hle::Pad::SetButton(Hle::Pad::kL1, pressed);
                    break;
                case SDLK_E:
                    Hle::Pad::SetButton(Hle::Pad::kR1, pressed);
                    break;
                default:
                    break;
                }
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                const bool pressed = (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                switch (ev.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_SOUTH:
                    Hle::Pad::SetButton(Hle::Pad::kCross, pressed);
                    break;
                case SDL_GAMEPAD_BUTTON_EAST:
                    Hle::Pad::SetButton(Hle::Pad::kCircle, pressed);
                    break;
                case SDL_GAMEPAD_BUTTON_WEST:
                    Hle::Pad::SetButton(Hle::Pad::kSquare, pressed);
                    break;
                case SDL_GAMEPAD_BUTTON_NORTH:
                    Hle::Pad::SetButton(Hle::Pad::kTriangle, pressed);
                    break;
                case SDL_GAMEPAD_BUTTON_START:
                    Hle::Pad::SetButton(Hle::Pad::kOptions, pressed);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                    Hle::Pad::SetButton(Hle::Pad::kUp, pressed);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    Hle::Pad::SetButton(Hle::Pad::kDown, pressed);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                    Hle::Pad::SetButton(Hle::Pad::kLeft, pressed);
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                    Hle::Pad::SetButton(Hle::Pad::kRight, pressed);
                    break;
                case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                    Hle::Pad::SetButton(Hle::Pad::kL1, pressed);
                    break;
                case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                    Hle::Pad::SetButton(Hle::Pad::kR1, pressed);
                    break;
                default:
                    break;
                }
                break;
            }
            default:
                break;
            }
        }

        if (void* tex = renderer->LatestFrame()) {
            presenter->PresentTexture(tex);
        } else {
            const float t = std::chrono::duration<float>(Clock::now() - start).count();
            presenter->PresentClear(0.5f + 0.5f * std::sin(t), 0.2f, 0.3f);
        }

        ++frames;
        const auto now = Clock::now();
        if (now - fps_window >= std::chrono::seconds(1)) {
            const double secs = std::chrono::duration<double>(now - fps_window).count();
            const double fps = frames / secs;
            char title[96];
            std::snprintf(title, sizeof(title), "Driveclub Native | %s | %.0f FPS",
                          gpu_name.c_str(), fps);
            SDL_SetWindowTitle(window, title);
            frames = 0;
            fps_window = now;
        }
    }

    SDL_Metal_DestroyView(view);
    SDL_DestroyWindow(window);
    SDL_Quit();
    // Guest threads and the GPU thread are still running and use the renderer
    // and HLE state: leave without running destructors under them.
    std::fflush(nullptr);
    std::_Exit(0);
}

} // namespace

int main(int argc, char** argv) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& e) {
        DC_ERROR("fatal: %s", e.what());
        // Guest threads may be running: do not run static destructors under them.
        std::fflush(nullptr);
        std::_Exit(1);
    }
}
