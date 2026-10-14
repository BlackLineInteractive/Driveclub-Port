#pragma once

// MSL helpers shared by every translated shader.

namespace Gpu::Shader {

inline constexpr const char* kMslPrelude = R"MSL(#include <metal_stdlib>
using namespace metal;

// Guest VA -> physical page table: a header followed by one entry per page,
// the page's (phys - va) in 16 KB units (INT_MIN = unmapped).
struct DcMap { ulong base; ulong pages; ulong split; uint shift; uint pad; ulong limit; };
struct DcDraw { float4 vp_scale; float4 vp_offset; float2 rt_scale; uint vte; uint pad; };
struct DcMem { device uint* lo; device uint* hi; device const DcMap* map; };

// ---- guest memory ----
static inline device uint* dc_ptr(thread const DcMem& m, ulong va) {
    ulong i = (va - m.map->base) >> m.map->shift;
    if (i >= m.map->pages) return nullptr;
    int e = ((device const int*)(m.map + 1))[i];
    if (e == int(0x80000000u)) return nullptr;
    ulong p = va + ulong(long(e) << 14);
    if (p >= m.map->limit) return nullptr;
    return p < m.map->split ? m.lo + (p >> 2) : m.hi + ((p - m.map->split) >> 2);
}
// `bytes` contiguous bytes at dword-aligned `va` (one translation for a
// multi-dword load), or nullptr.
static inline device uint* dc_ptr_n(thread const DcMem& m, ulong va, uint bytes) {
    device uint* p = dc_ptr(m, va);
    ulong last = va + ulong(bytes - 4u);
    if (((va ^ last) >> m.map->shift) != 0ul && dc_ptr(m, last) != p + ((bytes - 4u) >> 2)) return nullptr;
    return p;
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
// Byte / short stores: atomic and+or, since neighbouring lanes may write the
// other bytes of the same dword (a plain read-modify-write also trips the
// AMD Metal compiler).
static inline void dc_st_part(thread const DcMem& m, ulong va, uint v, uint bytes) {
    device uint* p = dc_ptr(m, va & ~3ul);
    if (!p) return;
    uint sh = uint(va & 3ul) * 8u;
    uint mask = (bytes == 1u ? 0xffu : 0xffffu) << sh;
    device atomic_uint* a = (device atomic_uint*)p;
    atomic_fetch_and_explicit(a, ~mask, memory_order_relaxed);
    atomic_fetch_or_explicit(a, (v << sh) & mask, memory_order_relaxed);
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
    uint words = dfmt == 14u ? 4u : dfmt == 13u ? 3u : (dfmt == 11u || dfmt == 12u) ? 2u : 1u;
    uint4 w = uint4(0u);
    if ((a & 3ul) == 0ul) {
        device uint* p = dc_ptr_n(m, a, words * 4u);
        if (p) for (uint k = 0u; k < words; ++k) w[k] = p[k];
    } else {
        for (uint k = 0u; k < words; ++k) w[k] = dc_ld(m, a + ulong(k) * 4ul);
    }
    uint w0 = w.x;
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
    case 11u: r.xy = w.xy; n = 2u; break;
    case 12u: r = uint4(w.x & 0xffffu, w.x >> 16, w.y & 0xffffu, w.y >> 16); bits = uint4(16u); n = 4u; break;
    case 13u: r.xyz = w.xyz; n = 3u; break;
    case 14u: r = w; n = 4u; break;
    default: break;
    }
    return dc_fmt_convert(r, bits, nfmt, n);
}
// Typed buffer store: packs v (float bits or integers) into dfmt/nfmt.
static inline uint dc_pack(uint v, uint bits, uint nfmt) {
    uint maxu = bits >= 32u ? 0xffffffffu : ((1u << bits) - 1u);
    float f = as_type<float>(v);
    switch (nfmt) {
    case 0u: return uint(rint(saturate(f) * float(maxu)));
    case 1u: return uint(int(rint(clamp(f, -1.0f, 1.0f) * float(maxu >> 1)))) & maxu;
    case 2u: return uint(clamp(f, 0.0f, float(maxu))) & maxu;
    case 3u: return uint(int(f)) & maxu;
    case 4u: return v & maxu;
    case 5u: return v & maxu;
    default: return bits == 32u ? v : bits == 16u ? uint(as_type<ushort>(half(f))) : 0u;
    }
}
static inline void dc_store_fmt(thread const DcMem& m, ulong a, uint4 v, uint dfmt, uint nfmt) {
    switch (dfmt) {
    case 1u: dc_st_part(m, a, dc_pack(v.x, 8u, nfmt), 1u); break;
    case 2u: dc_st_part(m, a, dc_pack(v.x, 16u, nfmt), 2u); break;
    case 3u: dc_st_part(m, a, dc_pack(v.x, 8u, nfmt) | (dc_pack(v.y, 8u, nfmt) << 8), 2u); break;
    case 4u: dc_st(m, a, v.x); break;
    case 5u: dc_st(m, a, dc_pack(v.x, 16u, nfmt) | (dc_pack(v.y, 16u, nfmt) << 16)); break;
    case 10u:
        dc_st(m, a, dc_pack(v.x, 8u, nfmt) | (dc_pack(v.y, 8u, nfmt) << 8) | (dc_pack(v.z, 8u, nfmt) << 16) |
                        (dc_pack(v.w, 8u, nfmt) << 24));
        break;
    case 11u: dc_st(m, a, v.x); dc_st(m, a + 4ul, v.y); break;
    case 12u:
        dc_st(m, a, dc_pack(v.x, 16u, nfmt) | (dc_pack(v.y, 16u, nfmt) << 16));
        dc_st(m, a + 4ul, dc_pack(v.z, 16u, nfmt) | (dc_pack(v.w, 16u, nfmt) << 16));
        break;
    case 13u: dc_st(m, a, v.x); dc_st(m, a + 4ul, v.y); dc_st(m, a + 8ul, v.z); break;
    case 14u: dc_st(m, a, v.x); dc_st(m, a + 4ul, v.y); dc_st(m, a + 8ul, v.z); dc_st(m, a + 12ul, v.w); break;
    default: break;
    }
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
// V_CVT_PKRTZ: round toward zero, so finite values never become infinity.
static inline uint dc_f16_rtz(float f) {
    if (isnan(f)) return 0x7e00u;
    if (isinf(f)) return f > 0.0f ? 0x7c00u : 0xfc00u;
    half h = half(clamp(f, -65504.0f, 65504.0f));
    if (abs(float(h)) > abs(f)) {
        ushort b = as_type<ushort>(h);
        h = as_type<half>(ushort(b - 1u)); // one ulp toward zero
    }
    return uint(as_type<ushort>(h));
}
// The VALU clamp modifier: NaN becomes 0 (Metal's saturate keeps NaN).
static inline float dc_sat(float f) { return f > 0.0f ? min(f, 1.0f) : 0.0f; }
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
