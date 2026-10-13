#pragma once

// Owns the MTLDevice and the single command queue all GPU work goes through.
// The header stays pure C++ so non-Metal code can hold a Device without ObjC.

#include <cstdint>
#include <memory>
#include <string>

namespace Gpu::Metal {

struct DeviceInfo {
    std::string name;
    bool unified_memory;        // false on the discrete Radeon: CPU<->GPU copies are real
    bool argument_buffers_tier2;
    std::uint64_t working_set_bytes;
};

class Device {
public:
    // Picks the discrete GPU (the AMD on dual-GPU MacBooks) unless DC_GPU
    // names another one by substring.
    Device();
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    const DeviceInfo& Info() const { return info; }

    // Opaque id<MTLDevice> / id<MTLCommandQueue>, bridged in .mm files.
    void* Handle() const;
    void* Queue() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    DeviceInfo info{};
};

} // namespace Gpu::Metal
