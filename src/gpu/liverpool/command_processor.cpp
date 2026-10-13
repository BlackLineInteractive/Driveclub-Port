#include "gpu/liverpool/command_processor.h"

#include <cstdio>
#include <cstring>

#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include "common/log.h"

namespace Gpu::Liverpool {

namespace {

// EVENT_WRITE_EOP / RELEASE_MEM data and interrupt selects.
enum DataSel : u32 { kDataNone = 0, kData32 = 1, kData64 = 2, kGpuClock = 3, kPerfCounter = 4 };
enum IntSel : u32 { kIntNone = 0, kIntOnly = 1, kIntWriteConfirm = 2, kIntUndocumented = 3 };

u64 GpuClock() {
    // The GPU clock runs at 100 MHz on PS4; derive it from the host clock.
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 10;
}

template <typename T>
T* Ptr(u32 lo, u32 hi) {
    return reinterpret_cast<T*>(u64(lo) | (u64(hi) << 32));
}

void WriteFence(u32 data_sel, u32 lo, u32 hi, u32 data_lo, u32 data_hi) {
    switch (data_sel) {
    case kData32:
        *Ptr<u32>(lo, hi) = data_lo;
        break;
    case kData64:
        *Ptr<u64>(lo, hi) = u64(data_lo) | (u64(data_hi) << 32);
        break;
    case kGpuClock:
        *Ptr<u64>(lo, hi) = GpuClock();
        break;
    case kPerfCounter:
        *Ptr<u64>(lo, hi) = __rdtsc();
        break;
    default:
        break;
    }
}

bool Compare(u32 func, u32 value, u32 ref) {
    switch (func) {
    case 0: return true;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
    }
}

} // namespace

CommandProcessor::CommandProcessor() : regs(kNumRegs, 0) {}

CommandProcessor::~CommandProcessor() {
    for (auto& r : rings) {
        if (r) {
            r->thread.request_stop();
            r->cv.notify_all();
        }
    }
}

CommandProcessor::Ring& CommandProcessor::GetRing(u32 index) {
    std::scoped_lock lk(rings_lock);
    if (rings.size() <= index) {
        rings.resize(index + 1);
    }
    if (!rings[index]) {
        auto ring = std::make_unique<Ring>();
        ring->index = index;
        Ring* r = ring.get();
        r->thread = std::jthread([this, r](std::stop_token st) { Run(*r, st); });
        rings[index] = std::move(ring);
    }
    return *rings[index];
}

void CommandProcessor::Push(u32 index, Job job) {
    Ring& r = GetRing(index);
    {
        std::scoped_lock lk(r.lock);
        r.jobs.push_back(std::move(job));
    }
    r.cv.notify_one();
}

void CommandProcessor::SubmitGraphics(std::vector<std::span<const u32>> dcbs,
                                      std::function<void()> after) {
    ++stats.submits;
    Push(0, {std::move(dcbs), std::move(after)});
}

void CommandProcessor::SubmitCompute(u32 queue, std::span<const u32> acb) {
    Push(queue, {{acb}, {}});
}

void CommandProcessor::WaitIdle() {
    std::vector<Ring*> all;
    {
        std::scoped_lock lk(rings_lock);
        for (auto& r : rings) {
            if (r) {
                all.push_back(r.get());
            }
        }
    }
    for (Ring* r : all) {
        std::unique_lock lk(r->lock);
        r->idle_cv.wait(lk, [&] { return r->jobs.empty() && !r->busy; });
    }
}

void CommandProcessor::Run(Ring& ring, std::stop_token stop) {
    char name[32];
    std::snprintf(name, sizeof(name), ring.index == 0 ? "GPU gfx" : "GPU compute %u", ring.index);
    pthread_setname_np(name);
    for (;;) {
        Job job;
        {
            std::unique_lock lk(ring.lock);
            ring.cv.wait(lk, [&] { return !ring.jobs.empty() || stop.stop_requested(); });
            if (stop.stop_requested()) {
                return;
            }
            job = std::move(ring.jobs.front());
            ring.jobs.pop_front();
            ring.busy = true;
        }
        for (const auto& buf : job.buffers) {
            Execute(buf, ring.index != 0, 0);
        }
        if (job.after) {
            job.after();
        }
        {
            std::scoped_lock lk(ring.lock);
            ring.busy = false;
        }
        ring.idle_cv.notify_all();
    }
}

void CommandProcessor::Interrupt(u32 irq) {
    if (on_irq) {
        on_irq(irq);
    }
}

void CommandProcessor::Execute(std::span<const u32> cmds, bool compute, int depth) {
    const u32* p = cmds.data();
    const u32* end = p + cmds.size();

    while (p < end) {
        const u32 header = *p;
        const u32 type = HeaderType(header);
        if (header == 0 || type == 2) { // filler / trailing padding
            ++p;
            continue;
        }
        if (type == 0) {
            const u32 base_reg = header & 0xFFFF;
            const u32 count = ((header >> 16) & 0x3FFF) + 1;
            if (p + 1 + count > end) {
                break;
            }
            for (u32 i = 0; i < count; ++i) {
                if (base_reg + i < regs.size()) {
                    regs[base_reg + i] = p[1 + i];
                }
            }
            p += 1 + count;
            continue;
        }
        if (type != 3) {
            DC_WARN("gpu: unsupported PM4 packet type %u (header %#x)", type, header);
            return;
        }
        const u32 op = HeaderOpcode(header);
        const u32 body_n = HeaderBodyDwords(header);
        const u32* body = p + 1;
        ++stats.packets;

        switch (op) {
        case Op::Nop:
            break;

        case Op::SetContextReg:
            std::memcpy(&regs[kContextRegBase + body[0]], body + 1, (body_n - 1) * 4);
            break;
        case Op::SetShReg:
            std::memcpy(&regs[kShRegBase + body[0]], body + 1, (body_n - 1) * 4);
            break;
        case Op::SetUconfigReg:
            std::memcpy(&regs[kUconfigRegBase + body[0]], body + 1, (body_n - 1) * 4);
            break;
        case Op::SetConfigReg:
            std::memcpy(&regs[kConfigRegBase + body[0]], body + 1, (body_n - 1) * 4);
            break;

        case Op::IndirectBuffer: {
            const u32* ib = Ptr<const u32>(body[0], body[1] & 0xFFFF);
            const u32 size = body[2] & 0xFFFFF;
            if (depth < 4) {
                Execute({ib, size}, compute, depth + 1);
            }
            break;
        }

        case Op::EventWriteEop: {
            const u32 data_control = body[2];
            WriteFence((data_control >> 29) & 7, body[1], data_control & 0xFFFF, body[3],
                       body[4]);
            const u32 int_sel = (data_control >> 24) & 3;
            if (int_sel == kIntOnly || int_sel == kIntWriteConfirm) {
                Interrupt(kIrqGfxEop);
            }
            break;
        }
        case Op::ReleaseMem: {
            const u32 dw2 = body[1];
            WriteFence((dw2 >> 29) & 7, body[2], body[3], body[4], body[5]);
            const u32 int_sel = (dw2 >> 24) & 7;
            if (int_sel == kIntWriteConfirm || int_sel == kIntUndocumented) {
                Interrupt(compute ? 0 : kIrqGfxEop);
            }
            break;
        }
        case Op::EventWriteEos: {
            const u32 cmd = (body[2] >> 29) & 7;
            if (cmd == 2) { // signal fence
                *Ptr<u32>(body[1], body[2] & 0xFFFF) = body[3];
            }
            break;
        }

        case Op::WriteData: {
            const u32 dst_sel = (body[0] >> 8) & 0xF;
            if (dst_sel == 1 || dst_sel == 2 || dst_sel == 5) {
                std::memcpy(Ptr<u32>(body[1], body[2]), body + 3, (body_n - 3) * 4);
            }
            break;
        }

        case Op::WaitRegMem: {
            const u32 func = body[0] & 7;
            const bool memory = (body[0] >> 4) & 1;
            const u32 ref = body[3];
            const u32 mask = body[4];
            if (memory) {
                const volatile u32* addr = Ptr<const u32>(body[1] & ~3u, body[2]);
                u32 iters = 0;
                while (!Compare(func, *addr & mask, ref)) {
                    if (++iters > 1000000) {
                        DC_WARN("gpu: WaitRegMem timeout (addr=%p val=%#x ref=%#x mask=%#x func=%u)",
                                addr, *addr, ref, mask, func);
                        break;
                    }
                    sched_yield();
                }
            }
            break;
        }

        case Op::DrawIndex2:
        case Op::DrawIndexAuto:
        case Op::DrawIndexOffset2:
        case Op::DrawIndirect:
        case Op::DrawIndexIndirect:
            ++stats.draws;
            break;

        case Op::DispatchDirect:
        case Op::DispatchIndirect:
            ++stats.dispatches;
            break;

        default:
            break;
        }
        p += body_n + 1;
    }
}

CommandProcessor& Instance() {
    static CommandProcessor cp;
    return cp;
}

} // namespace Gpu::Liverpool
