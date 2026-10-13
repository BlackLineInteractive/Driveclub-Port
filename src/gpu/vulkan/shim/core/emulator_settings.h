// Driveclub Native: stand-in for shadPS4's core/emulator_settings.h.
//
// The vendored shadPS4 GPU code (third_party/shadps4) reads its options
// through `EmulatorSettings.Xxx()`. We have no settings UI or JSON config, so
// every option has a fixed default here; the useful ones can be overridden
// with DC_VK_* environment variables (see emulator_settings.cpp).

#pragma once

#include <string>

#include "common/types.h"

#define EmulatorSettings (*EmulatorSettingsImpl::GetInstance())

enum GpuReadbacksMode : int {
    Disabled,
    Relaxed,
    Precise,
};

class EmulatorSettingsImpl {
public:
    static EmulatorSettingsImpl* GetInstance();

    // ---- GPU ----
    bool IsNullGPU() const { return false; }
    bool IsCopyGpuBuffers() const { return false; }
    u32 GetReadbacksMode() const { return readbacks_mode; }
    bool IsReadbackLinearImagesEnabled() const { return false; }
    bool IsDirectMemoryAccessEnabled() const { return direct_memory_access; }
    bool IsDumpShaders() const { return dump_shaders; }
    bool IsPatchShaders() const { return false; }
    bool IsInlineFetchShader() const { return false; }
    std::string GetPresentMode() const { return present_mode; }
    bool IsHdrAllowed() const { return false; }
    bool IsFsrEnabled() const { return false; }
    bool IsRcasEnabled() const { return false; }
    int GetRcasAttenuation() const { return 250; }
    bool IsShaderCollect() const { return false; }
    bool IsUserfaultfdTracking() const { return false; }
    bool IsPipelineCacheEnabled() const { return pipeline_cache; }
    bool IsPipelineCacheArchived() const { return false; }

    // ---- Vulkan ----
    s32 GetGpuId() const { return gpu_id; }
    bool IsRenderdocEnabled() const { return false; }
    bool IsVkValidationEnabled() const { return validation; }
    bool IsVkValidationCoreEnabled() const { return validation; }
    bool IsVkValidationSyncEnabled() const { return false; }
    bool IsVkValidationGpuEnabled() const { return false; }
    bool IsVkCrashDiagnosticEnabled() const { return false; }
    bool IsVkHostMarkersEnabled() const { return markers; }
    bool IsVkGuestMarkersEnabled() const { return markers; }

    // Where shader dumps and the pipeline cache go.
    const std::string& DataDir() const { return data_dir; }

    void SetPresentMode(std::string mode) { present_mode = std::move(mode); }

private:
    EmulatorSettingsImpl();

    u32 readbacks_mode = GpuReadbacksMode::Disabled;
    bool direct_memory_access = false;
    bool dump_shaders = false;
    bool pipeline_cache = false;
    std::string present_mode = "Fifo";
    s32 gpu_id = -1;
    bool validation = false;
    bool markers = false;
    std::string data_dir;
};
