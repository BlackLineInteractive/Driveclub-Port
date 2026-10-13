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
#include "gpu/liverpool/registers.h"

namespace Gpu {
class Renderer;
}

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

    // One draw command buffer and the constant command buffer that feeds it
    // (run by the constant engine, CE, alongside the draw engine, DE).
    struct CmdBuffers {
        std::span<const u32> dcb;
        std::span<const u32> ccb;
    };

    // Graphics ring. `after` runs on the GPU thread once the buffers are done
    // (used for flips).
    void SubmitGraphics(std::vector<CmdBuffers> bufs, std::function<void()> after = {});
    // Compute ring `queue` (1-based Gnm vqid).
    void SubmitCompute(u32 queue, std::span<const u32> acb);

    // Blocks until every queued submission has executed.
    void WaitIdle();

    void SetInterruptHandler(std::function<void(u32 irq)> handler) { on_irq = std::move(handler); }
    void SetRenderer(Gpu::Renderer* r) { renderer = r; }
    Gpu::Renderer* GetRenderer() const { return renderer; }

    const Stats& GetStats() const { return stats; }
    const RegFile& Regs() const { return regs; }

private:
    struct Job {
        std::vector<CmdBuffers> buffers;
        std::function<void()> after;
    };

    // Constant engine: fills its 48 KB RAM from the CCB and dumps it to
    // memory (descriptor tables) for the DE. It runs ahead of the DE, synced
    // through two counters; here it runs on demand whenever the DE waits.
    struct ConstantEngine {
        std::vector<std::span<const u32>> stack; // CCB + IB_CONST nesting
        u64 ce_counter = 0;
        u64 de_counter = 0;
        std::vector<u32> ram = std::vector<u32>(0xC000 / 4, 0);
        std::vector<u64> dumped; // dump targets since the renderer last went idle
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
    void SetRegs(u32 index, const u32* values, u32 count);
    void Draw(bool indexed, u32 count, u32 index_offset);
    void SyncRenderer();
    void ClearState();
    // Runs the CE until `done()` holds, it blocks on the DE, or its CCB ends.
    void RunConstantEngine(const std::function<bool()>& done);

    RegFile regs;
    ConstantEngine ce;
    // Draw state set by packets rather than registers.
    u64 index_base = 0;
    u32 index_size = 2;
    u32 num_instances = 1;
    bool warned_indirect = false;
    Gpu::Renderer* renderer = nullptr;
    Stats stats;
    std::function<void(u32)> on_irq;

    std::mutex rings_lock;
    std::vector<std::unique_ptr<Ring>> rings;
};

CommandProcessor& Instance();

} // namespace Gpu::Liverpool
