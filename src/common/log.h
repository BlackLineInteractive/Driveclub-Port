#pragma once

#include <cstdio>

#define DC_LOG(tag, fmt, ...) std::fprintf(stderr, "[" tag "] " fmt "\n" __VA_OPT__(, ) __VA_ARGS__)
#define DC_INFO(fmt, ...) DC_LOG("info", fmt __VA_OPT__(, ) __VA_ARGS__)
#define DC_WARN(fmt, ...) DC_LOG("warn", fmt __VA_OPT__(, ) __VA_ARGS__)
#define DC_ERROR(fmt, ...) DC_LOG("error", fmt __VA_OPT__(, ) __VA_ARGS__)
