#pragma once

// Liverpool (GCN 1.1 / Sea Islands) register file.
//
// The command processor keeps every register as a plain dword, indexed by its
// absolute dword offset (the numbers in AMD's CIK register headers). Only the
// registers the renderer reads get a name here; fields are decoded on demand
// by the small helpers below instead of mirroring each register as a struct.

#include <array>

#include "common/types.h"

namespace Gpu::Liverpool {

namespace Reg {

// ---- persistent shader state (SET_SH_REG window, 0x2C00) ----
constexpr u32 SPI_SHADER_PGM_LO_PS = 0x2C08;
constexpr u32 SPI_SHADER_PGM_HI_PS = 0x2C09;
constexpr u32 SPI_SHADER_PGM_RSRC1_PS = 0x2C0A;
constexpr u32 SPI_SHADER_PGM_RSRC2_PS = 0x2C0B;
constexpr u32 SPI_SHADER_USER_DATA_PS_0 = 0x2C0C;
constexpr u32 SPI_SHADER_PGM_LO_VS = 0x2C48;
constexpr u32 SPI_SHADER_PGM_HI_VS = 0x2C49;
constexpr u32 SPI_SHADER_PGM_RSRC1_VS = 0x2C4A;
constexpr u32 SPI_SHADER_PGM_RSRC2_VS = 0x2C4B;
constexpr u32 SPI_SHADER_USER_DATA_VS_0 = 0x2C4C;
constexpr u32 SPI_SHADER_PGM_LO_ES = 0x2CC8;
constexpr u32 SPI_SHADER_PGM_LO_LS = 0x2D48;

constexpr u32 COMPUTE_NUM_THREAD_X = 0x2E07;
constexpr u32 COMPUTE_NUM_THREAD_Y = 0x2E08;
constexpr u32 COMPUTE_NUM_THREAD_Z = 0x2E09;
constexpr u32 COMPUTE_PGM_LO = 0x2E0C;
constexpr u32 COMPUTE_PGM_HI = 0x2E0D;
constexpr u32 COMPUTE_PGM_RSRC1 = 0x2E12;
constexpr u32 COMPUTE_PGM_RSRC2 = 0x2E13;
constexpr u32 COMPUTE_USER_DATA_0 = 0x2E40;

// ---- context state (SET_CONTEXT_REG window, 0xA000) ----
constexpr u32 DB_RENDER_CONTROL = 0xA000;
constexpr u32 DB_DEPTH_VIEW = 0xA002;
constexpr u32 DB_DEPTH_CLEAR = 0xA00B;
constexpr u32 PA_SC_SCREEN_SCISSOR_TL = 0xA00C;
constexpr u32 PA_SC_SCREEN_SCISSOR_BR = 0xA00D;
constexpr u32 DB_Z_INFO = 0xA010;
constexpr u32 DB_STENCIL_INFO = 0xA011;
constexpr u32 DB_Z_READ_BASE = 0xA012;
constexpr u32 DB_STENCIL_READ_BASE = 0xA013;
constexpr u32 DB_Z_WRITE_BASE = 0xA014;
constexpr u32 DB_STENCIL_WRITE_BASE = 0xA015;
constexpr u32 DB_DEPTH_SIZE = 0xA016;
constexpr u32 DB_DEPTH_SLICE = 0xA017;
constexpr u32 PA_SC_WINDOW_OFFSET = 0xA080;
constexpr u32 PA_SC_WINDOW_SCISSOR_TL = 0xA081;
constexpr u32 PA_SC_WINDOW_SCISSOR_BR = 0xA082;
constexpr u32 CB_TARGET_MASK = 0xA08E;
constexpr u32 CB_SHADER_MASK = 0xA08F;
constexpr u32 PA_SC_GENERIC_SCISSOR_TL = 0xA090;
constexpr u32 PA_SC_GENERIC_SCISSOR_BR = 0xA091;
constexpr u32 PA_SC_VPORT_SCISSOR_0_TL = 0xA094; // 16 x {TL, BR}
constexpr u32 PA_SC_VPORT_ZMIN_0 = 0xA0B4;       // 16 x {MIN, MAX}
constexpr u32 VGT_INDX_OFFSET = 0xA102;
constexpr u32 VGT_MULTI_PRIM_IB_RESET_INDX = 0xA103;
constexpr u32 CB_BLEND_RED = 0xA105; // RED, GREEN, BLUE, ALPHA
constexpr u32 DB_STENCIL_CONTROL = 0xA10B;
constexpr u32 DB_STENCILREFMASK = 0xA10C;
constexpr u32 DB_STENCILREFMASK_BF = 0xA10D;
constexpr u32 PA_CL_VPORT_XSCALE = 0xA10F; // 16 x {XSCALE, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET}
constexpr u32 SPI_PS_INPUT_CNTL_0 = 0xA191; // 32 entries
constexpr u32 SPI_VS_OUT_CONFIG = 0xA1B1;
constexpr u32 SPI_PS_INPUT_ENA = 0xA1B3;
constexpr u32 SPI_PS_INPUT_ADDR = 0xA1B4;
constexpr u32 SPI_PS_IN_CONTROL = 0xA1B6;
constexpr u32 SPI_SHADER_POS_FORMAT = 0xA1C3;
constexpr u32 SPI_SHADER_Z_FORMAT = 0xA1C4;
constexpr u32 SPI_SHADER_COL_FORMAT = 0xA1C5;
constexpr u32 CB_BLEND0_CONTROL = 0xA1E0; // 8 entries
constexpr u32 DB_DEPTH_CONTROL = 0xA200;
constexpr u32 CB_COLOR_CONTROL = 0xA202;
constexpr u32 DB_SHADER_CONTROL = 0xA203;
constexpr u32 PA_CL_CLIP_CNTL = 0xA204;
constexpr u32 PA_SU_SC_MODE_CNTL = 0xA205;
constexpr u32 PA_CL_VTE_CNTL = 0xA206;
constexpr u32 PA_CL_VS_OUT_CNTL = 0xA207;
constexpr u32 VGT_PRIMITIVE_RESTART = 0xA2A5; // VGT_MULTI_PRIM_IB_RESET_EN
constexpr u32 CB_COLOR0_BASE = 0xA318;
constexpr u32 kColorTargetStride = 15; // CB_COLORn_* blocks are 15 dwords apart
// Offsets inside one CB_COLORn block.
constexpr u32 CB_BASE = 0, CB_PITCH = 1, CB_SLICE = 2, CB_VIEW = 3, CB_INFO = 4, CB_ATTRIB = 5;

// ---- user config (SET_UCONFIG_REG window, 0xC000) ----
constexpr u32 VGT_PRIMITIVE_TYPE = 0xC242;
constexpr u32 VGT_INDEX_TYPE = 0xC243;
constexpr u32 VGT_NUM_INDICES = 0xC24C;
constexpr u32 VGT_NUM_INSTANCES = 0xC24D;

} // namespace Reg

enum class PrimType : u32 {
    None = 0,
    PointList = 1,
    LineList = 2,
    LineStrip = 3,
    TriList = 4,
    TriFan = 5,
    TriStrip = 6,
    RectList = 0x11,
    LineLoop = 0x12,
    QuadList = 0x13,
    QuadStrip = 0x14,
    Polygon = 0x15,
};

constexpr u32 kNumColorTargets = 8;
constexpr u32 kNumUserData = 16;

constexpr u32 Bits(u32 v, u32 lo, u32 n) {
    return (v >> lo) & ((n >= 32) ? ~0u : ((1u << n) - 1));
}

class RegFile {
public:
    static constexpr u32 kSize = 0x10000;

    u32& operator[](u32 i) { return r[i]; }
    u32 operator[](u32 i) const { return r[i]; }
    u32* Data() { return r.data(); }
    const u32* Data() const { return r.data(); }

    float F(u32 i) const {
        float f;
        __builtin_memcpy(&f, &r[i], 4);
        return f;
    }

    // Shader program addresses are stored >> 8 across a LO/HI pair.
    u64 ProgramAddr(u32 lo_reg) const { return (u64(r[lo_reg]) | (u64(r[lo_reg + 1] & 0xFF) << 32)) << 8; }

    const u32* UserData(u32 user_data_0) const { return &r[user_data_0]; }

    u32 ColorTarget(u32 index, u32 field) const {
        return r[Reg::CB_COLOR0_BASE + index * Reg::kColorTargetStride + field];
    }

    PrimType Primitive() const { return static_cast<PrimType>(r[Reg::VGT_PRIMITIVE_TYPE] & 0x3F); }

private:
    std::array<u32, kSize> r{};
};

// CB_COLORn_INFO fields.
namespace ColorInfo {
constexpr u32 Format(u32 info) { return Bits(info, 2, 5); }      // COLOR_FORMAT
constexpr u32 NumberType(u32 info) { return Bits(info, 8, 3); }  // NUMBER_TYPE
constexpr u32 CompSwap(u32 info) { return Bits(info, 11, 2); }   // COMP_SWAP
constexpr u32 kNumberSrgb = 6; // CB NUMBER_TYPE differs from descriptor NUM_FORMAT here
} // namespace ColorInfo

// CB_COLOR_FORMAT values.
namespace ColorFormat {
constexpr u32 Invalid = 0, C8 = 1, C16 = 2, C8_8 = 3, C32 = 4, C16_16 = 5, C10_11_11 = 6,
              C11_11_10 = 7, C10_10_10_2 = 8, C2_10_10_10 = 9, C8_8_8_8 = 10, C32_32 = 11,
              C16_16_16_16 = 12, C32_32_32_32 = 14, C5_6_5 = 16, C1_5_5_5 = 17, C5_5_5_1 = 18,
              C4_4_4_4 = 19, C8_24 = 20, C24_8 = 21;
}

// Descriptor NUM_FORMAT values (CB NUMBER_TYPE matches except for sRGB).
namespace NumFormat {
constexpr u32 Unorm = 0, Snorm = 1, Uscaled = 2, Sscaled = 3, Uint = 4, Sint = 5, SnormNz = 6,
              Float = 7, Srgb = 9;
}

} // namespace Gpu::Liverpool
