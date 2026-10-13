#pragma once

// MSL helpers shared by every translated shader.

namespace Gpu::Shader {

inline constexpr const char* kMslPrelude = R"MSL(#include <metal_stdlib>
using namespace metal;

struct DcRange { ulong start; ulong size; ulong phys; };
struct DcMap { uint count; uint pad; ulong split; DcRange r[32]; };
struct DcDraw { float4 vp_scale; float4 vp_offset; float2 rt_scale; uint vte; uint pad; };
struct DcMem { device uint* lo; device uint* hi; constant DcMap* map; };

// ---- guest memory ----
static inline device uint* dc_ptr(thread const DcMem& m, ulong va) {
    constant DcMap& mp = *m.map;
    for (uint i = 0u; i < mp.count; ++i) {
        ulong d = va - mp.r[i].start;
        if (d < mp.r[i].size) {
            ulong p = mp.r[i].phys + d;
            return p < mp.split ? m.lo + (p >> 2) : m.hi + ((p - mp.split) >> 2);
        }
    }
    return nullptr;
}
static inline uint dc_ld(thread const DcMem& m, ulong va) {
    device uint* p = dc_ptr(m, va & ~3ul);
    uint lo = p ? *p : 0u;
    uint sh = uint(va & 3ul) * 8u;
    if (sh == 0u) return lo;
    device uint* q = dc_ptr(m, (va & ~3ul) + 4ul);
    uint hi = q ? *q : 0u;
    return (lo >> sh) | (hi << (32u - sh));
}
static inline void dc_st(thread const DcMem& m, ulong va, uint v) {
    device uint* p = dc_ptr(m, va & ~3ul);
    if (p) *p = v;
}
static inline void dc_st_part(thread const DcMem& m, ulong va, uint v, uint bytes) {
    device uint* p = dc_ptr(m, va & ~3ul);
    if (!p) return;
    uint sh = uint(va & 3ul) * 8u;
    uint mask = (bytes == 1u ? 0xffu : 0xffffu) << sh;
    *p = (*p & ~mask) | ((v << sh) & mask);
}
static inline ulong dc_vbase(uint a, uint b) { return ulong(a) | (ulong(b & 0xfffu) << 32); }
static inline uint dc_vstride(uint b) { return (b >> 16) & 0x3fffu; }

// ---- typed buffer fetch (BUF_DATA_FORMAT / BUF_NUM_FORMAT) ----
static inline float dc_ufloat(uint v, uint mbits) {
    uint e = (v >> mbits) & 31u;
    uint mt = v & ((1u << mbits) - 1u);
    float fm = float(mt) / float(1u << mbits);
    if (e == 0u) return ldexp(fm, -14);
    if (e == 31u) return mt == 0u ? INFINITY : NAN;
    return ldexp(1.0f + fm, int(e) - 15);
}
static inline uint4 dc_fmt_convert(uint4 r, uint4 bits, uint nfmt, uint n) {
    bool is_int = nfmt == 4u || nfmt == 5u;
    uint4 o = uint4(0u, 0u, 0u, is_int ? 1u : 0x3f800000u);
    for (uint k = 0u; k < n; ++k) {
        uint v = r[k];
        uint b = bits[k];
        int sv = b >= 32u ? int(v) : (int(v << (32u - b)) >> (32u - b));
        float maxu = b >= 32u ? 4294967295.0f : float((1u << b) - 1u);
        uint res;
        switch (nfmt) {
        case 0u: res = as_type<uint>(float(v) / maxu); break;
        case 1u:
        case 6u: res = as_type<uint>(max(float(sv) / float((1u << (b - 1u)) - 1u), -1.0f)); break;
        case 2u: res = as_type<uint>(float(v)); break;
        case 3u: res = as_type<uint>(float(sv)); break;
        case 4u: res = v; break;
        case 5u: res = uint(sv); break;
        default:
            res = b == 32u ? v
                : b == 16u ? as_type<uint>(float(as_type<half>(ushort(v))))
                           : as_type<uint>(dc_ufloat(v, b - 5u));
            break;
        }
        o[k] = res;
    }
    return o;
}
static inline uint4 dc_load_fmt(thread const DcMem& m, ulong a, uint dfmt, uint nfmt) {
    uint w0 = dc_ld(m, a);
    uint4 r = uint4(0u);
    uint4 bits = uint4(32u);
    uint n = 0u;
    switch (dfmt) {
    case 1u: r.x = w0 & 0xffu; bits = uint4(8u); n = 1u; break;
    case 2u: r.x = w0 & 0xffffu; bits = uint4(16u); n = 1u; break;
    case 3u: r.xy = uint2(w0 & 0xffu, (w0 >> 8) & 0xffu); bits = uint4(8u); n = 2u; break;
    case 4u: r.x = w0; n = 1u; break;
    case 5u: r.xy = uint2(w0 & 0xffffu, w0 >> 16); bits = uint4(16u); n = 2u; break;
    case 6u: r.xyz = uint3(w0 & 0x7ffu, (w0 >> 11) & 0x7ffu, w0 >> 22); bits = uint4(11u, 11u, 10u, 0u); n = 3u; break;
    case 7u: r.xyz = uint3(w0 & 0x3ffu, (w0 >> 10) & 0x7ffu, w0 >> 21); bits = uint4(10u, 11u, 11u, 0u); n = 3u; break;
    case 8u: r = uint4(w0 & 3u, (w0 >> 2) & 0x3ffu, (w0 >> 12) & 0x3ffu, w0 >> 22); bits = uint4(2u, 10u, 10u, 10u); n = 4u; break;
    case 9u: r = uint4(w0 & 0x3ffu, (w0 >> 10) & 0x3ffu, (w0 >> 20) & 0x3ffu, w0 >> 30); bits = uint4(10u, 10u, 10u, 2u); n = 4u; break;
    case 10u: r = uint4(w0 & 0xffu, (w0 >> 8) & 0xffu, (w0 >> 16) & 0xffu, w0 >> 24); bits = uint4(8u); n = 4u; break;
    case 11u: r.xy = uint2(w0, dc_ld(m, a + 4ul)); n = 2u; break;
    case 12u: { uint w1 = dc_ld(m, a + 4ul); r = uint4(w0 & 0xffffu, w0 >> 16, w1 & 0xffffu, w1 >> 16); bits = uint4(16u); n = 4u; break; }
    case 13u: r.xyz = uint3(w0, dc_ld(m, a + 4ul), dc_ld(m, a + 8ul)); n = 3u; break;
    case 14u: r = uint4(w0, dc_ld(m, a + 4ul), dc_ld(m, a + 8ul), dc_ld(m, a + 12ul)); n = 4u; break;
    default: break;
    }
    return dc_fmt_convert(r, bits, nfmt, n);
}
static inline uint dc_sel(uint4 v, uint sel, bool is_int) {
    switch (sel & 7u) {
    case 1u: return is_int ? 1u : 0x3f800000u;
    case 4u: return v.x;
    case 5u: return v.y;
    case 6u: return v.z;
    case 7u: return v.w;
    default: return 0u;
    }
}
static inline uint4 dc_swizzle(uint4 v, uint w3) {
    uint nf = (w3 >> 12) & 7u;
    bool is_int = nf == 4u || nf == 5u;
    return uint4(dc_sel(v, w3, is_int), dc_sel(v, w3 >> 3, is_int), dc_sel(v, w3 >> 6, is_int), dc_sel(v, w3 >> 9, is_int));
}

// ---- ALU helpers ----
static inline float dc_mul_legacy(float a, float b) { return (a == 0.0f || b == 0.0f) ? 0.0f : a * b; }
static inline uint dc_bfe_u(uint v, uint off, uint w) {
    off &= 31u; w &= 31u;
    if (w == 0u) return 0u;
    return uint((ulong(v) >> off) & ((1ul << w) - 1ul));
}
static inline uint dc_bfe_i(uint v, uint off, uint w) {
    off &= 31u; w &= 31u;
    if (w == 0u) return 0u;
    uint x = dc_bfe_u(v, off, w);
    return uint(int(x << (32u - w)) >> (32u - w));
}
// Scalar BFE takes a 7-bit width.
static inline uint dc_sbfe_u(uint v, uint off, uint w) {
    off &= 31u;
    if (w == 0u) return 0u;
    if (w >= 32u) return v >> off;
    return uint((ulong(v) >> off) & ((1ul << w) - 1ul));
}
static inline uint dc_sbfe_i(uint v, uint off, uint w) {
    if (w == 0u) return 0u;
    if (w >= 32u) return uint(int(v) >> (off & 31u));
    uint x = dc_sbfe_u(v, off, w);
    return uint(int(x << (32u - w)) >> (32u - w));
}
static inline uint dc_ffbh_u(uint v) { return v == 0u ? 0xffffffffu : clz(v); }
static inline uint dc_ffbl(uint v) { return v == 0u ? 0xffffffffu : ctz(v); }
static inline uint dc_ffbh_i(uint v) { uint t = int(v) < 0 ? ~v : v; return t == 0u ? 0xffffffffu : clz(t); }
static inline uint dc_cvt_u32(float f) {
    if (isnan(f) || f <= 0.0f) return 0u;
    return f >= 4294967040.0f ? 0xffffffffu : uint(f);
}
static inline uint dc_cvt_i32(float f) {
    if (isnan(f)) return 0u;
    if (f <= -2147483648.0f) return 0x80000000u;
    if (f >= 2147483520.0f) return 0x7fffffffu;
    return uint(int(f));
}
static inline uint dc_f16(float f) { return uint(as_type<ushort>(half(f))); }
static inline float dc_f32(uint v) { return float(as_type<half>(ushort(v & 0xffffu))); }
static inline float dc_med3(float a, float b, float c) { return max(min(a, b), min(max(a, b), c)); }
static inline int dc_med3i(int a, int b, int c) { return max(min(a, b), min(max(a, b), c)); }
static inline uint dc_med3u(uint a, uint b, uint c) { return max(min(a, b), min(max(a, b), c)); }
static inline float dc_cubeid(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return z < 0.0f ? 5.0f : 4.0f;
    if (abs(y) >= abs(x)) return y < 0.0f ? 3.0f : 2.0f;
    return x < 0.0f ? 1.0f : 0.0f;
}
static inline float dc_cubesc(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return z < 0.0f ? -x : x;
    if (abs(y) >= abs(x)) return x;
    return x < 0.0f ? z : -z;
}
static inline float dc_cubetc(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return -y;
    if (abs(y) >= abs(x)) return y < 0.0f ? -z : z;
    return -y;
}
static inline float dc_cubema(float x, float y, float z) {
    if (abs(z) >= abs(x) && abs(z) >= abs(y)) return 2.0f * z;
    if (abs(y) >= abs(x)) return 2.0f * y;
    return 2.0f * x;
}
static inline bool dc_class(float f, uint mask) {
    uint b = as_type<uint>(f);
    bool neg = (b >> 31) != 0u;
    uint e = (b >> 23) & 0xffu;
    uint mt = b & 0x7fffffu;
    uint bit;
    if (e == 0xffu) bit = mt != 0u ? ((mt & 0x400000u) != 0u ? 1u : 0u) : (neg ? 2u : 9u);
    else if (e == 0u) bit = mt == 0u ? (neg ? 5u : 6u) : (neg ? 4u : 7u);
    else bit = neg ? 3u : 8u;
    return ((mask >> bit) & 1u) != 0u;
}

// ---- position: guest viewport transform -> Metal clip space ----
static inline float4 dc_viewport(float4 p, constant DcDraw& d) {
    float w = p.w;
    float3 q = p.xyz;
    if ((d.vte & 0x100u) == 0u) q.xy /= w;
    if ((d.vte & 0x200u) == 0u) q.z /= w;
    float3 s = q * d.vp_scale.xyz + d.vp_offset.xyz;
    float2 n = float2(s.x * d.rt_scale.x - 1.0f, 1.0f - s.y * d.rt_scale.y);
    return float4(n * w, s.z * w, w);
}
)MSL";

} // namespace Gpu::Shader
