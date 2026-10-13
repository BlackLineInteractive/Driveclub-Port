// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Gpu::Liverpool {

enum class DataFormat : u32 {
    FormatInvalid = 0,
    Format8 = 1,
    Format16 = 2,
    Format8_8 = 3,
    Format32 = 4,
    Format16_16 = 5,
    Format10_11_11 = 6,
    Format11_11_10 = 7,
    Format10_10_10_2 = 8,
    Format2_10_10_10 = 9,
    Format8_8_8_8 = 10,
    Format32_32 = 11,
    Format16_16_16_16 = 12,
    Format32_32_32 = 13,
    Format32_32_32_32 = 14,
    Format5_6_5 = 16,
    Format1_5_5_5 = 17,
    Format5_5_5_1 = 18,
    Format4_4_4_4 = 19,
    Format8_24 = 20,
    Format24_8 = 21,
    FormatX24_8_32 = 22,
    FormatGB_GR = 32,
    FormatBG_RG = 33,
    Format5_9_9_9 = 34,
    FormatBc1 = 35,
    FormatBc2 = 36,
    FormatBc3 = 37,
    FormatBc4 = 38,
    FormatBc5 = 39,
    FormatBc6 = 40,
    FormatBc7 = 41,
};

enum class NumberFormat : u32 {
    Unorm = 0,
    Snorm = 1,
    Uscaled = 2,
    Sscaled = 3,
    Uint = 4,
    Sint = 5,
    SnormNz = 6,
    Float = 7,
    Srgb = 9,
    Ubnorm = 10,
    UbnormNz = 11,
    Ubint = 12,
    Ubscaled = 13,
};

inline u32 NumBitsPerBlock(DataFormat format) {
    switch (format) {
    case DataFormat::Format8: return 8;
    case DataFormat::Format16:
    case DataFormat::Format8_8:
    case DataFormat::Format5_6_5:
    case DataFormat::Format1_5_5_5:
    case DataFormat::Format5_5_5_1:
    case DataFormat::Format4_4_4_4: return 16;
    case DataFormat::Format32:
    case DataFormat::Format16_16:
    case DataFormat::Format10_11_11:
    case DataFormat::Format11_11_10:
    case DataFormat::Format10_10_10_2:
    case DataFormat::Format2_10_10_10:
    case DataFormat::Format8_8_8_8:
    case DataFormat::Format8_24:
    case DataFormat::Format24_8: return 32;
    case DataFormat::Format32_32:
    case DataFormat::Format16_16_16_16:
    case DataFormat::FormatBc1:
    case DataFormat::FormatBc4: return 64;
    case DataFormat::Format32_32_32: return 96;
    case DataFormat::Format32_32_32_32:
    case DataFormat::FormatBc2:
    case DataFormat::FormatBc3:
    case DataFormat::FormatBc5:
    case DataFormat::FormatBc6:
    case DataFormat::FormatBc7: return 128;
    default: return 0;
    }
}

} // namespace Gpu::Liverpool
