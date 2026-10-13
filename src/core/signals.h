#pragma once

// Host signal handling for guest code:
//  - SIGILL: patches AMD-only SSE4a instructions (the PS4's Jaguar CPU has
//    them, Intel CPUs do not) into jumps to equivalent SSE2 sequences, once
//    per site, so they then run natively.
//  - SIGSEGV/SIGBUS from write-watched pages: handed to the renderer.
//  - SIGSEGV/SIGBUS/SIGILL that we cannot handle: prints a crash report with
//    the guest module and offset.

#include <string>

#include "common/types.h"

namespace Core::Signals {

// Describes a host address as "module+offset" for crash reports.
using AddressDescriber = std::string (*)(u64 address);

void Install(AddressDescriber describer);

// Gives the patcher room for trampolines near a code range (rel32 jumps must
// reach). `area` must be mapped RWX and within 2 GB of the code.
void AddPatchArea(VAddr code_begin, VAddr code_end, VAddr area, u64 area_size);

// Runs first on SIGSEGV/SIGBUS with the ucontext_t* and the fault address;
// returning true resumes the faulting code (the GPU renderer write-watches
// guest pages this way).
using FaultHandler = bool (*)(void* ucontext, void* fault_address);
void SetFaultHandler(FaultHandler handler);

} // namespace Core::Signals
