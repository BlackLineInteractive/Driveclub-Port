# shadPS4 GPU code (vendored)

The Vulkan renderer of Driveclub Native is shadPS4's: `src/video_core` and
`src/shader_recompiler`, plus the parts of `src/common` they use, copied from
[shadPS4](https://github.com/shadps4-emu/shadPS4) 0.19.1 (October 2026).
Copyright shadPS4 Emulator Project and the authors named in each file;
GPL-2.0-or-later (see LICENSE), like the rest of this repository.

How it is wired in:

- `cmake/ShadPS4Vulkan.cmake` builds it as the static library `dc_shadps4_vulkan`.
- `src/gpu/vulkan/shim/` replaces the shadPS4 headers this code expects but we
  do not take (emulator settings, memory manager, signal dispatch, logging
  backend, ImGui, SDL window, the presenter's `Frame`, libkernel bits).
- `src/gpu/vulkan/backend/dc_vulkan.cpp` drives the rasterizer from our PM4
  command processor and presents frames; `src/gpu/vulkan/vk_renderer.cpp`
  exposes that as a `Gpu::Renderer`.

Not taken: shadPS4's PM4 front end (`amdgpu/liverpool.cpp`), its ImGui-based
presenter (`vk_presenter.*`), FSR, RenderDoc integration.

## Local changes

Every change is marked with a `Driveclub:` comment.

- `video_core/amdgpu/liverpool.h`: `SendCommand` runs work under a GPU-thread
  lock instead of queueing it to shadPS4's command thread; backend hooks
  (`DcSetGpuThread`, `DcComputeState`).
- `video_core/texture_cache/texture_cache.h`: std containers instead of Abseil.
- `video_core/renderer_vulkan/vk_platform.cpp`, `vk_instance.cpp`: on macOS,
  use the system Vulkan driver (MoltenVK) unless one is bundled, enable
  portability enumeration / `VK_KHR_portability_subset`, and accept a driver
  without `robustness2`.
- `common/assert.cpp`, `common/thread.cpp`, `common/path_util.cpp`: dropped
  references to shadPS4's emulator, guest threads and ZArchive.

To update: copy the same directories from a newer shadPS4, reapply the
changes above, and fix the shims where interfaces moved.
