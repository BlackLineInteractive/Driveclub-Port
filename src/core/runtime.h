#pragma once

#include "common/types.h"

// Runs the guest: a dedicated thread with guest TLS starts the preloaded Sony
// modules, then jumps to the executable's entry point. The host main thread
// stays free for the window and presentation (Cocoa requires it).

namespace Core::Loader {
class Linker;
}

namespace Core::Runtime {

// Starts the guest main thread and returns immediately.
void StartGame(Loader::Linker& linker);

// Calls a module's init (module_start) once; returns its result.
int StartModule(const char* name, u64 args = 0, const void* argp = nullptr);

} // namespace Core::Runtime
