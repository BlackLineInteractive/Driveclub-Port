// Driveclub Native: stand-in for shadPS4's libkernel process header. The GPU
// code only asks whether it runs on a PS4 Pro; Driveclub targets the base PS4.

#pragma once

namespace Libraries::Kernel {

inline bool sceKernelIsNeoMode() {
    return false;
}

} // namespace Libraries::Kernel
