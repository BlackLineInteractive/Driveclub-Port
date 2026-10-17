# DCport — Driveclub on Windows

The Windows backend of Driveclub Native: DRIVECLUB (PS4, CUSA00003, v1.28) running natively on Windows. The game's own x86-64 code runs directly on the PC; its graphics are translated to Vulkan (shadPS4's renderer).

![DCport launcher](media/launcher.png)

![DRIVECLUB BIKES on Windows, FSR 4](media/ingame-bike.png)
![Event preview on Windows, FSR 4](media/ingame-track.png)

## Download

Get the latest release from the Releases page:

- `DCport-<version>-windows-fsr.zip`: with FSR 3.1 and FSR 4 (FSR 4 needs a recent AMD or NVIDIA GPU)
- `DCport-<version>-windows.zip`: without upscaling

No game files are included. You need your own dump of DRIVECLUB 1.28 (the folder with `eboot.bin` and the `game*.dat` files).

## Playing

1. Unzip anywhere and start `DriveClub.exe`.
2. The first time, choose your game folder. On the first PLAY, DCport prepares the game files on your PC (once, under a minute).
3. The launcher has five tabs:
   - **RACE**: game art, ready checks, PLAY, quick frame rate, window and FPS counter
   - **SETUP**: game folder, frame rate (30–240, tested up to 120), window mode, FPS counter, upscaler (FSR edition)
   - **CONTROLS**: keyboard and controller bindings
   - **GARAGE**: save profiles, backups with restore, shader cache, desktop shortcut, Add to Steam
   - **LOG**: last run's log, "Copy for bug report"
4. `DriveClub.exe --play` starts the game directly (used by the desktop shortcut and Steam entry).
5. DLC: put your own dumped DLC folders (each with its own `sce_sys`) inside your game folder.
6. In game, Insert (or L3+R3) opens the settings menu.

Requirements: Windows 10/11 64-bit, a CPU with AVX, a Vulkan 1.3 GPU.

## Source in this folder

- `launcher/`: the DCport launcher (Python 3.12, Tkinter, Pillow; frozen with PyInstaller via `build.sh fsr|nofsr`) and the first-run prepare tool (`launcher/prepare/`). Tests: `python -m pytest tests`.
- `runtime/driveclub-runtime.patch`: the Driveclub changes to the native PS4 runtime, against [Supermedo/bloodborne_pc](https://github.com/Supermedo/bloodborne_pc) commit `fce08780508a72f50d34a3d3a2926e746d12284c` (version 1.8.2). Apply with `git apply`, then build with `build.sh` in MSYS2 CLANG64.
- `runtime/fsr-vulkan.patch.txt`: one small change to its FSR-Vulkan submodule.
- `patches/Driveclub.xml`: community patches (60 FPS with deltatime and others).

## Windows port credits

DCport (the Windows backend) is a modified version of other people's work; almost all of it is theirs.

- **Native PS4 runtime:** deadinside28 ([bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)) and the Windows port by Supermedo ([bloodborne_pc](https://github.com/Supermedo/bloodborne_pc)).
- **[shadPS4](https://github.com/shadps4-emu/shadPS4):** renderer, shader recompiler and libraries the runtime is built on.
- **[BlackLineInteractive](https://github.com/BlackLineInteractive/Driveclub-Port):** Driveclub Native for macOS; reference for Driveclub's system library behaviour (fonts and others).
- **[akitaonrails](https://github.com/akitaonrails/shadPS4):** shadPS4 investigation of Driveclub (MSAA depth resolve).
- **Also used:** [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the AMD FidelityFX SDK (FSR), FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX); FreeType, SDL3, FFmpeg and the other libraries listed in `licenses/`.
- **Community patches** ([shadps4-emu/ps4_cheats](https://github.com/shadps4-emu/ps4_cheats)): illusion, u/Charlie_Muggins, kalaposfos.
- Windows port by **qnfn**; the Driveclub-specific changes (system functions, fonts, readbacks, upscaler integration, launcher) were written with the help of Claude (Anthropic).

## Legal disclaimer

- **No Proprietary Game Files:** This project does not distribute, host, or link to any copyrighted material, proprietary game assets, PKG dumps, ROMs, executables (eboot.bin), libraries (.prx), engine archives (.dat), audio, or textures. Users must provide their own legitimately acquired copy of Driveclub (CUSA00003) and of any DLC. On Windows, DCport reads it from your folder and builds its working files on your own PC; nothing from the game is ever uploaded or shared. Please do not ask for, or share, game files anywhere in this project.
- **Intellectual Property:** Driveclub is a registered trademark of Sony Interactive Entertainment LLC. All trademarks, service marks, game titles, and copyrighted assets belong to Sony Interactive Entertainment and Evolution Studios. PlayStation and PS4 are trademarks of Sony Interactive Entertainment Inc.
- **Educational and Preservation Purpose:** This repository is an independent reverse-engineering project conducted strictly for interoperability, software preservation, and educational purposes. It is not affiliated with, endorsed by, or sponsored by Sony Interactive Entertainment, Evolution Studios, or any associated entities.
- **Free:** DCport is free and always will be. If someone sold it to you, ask for your money back.
- **FSR (Windows):** AMD, FidelityFX and FSR are trademarks of Advanced Micro Devices, Inc.; DCport is not affiliated with AMD. The FSR edition's download includes FSR 4 assets from [FireBurn/Q2RTX](https://github.com/FireBurn/Q2RTX) under AMD's MIT license (`fsr4_shaders/LICENSE-FSR4-v07.txt`). They are not part of this repository.
- **No Warranty:** DCport is provided "as is", without warranty of any kind (see the GPL). Use it at your own risk and keep a backup of your saves.
- Rights holders with a concern can open an issue on this repository; we will respond promptly.

## License

GPL-2.0-or-later, like the runtime and shadPS4. Third-party components keep their own licenses.
