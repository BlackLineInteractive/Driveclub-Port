#pragma once

// Background Metal shader compilation.
//
// Metal compiles MSL, and again per pipeline, synchronously and slowly
// (seconds for large shaders on AMD). The renderer hands that work to a few
// worker threads so the GPU thread keeps going; Metal's compiler service runs
// the requests in parallel.

#import <Metal/Metal.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace Gpu::Metal {

class ShaderCompiler {
public:
    ShaderCompiler(id<MTLDevice> device, u32 threads);
    ~ShaderCompiler();

    // The "main0" function of MSL `src`, cached by `key`; nil if it does not
    // compile. Thread-safe.
    id<MTLFunction> Function(u64 key, const std::string& src, u64 address);

    // Runs `job` on a worker thread.
    void Submit(std::function<void()> job);

private:
    void Worker();

    id<MTLDevice> device;
    std::mutex lock;
    std::condition_variable cv;
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> threads;
    bool stop = false;

    std::mutex fn_lock;
    std::unordered_map<u64, id<MTLFunction>> functions;
};

} // namespace Gpu::Metal
