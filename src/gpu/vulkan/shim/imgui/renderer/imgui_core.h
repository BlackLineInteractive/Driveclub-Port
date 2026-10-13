// Driveclub Native: shadPS4 draws its overlay (and the game image) with Dear
// ImGui. We present frames ourselves, so these hooks do nothing.

#pragma once

namespace ImGui::Core {

template <typename... Args>
inline void Initialize(Args&&...) {}
template <typename... Args>
inline void OnSurfaceFormatChange(Args&&...) {}
template <typename... Args>
inline void Shutdown(Args&&...) {}

} // namespace ImGui::Core
