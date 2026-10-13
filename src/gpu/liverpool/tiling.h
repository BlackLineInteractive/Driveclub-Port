// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Gpu::Liverpool {

enum class ArrayMode : u32 {
    ArrayLinearGeneral = 0,
    ArrayLinearAligned = 1,
    Array1DTiledThin1 = 2,
    Array1DTiledThick = 3,
    Array2DTiledThin1 = 4,
    ArrayPrtTiledThin1 = 5,
    ArrayPrt2DTiledThin1 = 6,
    Array2DTiledThick = 7,
    Array2DTiledXThick = 8,
    ArrayPrtTiledThick = 9,
    ArrayPrt2DTiledThick = 10,
    ArrayPrt3DTiledThin1 = 11,
    Array3DTiledThin1 = 12,
    Array3DTiledThick = 13,
    Array3DTiledXThick = 14,
    ArrayPrt3DTiledThick = 15,
};

enum class TileMode : u32 {
    Depth2DThin64 = 0,
    Depth2DThin128 = 1,
    Depth2DThin256 = 2,
    Depth2DThin512 = 3,
    Depth2DThin1K = 4,
    Depth1DThin = 5,
    Depth2DThinPrt256 = 6,
    Depth2DThinPrt1K = 7,
    DisplayLinearAligned = 8,
    Display1DThin = 9,
    Display2DThin = 10,
    DisplayThinPrt = 11,
    Display2DThinPrt = 12,
    Thin1DThin = 13,
    Thin2DThin = 14,
    Thin3DThin = 15,
    ThinThinPrt = 16,
    Thin2DThinPrt = 17,
    Thin3DThinPrt = 18,
    Thick1DThick = 19,
    Thick2DThick = 20,
    Thick3DThick = 21,
    ThickThickPrt = 22,
    Thick2DThickPrt = 23,
    Thick3DThickPrt = 24,
    Thick2DXThick = 25,
    Thick3DXThick = 26,
    DisplayLinearGeneral = 31,
};

} // namespace Gpu::Liverpool
