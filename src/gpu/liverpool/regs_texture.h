// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Gpu::Liverpool {

struct BorderColorBuffer {
    u64 base_addr : 40;
    template <typename T = const void*>
    T Address() const {
        return reinterpret_cast<T>(base_addr << 8);
    }
};

} // namespace Gpu::Liverpool
