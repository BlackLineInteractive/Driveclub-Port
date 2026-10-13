#pragma once

// Minimal SHA-1, used only to compute Sony NIDs from symbol names.

#include <array>
#include <cstring>
#include <string_view>

#include "common/types.h"

namespace Common {

inline std::array<u8, 20> Sha1(const u8* data, size_t len) {
    u32 h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](u32 v, int s) { return (v << s) | (v >> (32 - s)); };

    const u64 bit_len = static_cast<u64>(len) * 8;
    const size_t padded = ((len + 8) / 64 + 1) * 64;
    u8 block[64];

    for (size_t off = 0; off < padded; off += 64) {
        for (size_t i = 0; i < 64; ++i) {
            const size_t pos = off + i;
            if (pos < len) {
                block[i] = data[pos];
            } else if (pos == len) {
                block[i] = 0x80;
            } else if (pos >= padded - 8) {
                block[i] = static_cast<u8>(bit_len >> (8 * (padded - 1 - pos)));
            } else {
                block[i] = 0;
            }
        }

        u32 w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (u32(block[i * 4]) << 24) | (u32(block[i * 4 + 1]) << 16) |
                   (u32(block[i * 4 + 2]) << 8) | u32(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            u32 f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const u32 t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::array<u8, 20> out;
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<u8>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<u8>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<u8>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<u8>(h[i]);
    }
    return out;
}

} // namespace Common
