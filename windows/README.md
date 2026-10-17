# DCport — Driveclub on Windows

The Windows backend of Driveclub Native: DRIVECLUB (PS4, CUSA00003, v1.28) running natively on Windows. The game's own x86-64 code runs directly on the PC; its graphics are translated to Vulkan (shadPS4's renderer).

![DCport launcher](media/launcher.png)

![DRIVECLUB BIKES on Windows, FSR 4](media/ingame-bike.png)
![Event preview on Windows, FSR 4](media/ingame-track.png)

## ⚡ New in 0.3.0: FSR 4.1.1, built on your own PC, in one click

AMD ships FSR 4.1.1 as a DirectX 12 DLL. DCport's **FSR 4.1.1 builder** turns your own copy of that DLL into a native **Vulkan** upscaler for Driveclub — no Wine, no Linux, no tools to install:

1. SETUP → Upscaling → **Set up FSR 4.1.1…**
2. Pick your `amd_fidelityfx_upscaler_dx12.dll` (4.1.x).
3. About two minutes later, choose **FSR 4.1.1** as the upscaler. Done.

What happens in those two minutes, all on your PC:

- **Records the real thing.** The builder runs your DLL once on DirectX 12 at 1080p and 1440p and records every pass of AMD's INT8 neural network, its weights and its constants (`fsr4cap`).
- **Translates it to Vulkan.** Each pass's DXIL shader becomes SPIR-V (`dxil-spirv`, the translator vkd3d-proton uses), in a portable form that runs on Windows drivers, which lack the Linux-only extension the exact translation needs.
- **Repairs stripped shaders.** DLLs with performance-rewritten shaders lose their resource names; the builder restores them from AMD's original shaders (downloaded from AMD's public FidelityFX SDK and only read, never run).
- **Checks everything.** The dispatch sequence, group counts, tensor sizes and constants of every pass are checked against the rules the Vulkan replay uses before anything is written.

The result, `fsr4_411/`, is replayed by the runtime every frame (about 1 ms at 1080p). It works with **frame generation** too: `FSR 4.1.1 + FG` at 120 FPS (60 base) on an RX 6750 XT, an RDNA 2 card. Without the window: `DriveClub.exe --setup-fsr411 <path to the DLL>`.

Nothing of AMD's is in the download or in this repository: every player builds it from their own DLL. AMD's own DLL offers FSR 4.1.1 on Radeon RX 9000 cards only; other cards need a version of the DLL that lifts that limit.

Also new: **FSR in every race** (0.2.0's FSR ran in menus only), **FSR 3 frame generation** with a HUD-less copy of each frame, a **PATCHES** tab, window size and render resolution settings.

## Download

Get the latest release from the Releases page:

- `DCport-<version>-windows-fsr.zip`: with FSR 3.1 and FSR 4 (FSR 4 needs a recent AMD or NVIDIA GPU)
- `DCport-<version>-windows.zip`: without upscaling

No game files are included. You need your own dump of DRIVECLUB 1.28 (the folder with `eboot.bin` and the `game*.dat` files).

## Playing

1. Unzip anywhere and start `DriveClub.exe`.
2. The first time, choose your game folder. On the first PLAY, DCport prepares the game files on your PC (once, under a minute).
3. The launcher has six tabs:
   - **RACE**: game art, ready checks, PLAY, quick frame rate, window and FPS counter
   - **SETUP**: game folder, frame rate (30–240, tested up to 120), window mode and size, render resolution (1080p–4K), FPS counter; FSR edition: upscaler, sharpness, frame generation, FSR 4.1.1 set-up
   - **PATCHES**: community game patches (frame-rate patch type, no barriers, no reset, no motion blur/DOF, photo mode)
   - **CONTROLS**: keyboard and controller bindings
   - **GARAGE**: save profiles, backups with restore, shader cache, desktop shortcut, Add to Steam
   - **LOG**: last run's log, "Copy for bug report"
4. `DriveClub.exe --play` starts the game directly (used by the desktop shortcut and Steam entry).
5. DLC: put your own dumped DLC folders (each with its own `sce_sys`) inside your game folder.
6. In game, Insert (or L3+R3) opens the settings menu.

Requirements: Windows 10/11 64-bit, a CPU with AVX, a Vulkan 1.3 GPU.

### FSR edition extras (0.3.0)

- **FSR in every race:** Driveclub's TAA resolve comes in three variants (menus, races, others); FSR 3.1/4 replace all of them. The FPS counter names the upscaler only while it really runs ("TAA" otherwise).
- **Frame generation:** FSR 3 frame interpolation (FSR-Vulkan) with a HUD-less copy of each frame; the game runs at half the chosen rate and every frame gets a generated one (120 FPS = 60 + 60). The counter shows both rates.
- **FSR 4.1.1 (optional, bring your own DLL):** SETUP > *Set up FSR 4.1.1…* runs your own `amd_fidelityfx_upscaler_dx12.dll` once on DirectX 12, translates its shaders to SPIR-V on your PC (`bin/fsr411`: fsr4cap, dxil-spirv, SPIRV-Tools) and writes `fsr4_411/`, which the runtime replays in Vulkan. Nothing of AMD's is in the release or in this repository; AMD's FidelityFX loader is downloaded from AMD's public SDK repository during set-up. AMD's own DLL offers FSR 4.1.1 on Radeon RX 9000 only.

## Source in this folder

- `launcher/`: the DCport launcher (Python 3.12, Tkinter, Pillow; frozen with PyInstaller via `build.sh fsr|nofsr`) and the first-run prepare tool (`launcher/prepare/`). Tests: `python -m pytest tests`.
- `runtime/driveclub-runtime.patch`: the Driveclub changes to the native PS4 runtime, against [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) commit `fce08780508a72f50d34a3d3a2926e746d12284c` (version 1.8.2). Apply with `git apply`, then build with `build.sh` in MSYS2 CLANG64.
- `runtime/fsr-vulkan.patch.txt`: one small change to its FSR-Vulkan submodule.
- `patches/Driveclub.xml`: community patches (60 FPS with deltatime and others).

## Windows port credits

DCport is built on other people's work: the native PS4 runtime by deadinside28 and Supermedo, and shadPS4's renderer. The Driveclub support, the fixes that make it playable (graphics glitches, lighting, frame rate, fonts, crashes), the launcher and the tools are new.

- **[BlackLineInteractive](https://github.com/BlackLineInteractive/Driveclub-Port):** the original Driveclub Native project and the macOS backend; reference for Driveclub's system library behaviour.
- **Native PS4 runtime:** deadinside28 ([bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)) and the Windows port by Supermedo ([bloodborne_pc](https://github.com/Supermedo/bloodborne_pc)).
- **[shadPS4](https://github.com/shadps4-emu/shadPS4):** renderer, shader recompiler and libraries the runtime is built on.
- **[akitaonrails](https://github.com/akitaonrails/shadPS4):** shadPS4 investigation of Driveclub (MSAA depth resolve).
- **Also used:** [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK (FSR, FSR 3 frame generation), FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX); [dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) (MIT) and [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) (Apache 2.0) for the FSR 4.1.1 set-up; FreeType, SDL3, FFmpeg and the other libraries listed in `licenses/`.
- **Community patches** ([shadps4-emu/ps4_cheats](https://github.com/shadps4-emu/ps4_cheats)): illusion, u/Charlie_Muggins, kalaposfos.
- Windows port by **qnfn** (system functions, fonts, readbacks, upscaler integration, launcher).

## Legal disclaimer

- **No Proprietary Game Files:** This project does not distribute, host, or link to any copyrighted material, proprietary game assets, PKG dumps, ROMs, executables (eboot.bin), libraries (.prx), engine archives (.dat), audio, or textures. Users must provide their own legitimately acquired copy of Driveclub (CUSA00003) and of any DLC. On Windows, DCport reads it from your folder and builds its working files on your own PC; nothing from the game is ever uploaded or shared. Please do not ask for, or share, game files anywhere in this project.
- **Intellectual Property:** Driveclub is a registered trademark of Sony Interactive Entertainment LLC. All trademarks, service marks, game titles, and copyrighted assets belong to Sony Interactive Entertainment and Evolution Studios. PlayStation and PS4 are trademarks of Sony Interactive Entertainment Inc.
- **Educational and Preservation Purpose:** This repository is an independent reverse-engineering project conducted strictly for interoperability, software preservation, and educational purposes. It is not affiliated with, endorsed by, or sponsored by Sony Interactive Entertainment, Evolution Studios, or any associated entities.
- **Free:** DCport is free and always will be. If someone sold it to you, ask for your money back.
- **FSR (Windows):** AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc.; DCport is not affiliated with AMD. The FSR edition's download includes FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX) under AMD's MIT license (`fsr4_shaders/LICENSE-FSR4-v07.txt`). They are not part of this repository. FSR 4.1.1 is never distributed: it is built on the player's PC from their own DLL.
- **No Warranty:** DCport is provided "as is", without warranty of any kind (see the GPL). Use it at your own risk and keep a backup of your saves.
- Rights holders with a concern can open an issue on this repository; we will respond promptly.

## License

GPL-2.0-or-later, like the runtime and shadPS4. Third-party components keep their own licenses.
