#include "gpu/liverpool/command_processor.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include "common/log.h"
#include "gpu/renderer.h"

namespace Gpu::Liverpool {

namespace {

// EVENT_WRITE_EOP / RELEASE_MEM data and interrupt selects.
enum DataSel : u32 { kDataNone = 0, kData32 = 1, kData64 = 2, kGpuClock = 3, kPerfCounter = 4 };
// NOP payload markers the Gnm driver uses for flip requests.
constexpr u32 kNopPrepareFlipLabel = 0x68750778;
constexpr u32 kNopPrepareFlipInterrupt = 0x68750780;
constexpr u32 kNopPrepareFlipInterruptLabel = 0x68750781;

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

thread_local CommandProcessor::Ring* CommandProcessor::t_ring = nullptr;
thread_local RegFile* CommandProcessor::t_regs = nullptr;

CommandProcessor::CommandProcessor() {
    trace = std::getenv("DC_CP_TRACE") != nullptr;
    ClearState();
}

// CLEAR_STATE: context registers back to their power-on values. Most are 0;
// these are the ones whose default matters (from AMD's CIK clear-state table).
void CommandProcessor::ClearState() {
    std::memset(regs.Data() + kContextRegBase, 0, 0x400 * 4);
    regs[Reg::PA_SC_SCREEN_SCISSOR_BR] = 0x40004000;
    regs[Reg::PA_SC_WINDOW_SCISSOR_TL] = 0x80000000;
    regs[Reg::PA_SC_WINDOW_SCISSOR_BR] = 0x40004000;
    regs[0xA083] = 0xFFFF; // PA_SC_CLIPRECT_RULE
    for (u32 k = 0; k < 4; ++k) {
        regs[0xA085 + 2 * k] = 0x20002000; // PA_SC_CLIPRECT_n_BR
    }
    regs[0xA08C] = 0xAA99AAAA; // PA_SC_EDGERULE
    regs[Reg::PA_SC_GENERIC_SCISSOR_TL] = 0x80000000;
    regs[Reg::PA_SC_GENERIC_SCISSOR_BR] = 0x40004000;
    for (u32 k = 0; k < 16; ++k) {
        regs[Reg::PA_SC_VPORT_SCISSOR_0_TL + 2 * k] = 0x80000000;
        regs[Reg::PA_SC_VPORT_SCISSOR_0_TL + 2 * k + 1] = 0x40004000;
        regs[Reg::PA_SC_VPORT_ZMIN_0 + 2 * k + 1] = 0x3F800000; // ZMAX = 1.0
    }
}

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

void CommandProcessor::SubmitGraphics(std::vector<CmdBuffers> bufs, std::function<void()> after) {
    ++stats.submits;
    Push(0, {std::move(bufs), std::move(after)});
}

void CommandProcessor::SubmitCompute(u32 queue, std::span<const u32> acb) {
    Push(queue, {{CmdBuffers{acb, {}}}, {}});
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
    t_ring = &ring;
    t_regs = ring.index == 0 ? &regs : &ring.regs;
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
            if (ring.index == 0) {
                ce.stack.clear();
                if (!buf.ccb.empty()) {
                    ce.stack.push_back(buf.ccb);
                }
                ce.ce_counter = ce.de_counter = 0;
            }
            Execute(buf.dcb, ring.index != 0, 0);
            if (ring.index == 0) {
                RunConstantEngine([] { return false; }); // whatever the CE has left
            }
        }
        if (renderer) {
            renderer->OnSubmit();
        }
        if (job.after) {
            job.after();
        }
        if (flip_label.addr || flip_label.interrupt) {
            if (flip_label.addr) {
                *Ptr<u32>(static_cast<u32>(flip_label.addr), static_cast<u32>(flip_label.addr >> 32)) = flip_label.value;
            }
            if (flip_label.interrupt) {
                Interrupt(kIrqGfxEop);
            }
            flip_label = {};
        }
        {
            std::scoped_lock lk(ring.lock);
            ring.busy = false;
        }
        ring.idle_cv.notify_all();
    }
}

RegFile& CommandProcessor::CurRegs() {
    return t_regs ? *t_regs : regs;
}

void CommandProcessor::SetRegs(u32 index, const u32* values, u32 count) {
    if (index + count > RegFile::kSize) {
        return;
    }
    std::memcpy(CurRegs().Data() + index, values, count * 4);
}

void CommandProcessor::SyncRenderer() {
    if (renderer) {
        renderer->WaitIdle();
    }
    ce.dumped.clear();
}

void CommandProcessor::RunConstantEngine(const std::function<bool()>& done) {
    while (!ce.stack.empty() && !done()) {
        std::span<const u32>& cur = ce.stack.back();
        if (cur.empty()) {
            ce.stack.pop_back();
            continue;
        }
        const u32 header = cur[0];
        const u32 type = HeaderType(header);
        if (header == 0 || type == 2) {
            cur = cur.subspan(1);
            continue;
        }
        if (type != 3) {
            DC_WARN("gpu: CE: unsupported packet type %u", type);
            ce.stack.clear();
            return;
        }
        const u32 n = HeaderBodyDwords(header);
        if (n + 1 > cur.size()) {
            ce.stack.pop_back();
            continue;
        }
        const u32* body = cur.data() + 1;
        switch (HeaderOpcode(header)) {
        case Op::WriteConstRam: {
            const u32 off = (body[0] & 0xFFFF) / 4;
            for (u32 k = 0; k + 1 < n && off + k < ce.ram.size(); ++k) {
                ce.ram[off + k] = body[1 + k];
            }
            break;
        }
        case Op::LoadConstRam: {
            const u32* src = Ptr<const u32>(body[0], body[1]);
            const u32 count = body[2] & 0x7FFF;
            const u32 off = (body[3] & 0xFFFF) / 4;
            if (src && off + count <= ce.ram.size()) {
                std::memcpy(ce.ram.data() + off, src, count * 4);
            }
            break;
        }
        case Op::DumpConstRam: {
            const u32 off = (body[0] & 0xFFFF) / 4;
            const u32 count = body[1] & 0x7FFF;
            u32* dst = Ptr<u32>(body[2], body[3]);
            if (!dst || off + count > ce.ram.size()) {
                break;
            }
            // The renderer may still have to read what an earlier dump put at
            // this address: let it finish before the ring slot is reused.
            const u64 addr = reinterpret_cast<u64>(dst);
            if (std::find(ce.dumped.begin(), ce.dumped.end(), addr) != ce.dumped.end()) {
                SyncRenderer();
            }
            ce.dumped.push_back(addr);
            std::memcpy(dst, ce.ram.data() + off, count * 4);
            break;
        }
        case Op::IncrementCeCounter:
            ++ce.ce_counter;
            break;
        case Op::WaitOnDeCounterDiff:
            if (ce.ce_counter - ce.de_counter >= body[0]) {
                return; // blocked until the DE catches up; retried later
            }
            break;
        case Op::IndirectBufferConst: {
            const u32* ib = Ptr<const u32>(body[0], body[1] & 0xFFFF);
            const u32 size = body[2] & 0xFFFFF;
            cur = cur.subspan(n + 1);
            if (ib && size) {
                ce.stack.push_back({ib, size});
            }
            continue;
        }
        default:
            break;
        }
        cur = cur.subspan(n + 1);
    }
}

void CommandProcessor::Draw(bool indexed, u32 count, u32 index_offset, u32 first_vertex, u32 instances) {
    ++stats.draws;
    if (!renderer || count == 0) {
        return;
    }
    DrawCall dc;
    dc.indexed = indexed;
    dc.count = count;
    dc.instances = instances ? instances : num_instances;
    dc.index_base = index_base;
    dc.index_offset = index_offset;
    dc.index_size = index_size;
    dc.first_vertex = first_vertex;
    renderer->Draw(CurRegs(), dc);
}

// Indirect draw/dispatch arguments live at the SET_BASE address + offset.
const u32* CommandProcessor::IndirectArgs(u32 offset, u32 dwords) {
    const Ring* ring = t_ring;
    const u64 addr = (ring ? ring->indirect_base : 0) + offset;
    if (!addr) {
        return nullptr;
    }
    (void)dwords;
    return reinterpret_cast<const u32*>(addr);
}

void CommandProcessor::Interrupt(u32 irq) {
    if (on_irq) {
        on_irq(irq);
    }
}

void CommandProcessor::Execute(std::span<const u32> cmds, bool compute, int depth) {
    RegFile& regs = CurRegs();
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
                if (base_reg + i < RegFile::kSize) {
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
            // The Gnm driver hides flip requests in NOPs: "when this frame is
            // shown, write `value` to `label` and/or raise the EOP interrupt".
            if (body_n >= 4 && (body[0] == kNopPrepareFlipLabel || body[0] == kNopPrepareFlipInterruptLabel)) {
                flip_label = {u64(body[1] & ~3u) | (u64(body[2]) << 32), body[3], body[0] == kNopPrepareFlipInterruptLabel};
            } else if (body_n >= 1 && body[0] == kNopPrepareFlipInterrupt) {
                flip_label = {0, 0, true};
            }
            break;
        case Op::ClearState:
            ClearState();
            break;

        case Op::SetContextReg:
            SetRegs(kContextRegBase + body[0], body + 1, body_n - 1);
            break;
        case Op::SetShReg:
            SetRegs(kShRegBase + body[0], body + 1, body_n - 1);
            break;
        case Op::SetUconfigReg:
            SetRegs(kUconfigRegBase + body[0], body + 1, body_n - 1);
            break;
        case Op::SetConfigReg:
            SetRegs(kConfigRegBase + body[0], body + 1, body_n - 1);
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
            SyncRenderer();
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
            SyncRenderer();
            const u32 dw2 = body[1];
            WriteFence((dw2 >> 29) & 7, body[2], body[3], body[4], body[5]);
            const u32 int_sel = (dw2 >> 24) & 7;
            if (int_sel == kIntWriteConfirm || int_sel == kIntUndocumented) {
                Interrupt(compute ? 0 : kIrqGfxEop);
            }
            break;
        }
        case Op::EventWriteEos: {
            SyncRenderer();
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

        case Op::DmaData: {
            // CP DMA: copies/fills between memory, GDS and immediate data.
            const u32 ctl = body[0];
            const u32 cmd = body[5];
            const u32 bytes = cmd & 0x1FFFFF;
            const u32 src_sel = (ctl >> 29) & 3; // 0/3 memory, 1 GDS, 2 data
            const u32 dst_sel = (ctl >> 20) & 3; // 0/3 memory, 1 GDS
            if ((cmd >> 27) & 1) {
                break; // destination is register space: cache prefetch hints
            }
            u8* gds = renderer ? renderer->Gds() : nullptr;
            u8* dst = dst_sel == 1 ? (gds ? gds + (body[3] & 0xFFFF) : nullptr) : Ptr<u8>(body[3], body[4]);
            if (!dst || bytes == 0) {
                break;
            }
            SyncRenderer(); // the GPU may still be producing what we copy
            if (src_sel == 2) {
                for (u32 k = 0; k + 4 <= bytes; k += 4) {
                    std::memcpy(dst + k, &body[1], 4);
                }
            } else {
                const u8* src = src_sel == 1 ? (gds ? gds + (body[1] & 0xFFFF) : nullptr) : Ptr<const u8>(body[1], body[2]);
                if (src) {
                    std::memmove(dst, src, bytes);
                }
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

        case Op::WaitOnCeCounter:
            if (!compute) {
                RunConstantEngine([&] { return ce.ce_counter > ce.de_counter; });
            }
            break;
        case Op::IncrementDeCounter:
            ++ce.de_counter;
            break;

        case Op::IndexType:
            index_size = (body[0] & 3) == 1 ? 4 : 2;
            break;
        case Op::IndexBase:
            index_base = u64(body[0] & ~1u) | (u64(body[1] & 0xFFFF) << 32);
            break;
        case Op::NumInstances:
            num_instances = body[0] ? body[0] : 1;
            break;

        case Op::DrawIndex2:
            index_base = u64(body[1]) | (u64(body[2] & 0xFFFF) << 32);
            Draw(true, body[3], 0, 0, 0);
            break;
        case Op::DrawIndexOffset2:
            Draw(true, body[2], body[1], 0, 0);
            break;
        case Op::DrawIndexAuto:
            Draw(false, body[0], 0, 0, 0);
            break;
        case Op::SetBase:
            if (t_ring) {
                t_ring->indirect_base = u64(body[1] & ~7u) | (u64(body[2] & 0xFFFF) << 32);
            }
            break;
        case Op::DrawIndirect:
        case Op::DrawIndexIndirect: {
            // DrawIndirect args: vertex count, instances, first vertex, first instance.
            // DrawIndexIndirect: index count, instances, first index, base vertex, first instance.
            const bool indexed = op == Op::DrawIndexIndirect;
            const u32* a = IndirectArgs(body[0], indexed ? 5 : 4);
            if (!a) {
                break;
            }
            const u32 base_vtx_loc = body[1] & 0xFFFF;
            const u32 start_inst_loc = body[2] & 0xFFFF;
            if (base_vtx_loc) {
                regs[kShRegBase + base_vtx_loc] = indexed ? a[3] : a[2];
            }
            if (start_inst_loc) {
                regs[kShRegBase + start_inst_loc] = indexed ? a[4] : a[3];
            }
            if (indexed) {
                Draw(true, a[0], a[2], 0, a[1]);
            } else {
                Draw(false, a[0], 0, a[2], a[1]);
            }
            break;
        }

        case Op::DispatchDirect:
            ++stats.dispatches;
            if (renderer) {
                renderer->Dispatch(regs, body[0], body[1], body[2]);
            }
            break;
        case Op::DispatchIndirect: {
            ++stats.dispatches;
            const u32* a = IndirectArgs(body[0], 3);
            if (renderer && a) {
                renderer->Dispatch(regs, a[0], a[1], a[2]);
            }
            break;
        }

        default:
            if (trace) {
                static std::atomic<u64> seen[4]{};
                const u64 bit = 1ull << (op & 63);
                if (!(seen[op >> 6].fetch_or(bit) & bit)) {
                    DC_INFO("gpu: CP%s: unhandled PM4 opcode %#x (%u dwords)", compute ? " compute" : "", op, body_n);
                }
            }
            break;
        }
        if (trace && (op == Op::EventWriteEop || op == Op::ReleaseMem || op == Op::EventWriteEos ||
                      op == Op::WriteData || op == Op::DmaData || op == Op::WaitRegMem)) {
            DC_INFO("gpu: CP%s %#x: %08x %08x %08x %08x %08x %08x", compute ? " compute" : "", op, body[0],
                    body_n > 1 ? body[1] : 0, body_n > 2 ? body[2] : 0, body_n > 3 ? body[3] : 0,
                    body_n > 4 ? body[4] : 0, body_n > 5 ? body[5] : 0);
        }
        p += body_n + 1;
    }
}

CommandProcessor& Instance() {
    static CommandProcessor cp;
    return cp;
}

} // namespace Gpu::Liverpool
