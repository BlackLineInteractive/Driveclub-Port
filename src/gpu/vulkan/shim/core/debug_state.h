// Driveclub Native: stand-in for shadPS4's core/debug_state.h (its devtools
// overlay). The renderer only reports counters and collected shaders to it;
// we have no overlay, so those calls do nothing.

#pragma once

#include <span>
#include <string>

#include "common/types.h"

#define DebugState (::DebugStateType::g_state)

namespace DebugStateType {

struct State {
    template <typename... Args>
    void CollectShader(Args&&...) {}
    void IncDrawCall() {}
    void IncDispatch() {}
    void IncFlipFrameNum() {}
};

inline State g_state;

} // namespace DebugStateType
