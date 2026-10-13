#pragma once

// The guest GPU front end: executes PM4 command streams the game submits.
//
// Submissions are queued and run on a dedicated GPU thread, like the real
// command processor, so the game keeps running while "the GPU" works. Sync
// packets (fences, waits, memory writes, interrupts) are executed here;
// draws and dispatches are handed to a Gpu::Renderer.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "common/types.h"
#include "gpu/liverpool/pm4.h"

namespace Gpu::Liverpool {

class CommandProcessor {
public:
    // Interrupt sources the game can listen to (sceGnmAddEqEvent ids).
    static constexpr u32 kIrqGfxEop = 0x40;

    struct Stats {
        std::atomic<u64> packets{0};
        std::atomic<u64> draws{0};
        std::atomic<u64> dispatches{0};
        std::atomic<u64> submits{0};
    };

    CommandProcessor();
    ~CommandProcessor();

    // Graphics ring: draw command buffers (+ constant buffers, ignored for
    // now). `after` runs on the GPU thread once they are done (used for flips).
    void SubmitGraphics(std::vector<std::span<const u32>> dcbs, std::function<void()> after = {});
    // Compute ring `queue` (1-based Gnm vqid).
    void SubmitCompute(u32 queue, std::span<const u32> acb);

    // Blocks until every queued submission has executed.
    void WaitIdle();

    void SetInterruptHandler(std::function<void(u32 irq)> handler) { on_irq = std::move(handler); }

    const Stats& GetStats() const { return stats; }
    const u32* Regs() const { return regs.data(); }

private:
    struct Job {
        std::vector<std::span<const u32>> buffers;
        std::function<void()> after;
    };

    // One hardware ring: the graphics ring or a compute queue. Rings run in
    // parallel, as on the GPU, so a wait on one can be satisfied by another.
    struct Ring {
        u32 index = 0; // 0 = graphics, n = compute vqid n
        std::mutex lock;
        std::condition_variable cv;
        std::condition_variable idle_cv;
        std::deque<Job> jobs;
        bool busy = false;
        std::jthread thread;
    };

    Ring& GetRing(u32 index);
    void Push(u32 ring, Job job);
    void Run(Ring& ring, std::stop_token stop);
    void Execute(std::span<const u32> cmds, bool compute, int depth);
    void Interrupt(u32 irq);

    std::vector<u32> regs;
    Stats stats;
    std::function<void(u32)> on_irq;

    std::mutex rings_lock;
    std::vector<std::unique_ptr<Ring>> rings;
};

CommandProcessor& Instance();

} // namespace Gpu::Liverpool
