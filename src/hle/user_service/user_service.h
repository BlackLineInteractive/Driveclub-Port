#pragma once

#include "common/types.h"

namespace Hle::UserService {

constexpr s32 kInitialUserId = 0x10000000;

void RegisterUserService();

} // namespace Hle::UserService
