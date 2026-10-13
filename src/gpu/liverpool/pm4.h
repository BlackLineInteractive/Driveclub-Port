#pragma once

// PM4 packets of the PS4 GPU ("Liverpool", AMD GCN 1.1 / Sea Islands).
// Layouts follow AMD's CIK packet documentation; values cross-checked with
// shadPS4 (GPL-2.0-or-later).

#include <cstring>

#include "common/types.h"

namespace Gpu::Liverpool {

namespace Op {
constexpr u32 Nop = 0x10;
constexpr u32 ClearState = 0x12;
constexpr u32 IndexBufferSize = 0x13;
constexpr u32 DispatchDirect = 0x15;
constexpr u32 DispatchIndirect = 0x16;
constexpr u32 DrawIndirect = 0x24;
constexpr u32 DrawIndexIndirect = 0x25;
constexpr u32 IndexBase = 0x26;
constexpr u32 DrawIndex2 = 0x27;
constexpr u32 ContextControl = 0x28;
constexpr u32 IndexType = 0x2A;
constexpr u32 DrawIndexAuto = 0x2D;
constexpr u32 NumInstances = 0x2F;
constexpr u32 IndirectBufferConst = 0x33;
constexpr u32 DrawIndexOffset2 = 0x35;
constexpr u32 WriteData = 0x37;
constexpr u32 MemSemaphore = 0x39;
constexpr u32 WaitRegMem = 0x3C;
constexpr u32 IndirectBuffer = 0x3F;
constexpr u32 EventWrite = 0x46;
constexpr u32 EventWriteEop = 0x47;
constexpr u32 EventWriteEos = 0x48;
constexpr u32 ReleaseMem = 0x49;
constexpr u32 DmaData = 0x50;
constexpr u32 AcquireMem = 0x58;
constexpr u32 SetConfigReg = 0x68;
constexpr u32 SetContextReg = 0x69;
constexpr u32 SetShReg = 0x76;
constexpr u32 SetUconfigReg = 0x79;
constexpr u32 LoadConstRam = 0x80;
constexpr u32 WriteConstRam = 0x81;
constexpr u32 DumpConstRam = 0x83;
constexpr u32 IncrementCeCounter = 0x84;
constexpr u32 IncrementDeCounter = 0x85;
constexpr u32 WaitOnCeCounter = 0x86;
constexpr u32 WaitOnDeCounterDiff = 0x88;
} // namespace Op

// Register file windows (dword indices); SET_*_REG offsets are relative.
constexpr u32 kConfigRegBase = 0x2000;
constexpr u32 kShRegBase = 0x2C00;
constexpr u32 kContextRegBase = 0xA000;
constexpr u32 kUconfigRegBase = 0xC000;
constexpr u32 kNumRegs = 0x10000;

// Type-3 header: [31:30]=3, [29:16]=body dwords - 1, [15:8]=opcode,
// [1]=shader type (1 = compute), [0]=predicate.
constexpr u32 Type3(u32 opcode, u32 body_dwords, bool compute = false) {
    return (3u << 30) | ((body_dwords - 1) << 16) | (opcode << 8) | (compute ? 2u : 0u);
}

inline u32 HeaderType(u32 h) { return h >> 30; }
inline u32 HeaderOpcode(u32 h) { return (h >> 8) & 0xFF; }
inline u32 HeaderBodyDwords(u32 h) { return ((h >> 16) & 0x3FFF) + 1; }

// ------------------------------------------------- packet writers (Gnm) ----

template <typename... V>
u32* WriteShReg(u32* p, u32 reg, V... values) {
    constexpr u32 n = sizeof...(V);
    *p++ = Type3(Op::SetShReg, n + 1);
    *p++ = reg;
    ((*p++ = values), ...);
    return p;
}

template <typename... V>
u32* WriteShRegCompute(u32* p, u32 reg, V... values) {
    constexpr u32 n = sizeof...(V);
    *p++ = Type3(Op::SetShReg, n + 1, true);
    *p++ = reg;
    ((*p++ = values), ...);
    return p;
}

template <typename... V>
u32* WriteContextReg(u32* p, u32 reg, V... values) {
    constexpr u32 n = sizeof...(V);
    *p++ = Type3(Op::SetContextReg, n + 1);
    *p++ = reg;
    ((*p++ = values), ...);
    return p;
}

// NOP filling `body` dwords after the header (total body + 1 dwords).
inline u32* WriteNop(u32* p, u32 body) {
    p[0] = Type3(Op::Nop, body);
    p[1] = 0;
    return p + body + 1;
}

} // namespace Gpu::Liverpool
