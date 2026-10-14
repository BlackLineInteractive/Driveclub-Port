// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle/gnmdriver/gnmdriver.h"

#include <array>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "gpu/liverpool/command_processor.h"
#include "gpu/liverpool/pm4.h"
#include "gpu/renderer.h"
#include "hle/hle.h"
#include "hle/kernel/equeue.h"
#include "hle/video_out/video_out.h"

namespace Hle::GnmDriver {

namespace {

using namespace Gpu::Liverpool;

constexpr u32 kHwInitPacketSize = 0x100u; // 256 dwords

// Context reset sequence (12 dwords)
constexpr std::array<u32, 12> kClearStateSequence{
    0xc0012800u, 0x80000000u, 0x80000000u, 0xc0001200u, 0u, 0xc0055800u,
    0x2ec47fc0u, 0xffffffffu, 0u,          0u,          0u, 10u,
};

// Initial hardware state sequence (115 dwords)
constexpr std::array<u32, 115> kInitSequence{
    0xc0017600u, 0x216u, 0xffffffffu,
    0xc0017600u, 0x217u, 0xffffffffu,
    0xc0017600u, 0x215u, 0u,
    0xc0016900u, 0x2f9u, 0x2du,
    0xc0016900u, 0x282u, 8u,
    0xc0016900u, 0x280u, 0x80008u,
    0xc0016900u, 0x281u, 0xffff0000u,
    0xc0016900u, 0x204u, 0u,
    0xc0016900u, 0x206u, 0x43fu,
    0xc0016900u, 0x83u,  0xffffu,
    0xc0016900u, 0x317u, 0x10u,
    0xc0016900u, 0x2fau, 0x3f800000u,
    0xc0016900u, 0x2fcu, 0x3f800000u,
    0xc0016900u, 0x2fbu, 0x3f800000u,
    0xc0016900u, 0x2fdu, 0x3f800000u,
    0xc0016900u, 0x202u, 0xcc0010u,
    0xc0016900u, 0x30eu, 0xffffffffu,
    0xc0016900u, 0x30fu, 0xffffffffu,
    0xc0002f00u, 1u,
    0xc0017600u, 7u,     0x1ffu,
    0xc0017600u, 0x46u,  0x1ffu,
    0xc0017600u, 0x87u,  0x1ffu,
    0xc0017600u, 0xc7u,  0x1ffu,
    0xc0017600u, 0x107u, 0u,
    0xc0017600u, 0x147u, 0x1ffu,
    0xc0016900u, 0x1b1u, 2u,
    0xc0016900u, 0x101u, 0u,
    0xc0016900u, 0x100u, 0xffffffffu,
    0xc0016900u, 0x103u, 0u,
    0xc0016900u, 0x284u, 0u,
    0xc0016900u, 0x290u, 0u,
    0xc0016900u, 0x2aeu, 0u,
    0xc0016900u, 0x292u, 0u,
    0xc0016900u, 0x293u, 0x6000000u,
    0xc0016900u, 0x2f8u, 0u,
    0xc0016900u, 0x2deu, 0x1e9u,
    0xc0036900u, 0x295u, 0x100u, 0x100u, 4u,
    0xc0017900u, 0x200u, 0xe0000000u,
};

// ------------------------------------------------------------- Compute queues ----
struct ComputeQueue {
    u32 pipe_id;
    u32 queue_id;
    const u32* ring_base;
    u32 ring_size_dw;
    u32* read_ptr;
    u32 last_offs_dw = 0;
};

std::mutex g_queue_lock;
std::unordered_map<u32, ComputeQueue> g_compute_queues;
u32 g_next_vqid = 1;

// ---------------------------------------------------------------- Gnm Events ----
std::mutex g_event_lock;
std::unordered_map<u64, std::vector<Kernel::EventQueue*>> g_gnm_event_listeners;

void TriggerGnmEvent(u32 irq) {
    std::vector<Kernel::EventQueue*> targets;
    {
        std::scoped_lock lk(g_event_lock);
        auto it = g_gnm_event_listeners.find(irq);
        if (it != g_gnm_event_listeners.end()) {
            targets = it->second;
        }
    }
    for (auto* eq : targets) {
        eq->Trigger(irq, Kernel::kEvfiltGraphicsCore, irq);
    }
}

// ----------------------------------------------------- PM4 hardware init ----
u32 sceGnmDrawInitDefaultHardwareState(u32* cmdbuf, u32 size) {
    if (!cmdbuf || size < kHwInitPacketSize) {
        return 0;
    }
    u32* p = cmdbuf;
    std::memcpy(p, kClearStateSequence.data(), kClearStateSequence.size() * 4);
    p += kClearStateSequence.size();

    std::memcpy(p, kInitSequence.data(), kInitSequence.size() * 4);
    p += kInitSequence.size();

    const u32 left = (cmdbuf + kHwInitPacketSize) - p;
    if (left > 1) {
        WriteNop(p, left - 1); // header + body fill the block exactly
    }
    return kHwInitPacketSize;
}

u32 sceGnmDispatchInitDefaultHardwareState(u32* cmdbuf, u32 size) {
    if (!cmdbuf || size < kHwInitPacketSize) {
        return 0;
    }
    u32* p = cmdbuf;
    p = WriteShRegCompute(p, 0x216u, 0xffffffffu); // COMPUTE_STATIC_THREAD_MGMT_SE0
    p = WriteShRegCompute(p, 0x217u, 0xffffffffu); // COMPUTE_STATIC_THREAD_MGMT_SE1
    p = WriteShRegCompute(p, 0x215u, 0x170u);        // COMPUTE_RESOURCE_LIMITS

    const u32 left = (cmdbuf + kHwInitPacketSize) - p;
    if (left > 1) {
        WriteNop(p, left - 1); // header + body fill the block exactly
    }
    return kHwInitPacketSize;
}

// ---------------------------------------------------------- Shader Setters ----
s32 sceGnmSetVsShader(u32* cmdbuf, u32 size, const u32* vs_regs, u32 modifier) {
    if (!cmdbuf || size <= 0x1c || !vs_regs) {
        return -1;
    }
    const u32 rsrc1 = modifier == 0 ? vs_regs[2] : (vs_regs[2] & 0xfcfffc3f) | modifier;
    cmdbuf = WriteShReg(cmdbuf, 0x48u, vs_regs[0], 0u);   // SPI_SHADER_PGM_LO_VS
    cmdbuf = WriteShReg(cmdbuf, 0x4au, rsrc1, vs_regs[3]); // SPI_SHADER_PGM_RSRC1_VS
    cmdbuf = WriteContextReg(cmdbuf, 0x207u, vs_regs[6]); // PA_CL_VS_OUT_CNTL
    cmdbuf = WriteContextReg(cmdbuf, 0x1b1u, vs_regs[4]); // SPI_VS_OUT_CONFIG
    cmdbuf = WriteContextReg(cmdbuf, 0x1c3u, vs_regs[5]); // SPI_SHADER_POS_FORMAT
    WriteNop(cmdbuf, 11);
    return 0;
}

s32 sceGnmSetPsShader(u32* cmdbuf, u32 size, const u32* ps_regs) {
    if (!cmdbuf || size <= 0x27) {
        return -1;
    }
    if (!ps_regs) {
        cmdbuf = WriteShReg(cmdbuf, 8u, 0u, 0u);
        cmdbuf = WriteContextReg(cmdbuf, 0x203u, 0u);
        WriteNop(cmdbuf, 0x20);
    } else {
        cmdbuf = WriteShReg(cmdbuf, 8u, ps_regs[0], 0u);
        cmdbuf = WriteShReg(cmdbuf, 10u, ps_regs[2], ps_regs[3]);
        cmdbuf = WriteContextReg(cmdbuf, 0x1c4u, ps_regs[4], ps_regs[5]);
        cmdbuf = WriteContextReg(cmdbuf, 0x1b3u, ps_regs[6], ps_regs[7]);
        cmdbuf = WriteContextReg(cmdbuf, 0x1b6u, ps_regs[8]);
        cmdbuf = WriteContextReg(cmdbuf, 0x1b8u, ps_regs[9]);
        cmdbuf = WriteContextReg(cmdbuf, 0x203u, ps_regs[10]);
        cmdbuf = WriteContextReg(cmdbuf, 0x8fu, ps_regs[11]);
        WriteNop(cmdbuf, 11);
    }
    return 0;
}

s32 sceGnmSetCsShader(u32* cmdbuf, u32 size, const u32* cs_regs) {
    if (!cmdbuf || size <= 0x18 || !cs_regs) {
        return -1;
    }
    cmdbuf = WriteShReg(cmdbuf, 0x20cu, cs_regs[0], 0u);
    cmdbuf = WriteShReg(cmdbuf, 0x212u, cs_regs[2], cs_regs[3]);
    cmdbuf = WriteShReg(cmdbuf, 0x207u, cs_regs[4], cs_regs[5], cs_regs[6]);
    WriteNop(cmdbuf, 11);
    return 0;
}

s32 sceGnmSetEsShader(u32* cmdbuf, u32 size, const u32* es_regs, u32 modifier) {
    if (!cmdbuf || size < 0x14 || !es_regs) {
        return -1;
    }
    const u32 rsrc1 = modifier == 0 ? es_regs[2] : (es_regs[2] & 0xfcfffc3f) | modifier;
    cmdbuf = WriteShReg(cmdbuf, 0xc8u, es_regs[0], 0u);
    cmdbuf = WriteShReg(cmdbuf, 0xcau, rsrc1, es_regs[3]);
    WriteNop(cmdbuf, 11);
    return 0;
}

s32 sceGnmSetGsShader(u32* cmdbuf, u32 size, const u32* gs_regs) {
    if (!cmdbuf || size < 0x1d || !gs_regs) {
        return -1;
    }
    cmdbuf = WriteShReg(cmdbuf, 0x88u, gs_regs[0], 0u);
    cmdbuf = WriteShReg(cmdbuf, 0x8au, gs_regs[2], gs_regs[3]);
    cmdbuf = WriteContextReg(cmdbuf, 0x2e5u, gs_regs[4]);
    cmdbuf = WriteContextReg(cmdbuf, 0x29bu, gs_regs[5]);
    cmdbuf = WriteContextReg(cmdbuf, 0x2e4u, gs_regs[6]);
    WriteNop(cmdbuf, 11);
    return 0;
}

s32 sceGnmSetHsShader(u32* cmdbuf, u32 size, const u32* hs_regs, u32 param4) {
    if (!cmdbuf || size < 0x1e || !hs_regs) {
        return -1;
    }
    cmdbuf = WriteShReg(cmdbuf, 0x108u, hs_regs[0], 0u);
    cmdbuf = WriteShReg(cmdbuf, 0x10au, hs_regs[2], hs_regs[3]);
    cmdbuf = WriteContextReg(cmdbuf, 0x286u, hs_regs[5], hs_regs[6]);
    cmdbuf = WriteContextReg(cmdbuf, 0x2dbu, hs_regs[4]);
    cmdbuf = WriteContextReg(cmdbuf, 0x2d6u, param4);
    WriteNop(cmdbuf, 11);
    return 0;
}

s32 sceGnmSetLsShader(u32* cmdbuf, u32 size, const u32* ls_regs, u32 modifier) {
    if (!cmdbuf || size < 0x17 || !ls_regs) {
        return -1;
    }
    const u32 rsrc1 = modifier == 0 ? ls_regs[2] : (ls_regs[2] & 0xfcfffc3f) | modifier;
    cmdbuf = WriteShReg(cmdbuf, 0x148u, ls_regs[0], 0u);
    cmdbuf = WriteShReg(cmdbuf, 0x14bu, ls_regs[3]);
    cmdbuf = WriteShReg(cmdbuf, 0x14au, rsrc1, ls_regs[3]);
    WriteNop(cmdbuf, 11);
    return 0;
}

s32 sceGnmSetGsRingSizes(u32 esgs_ring_size, u32 gsvs_ring_size) {
    (void)esgs_ring_size;
    (void)gsvs_ring_size;
    return 0;
}

// ------------------------------------------------------------- Sync & Markers ----
s32 sceGnmInsertWaitFlipDone(u32* cmdbuf, u32 size, s32 vo_handle, u32 buf_idx) {
    if (!cmdbuf || size != 7) {
        return -1;
    }
    uintptr_t label_addr = VideoOut::Driver::Instance().GetBufferLabelAddress(vo_handle);
    if (!label_addr) {
        return -1;
    }

    // WAIT_REG_MEM: 6 body dwords, filling the 7 the caller reserved
    cmdbuf[0] = Type3(Op::WaitRegMem, 6);
    cmdbuf[1] = 0x13u; // function: equal, memory poll
    const u64 poll_addr = (label_addr + buf_idx * sizeof(u64)) & ~3ULL;
    cmdbuf[2] = static_cast<u32>(poll_addr);
    cmdbuf[3] = static_cast<u32>(poll_addr >> 32);
    cmdbuf[4] = 0;           // ref: wait until label == 0
    cmdbuf[5] = 0xFFFFFFFFu; // mask
    cmdbuf[6] = 10u;         // poll interval
    return 0;
}

s32 sceGnmInsertSetMarker(u32* cmdbuf, u32 size, const char* marker) {
    if (cmdbuf && marker && size >= 3) {
        cmdbuf[0] = Type3(Op::Nop, size - 1);
        cmdbuf[1] = 0;
        return 0;
    }
    return -1;
}

s32 sceGnmInsertSetColorMarker(u32* cmdbuf, u32 size, const char* marker, u32 color) {
    (void)marker;
    (void)color;
    if (cmdbuf && size >= 3) {
        cmdbuf[0] = Type3(Op::Nop, size - 1);
        cmdbuf[1] = 0;
        return 0;
    }
    return -1;
}

s32 sceGnmInsertPushMarker(u32* cmdbuf, u32 size, const char* marker) {
    if (cmdbuf && marker && size >= 3) {
        cmdbuf[0] = Type3(Op::Nop, size - 1);
        cmdbuf[1] = 0;
        return 0;
    }
    return -1;
}

s32 sceGnmInsertPushColorMarker(u32* cmdbuf, u32 size, const char* marker, u32 color) {
    (void)marker;
    (void)color;
    if (cmdbuf && size >= 3) {
        cmdbuf[0] = Type3(Op::Nop, size - 1);
        cmdbuf[1] = 0;
        return 0;
    }
    return -1;
}

s32 sceGnmInsertPopMarker(u32* cmdbuf, u32 size) {
    if (cmdbuf && size >= 2) {
        cmdbuf[0] = Type3(Op::Nop, size - 1);
        cmdbuf[1] = 0;
        return 0;
    }
    return -1;
}

// ---------------------------------------------------------- Submit & Execution ----
// Pairs each draw command buffer with its constant command buffer.
std::vector<Gpu::Liverpool::CommandProcessor::CmdBuffers> CollectBuffers(u32 count, const u32* dcb_ptrs[],
                                                                       const u32 dcb_sizes[],
                                                                       const u32* ccb_ptrs[],
                                                                       const u32 ccb_sizes[]) {
    std::vector<Gpu::Liverpool::CommandProcessor::CmdBuffers> bufs;
    bufs.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        Gpu::Liverpool::CommandProcessor::CmdBuffers b;
        if (dcb_ptrs && dcb_sizes && dcb_ptrs[i] && dcb_sizes[i] > 0) {
            b.dcb = {reinterpret_cast<const u32*>(dcb_ptrs[i]), dcb_sizes[i] / 4};
        }
        if (ccb_ptrs && ccb_sizes && ccb_ptrs[i] && ccb_sizes[i] > 0) {
            b.ccb = {reinterpret_cast<const u32*>(ccb_ptrs[i]), ccb_sizes[i] / 4};
        }
        if (!b.dcb.empty() || !b.ccb.empty()) {
            bufs.push_back(b);
        }
    }
    return bufs;
}

s32 sceGnmSubmitCommandBuffers(u32 count, const u32* dcb_ptrs[], const u32 dcb_sizes[],
                               const u32* ccb_ptrs[], const u32 ccb_sizes[]) {
    static u64 submit_count = 0;
    if (++submit_count % 60 == 1) {
        DC_INFO("gnm: submit #%llu (count=%u)", submit_count, count);
    }
    Instance().SubmitGraphics(CollectBuffers(count, dcb_ptrs, dcb_sizes, ccb_ptrs, ccb_sizes));
    return 0;
}

// Runs after the frame's command buffers: show the buffer, then tell the game.
void FlipOnGpuThread(u32 vo_handle, u32 buf_idx, s64 flip_arg) {
    auto& vo = VideoOut::Driver::Instance();
    vo.WaitFlipVblank(static_cast<s32>(vo_handle));
    if (auto* r = Instance().GetRenderer()) {
        const Gpu::FlipInfo flip = vo.GetFlipInfo(static_cast<s32>(vo_handle), buf_idx);
        if (flip.address) {
            r->Flip(flip);
        }
    }
    vo.OnFlipDone(static_cast<s32>(vo_handle), buf_idx, flip_arg);
}

s32 sceGnmSubmitAndFlipCommandBuffers(u32 count, const u32* dcb_ptrs[], const u32 dcb_sizes[],
                                      const u32* ccb_ptrs[], const u32 ccb_sizes[],
                                      u32 vo_handle, u32 buf_idx, u32 flip_mode, s64 flip_arg) {
    (void)flip_mode;
    static u64 flip_count = 0;
    if (++flip_count % 60 == 1) {
        DC_INFO("gnm: submit and flip #%llu (count=%u, vo=%u, buf=%u, arg=%lld)",
                flip_count, count, vo_handle, buf_idx, flip_arg);
    }
    VideoOut::Driver::Instance().MarkFlipPending(vo_handle, buf_idx);

    Instance().SubmitGraphics(CollectBuffers(count, dcb_ptrs, dcb_sizes, ccb_ptrs, ccb_sizes), [vo_handle, buf_idx, flip_arg] {
        FlipOnGpuThread(vo_handle, buf_idx, flip_arg);
    });
    return 0;
}

s32 sceGnmSubmitDone() {
    Instance().WaitIdle();
    return 0;
}

int sceGnmAreSubmitsAllowed() {
    return 1;
}

s32 sceGnmValidateCommandBuffers() {
    return 0;
}

void sceGnmFlushGarlic() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

s32 sceGnmRequestFlipAndSubmitDone(u32 vo_handle, u32 buf_idx, s64 flip_arg) {
    Instance().SubmitGraphics({}, [vo_handle, buf_idx, flip_arg] {
        FlipOnGpuThread(vo_handle, buf_idx, flip_arg);
    });
    return sceGnmSubmitDone();
}

int sceGnmMapComputeQueue(u32 pipe_id, u32 queue_id, const u32* ring_base, u32 ring_size_dw,
                          u32* read_ptr) {
    std::scoped_lock lk(g_queue_lock);
    const u32 vqid = g_next_vqid++;
    g_compute_queues[vqid] = {pipe_id, queue_id, ring_base, ring_size_dw, read_ptr, 0};
    DC_INFO("gnm: mapped compute queue vqid %u (pipe %u, queue %u, size %u dw)", vqid, pipe_id,
            queue_id, ring_size_dw);
    return vqid;
}

int sceGnmUnmapComputeQueue(u32 vqid) {
    std::scoped_lock lk(g_queue_lock);
    g_compute_queues.erase(vqid);
    return 0;
}

void sceGnmDingDong(u32 gnm_vqid, u32 next_offs_dw) {
    std::scoped_lock lk(g_queue_lock);
    auto it = g_compute_queues.find(gnm_vqid);
    if (it == g_compute_queues.end()) {
        return;
    }
    ComputeQueue& q = it->second;
    if (next_offs_dw == q.last_offs_dw) {
        return;
    }
    if (next_offs_dw > q.last_offs_dw) {
        Instance().SubmitCompute(gnm_vqid, {q.ring_base + q.last_offs_dw, next_offs_dw - q.last_offs_dw});
    } else {
        // Wrapped around ring buffer
        Instance().SubmitCompute(gnm_vqid, {q.ring_base + q.last_offs_dw, q.ring_size_dw - q.last_offs_dw});
        Instance().SubmitCompute(gnm_vqid, {q.ring_base, next_offs_dw});
    }
    q.last_offs_dw = next_offs_dw;
    if (q.read_ptr) {
        *q.read_ptr = next_offs_dw;
    }
}

// ------------------------------------------------------------- Event Queues ----
s32 sceGnmAddEqEvent(Kernel::EventQueue* eq, u64 id, void* udata) {
    if (!eq) {
        return -1;
    }
    eq->Add(id, Kernel::kEvfiltGraphicsCore, udata);
    {
        std::scoped_lock lk(g_event_lock);
        g_gnm_event_listeners[id].push_back(eq);
    }
    return 0;
}

s32 sceGnmDeleteEqEvent(Kernel::EventQueue* eq, u64 id) {
    if (!eq) {
        return -1;
    }
    eq->Remove(id, Kernel::kEvfiltGraphicsCore);
    {
        std::scoped_lock lk(g_event_lock);
        auto it = g_gnm_event_listeners.find(id);
        if (it != g_gnm_event_listeners.end()) {
            std::erase(it->second, eq);
        }
    }
    return 0;
}

s32 sceGnmGetEqEventType(const Kernel::GuestKevent* ev) {
    return ev ? static_cast<s32>(ev->data) : 0;
}

s32 sceGnmGetEqTimeStamp() {
    return 0;
}

// -------------------------------------------------------- Hardware Topology ----
int sceGnmLogicalCuIndexToPhysicalCuIndex(int cu) {
    return cu;
}

s32 sceGnmLogicalCuMaskToPhysicalCuMask(s64, s32 logical_cu_mask) {
    return logical_cu_mask;
}

int sceGnmDebugHardwareStatus() {
    return 0;
}

int sceGnmIsUserPaEnabled() {
    return 0;
}

u64 sceGnmGetTheTessellationFactorRingBufferBaseAddress() {
    alignas(0x1000) static u8 s_tess_ring[0x800000];
    return reinterpret_cast<u64>(s_tess_ring);
}

// ------------------------------------------------------------- Stubs & Diagnostics ----
int sceRazorIsLoaded() {
    return 0;
}

int sceRazorCaptureImmediate() {
    return 0;
}

int sceRazorCaptureSinceLastFlip() {
    return 0;
}

int sceRazorCaptureCommandBuffersOnlyImmediate() {
    return 0;
}

int sceRazorCaptureCommandBuffersOnlySinceLastFlip() {
    return 0;
}

int sceGnmDriverTriggerCapture() {
    return 0;
}

bool sceGnmDriverCaptureInProgress() {
    return false;
}

int sceGnmSetupMipStatsReport() {
    return 0;
}

int sceGnmRequestMipStatsReportAndReset() {
    return 0;
}

int sceGnmDisableMipStatsReport() {
    return 0;
}

} // namespace

void RegisterGnmDriver() {
    Instance().SetInterruptHandler([](u32 irq) { TriggerGnmEvent(irq); });

    Hle::Register("libSceGnmDriver", "Idffwf3yh8s", reinterpret_cast<void*>(sceGnmDrawInitDefaultHardwareState));
    Hle::Register("libSceGnmDriver", "nF6bFRUBRAU", reinterpret_cast<void*>(sceGnmDispatchInitDefaultHardwareState));

    Hle::Register("libSceGnmDriver", "bQVd5YzCal0", reinterpret_cast<void*>(sceGnmSetPsShader));
    Hle::Register("libSceGnmDriver", "gAhCn6UiU4Y", reinterpret_cast<void*>(sceGnmSetVsShader));
    Hle::Register("libSceGnmDriver", "KXltnCwEJHQ", reinterpret_cast<void*>(sceGnmSetCsShader));
    Hle::Register("libSceGnmDriver", "FUHG8sQ3R58", reinterpret_cast<void*>(sceGnmSetEsShader));
    Hle::Register("libSceGnmDriver", "UJwNuMBcUAk", reinterpret_cast<void*>(sceGnmSetGsShader));
    Hle::Register("libSceGnmDriver", "VJNjFtqiF5w", reinterpret_cast<void*>(sceGnmSetHsShader));
    Hle::Register("libSceGnmDriver", "vckdzbQ46SI", reinterpret_cast<void*>(sceGnmSetLsShader));
    Hle::Register("libSceGnmDriver", "jtkqXpAOY6w", reinterpret_cast<void*>(sceGnmSetGsRingSizes));

    Hle::Register("libSceGnmDriver", "jiItzS6+22g", reinterpret_cast<void*>(sceGnmInsertSetMarker));
    Hle::Register("libSceGnmDriver", "aj3L-iaFmyk", reinterpret_cast<void*>(sceGnmInsertSetColorMarker));
    Hle::Register("libSceGnmDriver", "W1Etj-jlW7Y", reinterpret_cast<void*>(sceGnmInsertPushMarker));
    Hle::Register("libSceGnmDriver", "aPIZJTXC+cU", reinterpret_cast<void*>(sceGnmInsertPushColorMarker));
    Hle::Register("libSceGnmDriver", "7qZVNgEu+SY", reinterpret_cast<void*>(sceGnmInsertPopMarker));

    Hle::Register("libSceGnmDriver", "1qXLHIpROPE", reinterpret_cast<void*>(sceGnmInsertWaitFlipDone));

    Hle::Register("libSceGnmDriver", "zwY0YV91TTI", reinterpret_cast<void*>(sceGnmSubmitCommandBuffers));
    Hle::Register("libSceGnmDriver", "xbxNatawohc", reinterpret_cast<void*>(sceGnmSubmitAndFlipCommandBuffers));
    Hle::Register("libSceGnmDriver", "yvZ73uQUqrk", reinterpret_cast<void*>(sceGnmSubmitDone));
    Hle::Register("libSceGnmDriver", "b08AgtPlHPg", reinterpret_cast<void*>(sceGnmAreSubmitsAllowed));
    Hle::Register("libSceGnmDriver", "iCO804ZgzdA", reinterpret_cast<void*>(sceGnmValidateCommandBuffers));
    Hle::Register("libSceGnmDriver", "iBt3Oe00Kvc", reinterpret_cast<void*>(sceGnmFlushGarlic));
    Hle::Register("libSceGnmDriver", "gObODli-OH8", reinterpret_cast<void*>(sceGnmRequestFlipAndSubmitDone));

    Hle::Register("libSceGnmDriver", "29oKvKXzEZo", reinterpret_cast<void*>(sceGnmMapComputeQueue));
    Hle::Register("libSceGnmDriver", "ArSg-TGinhk", reinterpret_cast<void*>(sceGnmUnmapComputeQueue));
    Hle::Register("libSceGnmDriver", "bX5IbRvECXk", reinterpret_cast<void*>(sceGnmDingDong));

    Hle::Register("libSceGnmDriver", "b0xyllnVY-I", reinterpret_cast<void*>(sceGnmAddEqEvent));
    Hle::Register("libSceGnmDriver", "PVT+fuoS9gU", reinterpret_cast<void*>(sceGnmDeleteEqEvent));
    Hle::Register("libSceGnmDriver", "UoYY0DWMC0U", reinterpret_cast<void*>(sceGnmGetEqEventType));
    Hle::Register("libSceGnmDriver", "H7-fgvEutM0", reinterpret_cast<void*>(sceGnmGetEqTimeStamp));

    Hle::Register("libSceGnmDriver", "26PM5Mzl8zc", reinterpret_cast<void*>(sceGnmLogicalCuIndexToPhysicalCuIndex));
    Hle::Register("libSceGnmDriver", "RU74kek-N0c", reinterpret_cast<void*>(sceGnmLogicalCuMaskToPhysicalCuMask));
    Hle::Register("libSceGnmDriver", "qpGITzPE+Zc", reinterpret_cast<void*>(sceGnmDebugHardwareStatus));
    Hle::Register("libSceGnmDriver", "jg33rEKLfVs", reinterpret_cast<void*>(sceGnmIsUserPaEnabled));
    Hle::Register("libSceGnmDriver", "ln33zjBrfjk", reinterpret_cast<void*>(sceGnmGetTheTessellationFactorRingBufferBaseAddress));

    Hle::Register("libSceGnmDriver", "f33OrruQYbM", reinterpret_cast<void*>(sceRazorIsLoaded));
    Hle::Register("libSceGnmDriver", "u9YKpRRHe-M", reinterpret_cast<void*>(sceRazorCaptureImmediate));
    Hle::Register("libSceGnmDriver", "4UFagYlfuAM", reinterpret_cast<void*>(sceRazorCaptureSinceLastFlip));
    Hle::Register("libSceGnmDriver", "xeTLfxVIQO4", reinterpret_cast<void*>(sceRazorCaptureCommandBuffersOnlyImmediate));
    Hle::Register("libSceGnmDriver", "9thMn+uB1is", reinterpret_cast<void*>(sceRazorCaptureCommandBuffersOnlySinceLastFlip));
    Hle::Register("libSceGnmDriver", "d88anrgNoKY", reinterpret_cast<void*>(sceGnmDriverTriggerCapture));
    Hle::Register("libSceGnmDriver", "TLV4mswiZ4A", reinterpret_cast<void*>(sceGnmDriverCaptureInProgress));
    Hle::Register("libSceGnmDriver", "+xuDhxlWRPg", reinterpret_cast<void*>(sceGnmSetupMipStatsReport));
    Hle::Register("libSceGnmDriver", "f85orjx7qts", reinterpret_cast<void*>(sceGnmRequestMipStatsReportAndReset));
    Hle::Register("libSceGnmDriver", "HHo1BAljZO8", reinterpret_cast<void*>(sceGnmDisableMipStatsReport));
}

} // namespace Hle::GnmDriver
