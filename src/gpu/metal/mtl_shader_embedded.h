// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Gpu::Metal::EmbeddedShaders {

constexpr const char* kShaderVsUI = R"MSL(
#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T>
[[clang::optnone]] T spvFMul(T l, T r)
{
    return fma(l, r, T(0));
}

template<typename T, int Cols, int Rows>
[[clang::optnone]] vec<T, Cols> spvFMulVectorMatrix(vec<T, Rows> v, matrix<T, Cols, Rows> m)
{
    vec<T, Cols> res = vec<T, Cols>(0);
    for (uint i = Rows; i > 0; --i)
    {
        vec<T, Cols> tmp(0);
        for (uint j = 0; j < Cols; ++j)
        {
            tmp[j] = m[j][i - 1];
        }
        res = fma(tmp, vec<T, Cols>(v[i - 1]), res);
    }
    return res;
}

template<typename T, int Cols, int Rows>
[[clang::optnone]] vec<T, Rows> spvFMulMatrixVector(matrix<T, Cols, Rows> m, vec<T, Cols> v)
{
    vec<T, Rows> res = vec<T, Rows>(0);
    for (uint i = Cols; i > 0; --i)
    {
        res = fma(m[i - 1], vec<T, Rows>(v[i - 1]), res);
    }
    return res;
}

template<typename T, int LCols, int LRows, int RCols, int RRows>
[[clang::optnone]] matrix<T, RCols, LRows> spvFMulMatrixMatrix(matrix<T, LCols, LRows> l, matrix<T, RCols, RRows> r)
{
    static_assert(LCols == RRows, "column-row configuration mismatch");
    matrix<T, RCols, LRows> res;
    for (uint i = 0; i < RCols; i++)
    {
        vec<T, LRows> tmp(0);
        for (uint j = 0; j < LCols; j++)
        {
            tmp = fma(vec<T, LRows>(r[i][j]), l[j], tmp);
        }
        res[i] = tmp;
    }
    return res;
}

template<typename T>
[[clang::optnone]] T spvFAdd(T l, T r)
{
    return fma(T(1), l, r);
}

template<typename T>
[[clang::optnone]] T spvFSub(T l, T r)
{
    return fma(T(-1), r, l);
}

struct full_result_i32x2
{
    int _m0;
    int _m1;
};

struct full_result_u32x2
{
    uint _m0;
    uint _m1;
};

struct frexp_result_f32
{
    float _m0;
    int _m1;
};

struct AuxData
{
    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    uint4 buf_offsets0;
    uint4 buf_offsets1;
    uint2 buf_offsets2;
};

struct _58
{
    uint data[1];
};

struct _65
{
    float data[1];
};

struct main0_out
{
    float4 out_attr0 [[user(locn0)]];
    float4 out_attr1 [[user(locn1)]];
    float4 out_attr2 [[user(locn2)]];
    float4 out_attr3 [[user(locn3)]];
    float4 out_attr4 [[user(locn4)]];
    float4 gl_Position [[position]];
};

vertex main0_out main0(const device _58& ssbo_1 [[buffer(0)]], const device _58& ssbo_2 [[buffer(1)]], const device _58& ssbo_3 [[buffer(2)]], uint gl_VertexIndex [[vertex_id]])
{
    main0_out out = {};
    uint _81 = 3u & gl_VertexIndex;
    float _83 = float(extract_bits(_81, 1u, 1u));
    bool _84 = 0.0 < _83;
    uint4 _182 = uint4(ssbo_1.data[0u], ssbo_1.data[0u + 1u], ssbo_1.data[0u + 2u], ssbo_1.data[0u + 3u]);
    uint _185 = _182.z;
    uint _186 = _182.w;
    float _189 = float(extract_bits(_81 - 1u, 1u, 1u));
    float _191 = as_type<float>(ssbo_1.data[16u]);
    float _192 = as_type<float>(ssbo_1.data[17u]);
    bool _194 = 0.0 < _189;
    float _195 = as_type<float>(ssbo_1.data[19u]);
    float _196 = _84 ? _191 : _192;
    float _197 = as_type<float>(ssbo_1.data[18u]);
    float _201 = fma(2.0, _83, -1.0);
    float _203 = fma(2.0, _189, -1.0);
    float _204 = _194 ? _197 : _195;
    float _246 = spvFAdd(spvFMul(_83, as_type<float>(_185)), as_type<float>(_182.x));
    float _250 = spvFAdd(spvFMul(_189, as_type<float>(_186)), as_type<float>(_182.y));
    float _254 = fast::max(0.0, spvFMul(spvFSub(_246, _84 ? _192 : _191), _201));
    float _256 = fast::max(0.0, spvFMul(spvFSub(_250, _194 ? _195 : _197), _203));
    float _259 = spvFAdd(spvFMul(-_201, _254), _246);
    float _263 = spvFAdd(spvFMul(-_203, _256), _250);
    bool _265 = 0.0 > spvFMul(spvFSub(_259, _196), _201);
    float _266 = _265 ? _196 : _259;
    bool _269 = 0.0 > spvFMul(spvFSub(_263, _204), _203);
    float _270 = _269 ? _204 : _263;
    float _279 = spvFAdd(spvFMul(as_type<float>(ssbo_1.data[2u]), _266), spvFAdd(spvFMul(as_type<float>(ssbo_1.data[6u]), _270), as_type<float>(ssbo_1.data[14u])));
    float _286 = spvFAdd(spvFMul(as_type<float>(ssbo_1.data[3u]), _266), spvFAdd(spvFMul(as_type<float>(ssbo_1.data[7u]), _270), as_type<float>(ssbo_1.data[15u])));
    float _288 = spvFMul(as_type<float>(ssbo_2.data[107u]), _286);
    float _296 = spvFMul(as_type<float>(ssbo_2.data[106u]), _286);
    float _298 = spvFSub(_270, as_type<float>(ssbo_1.data[29u]));
    float _306 = spvFSub(_266, as_type<float>(ssbo_1.data[28u]));
    float _309 = spvFAdd(spvFMul(as_type<float>(ssbo_2.data[80u]), _296), spvFAdd(spvFMul(as_type<float>(ssbo_2.data[84u]), _288), spvFAdd(spvFMul(as_type<float>(ssbo_2.data[92u]), _286), spvFMul(as_type<float>(ssbo_2.data[88u]), _279))));
    float _312 = spvFAdd(spvFMul(as_type<float>(ssbo_2.data[81u]), _296), spvFAdd(spvFMul(as_type<float>(ssbo_2.data[85u]), _288), spvFAdd(spvFMul(as_type<float>(ssbo_2.data[93u]), _286), spvFMul(as_type<float>(ssbo_2.data[89u]), _279))));
    float _316 = as_type<float>(ssbo_1.data[30u]);
    float _319 = rsqrt(spvFAdd(spvFMul(_316, _316), spvFAdd(spvFMul(_298, _298), spvFMul(_306, _306))));
    float _320 = spvFMul(_298, _319);
    float _321 = spvFMul(_306, _319);
    float _323 = spvFMul(-_316, _319);
    float _330 = 1.0 / spvFMul(fast::max(0.00999999977648258209228515625, abs(_323)), (0.0 > _323) ? (-1.0) : 1.0);
    float _333 = spvFMul(spvFMul(as_type<float>(ssbo_1.data[18u]), _309), _330);
    float _338 = spvFMul(spvFMul(as_type<float>(ssbo_1.data[22u]), _312), _330);
    float _355 = spvFAdd(abs(spvFAdd(spvFMul(as_type<float>(ssbo_1.data[17u]), _309), spvFMul(_320, _333))), abs(spvFAdd(spvFMul(as_type<float>(ssbo_1.data[21u]), _312), spvFMul(_320, _338))));
    float _356 = _265 ? spvFSub(1.0, _83) : _83;
    float _357 = _269 ? spvFSub(1.0, _189) : _189;
    float _358 = fma(2.0, _357, -1.0);
    float _361 = spvFAdd(abs(spvFAdd(spvFMul(as_type<float>(ssbo_1.data[16u]), _309), spvFMul(_321, _333))), abs(spvFAdd(spvFMul(as_type<float>(ssbo_1.data[20u]), _312), spvFMul(_321, _338))));
    float _363 = spvFAdd(spvFMul(_358, _355), _270);
    float _368 = fma(2.0, _356, -1.0);
    float _370 = spvFAdd(spvFMul(_368, _361), _266);
    float _373 = spvFAdd(spvFMul(as_type<float>(ssbo_1.data[34u]), _370), spvFAdd(spvFMul(as_type<float>(ssbo_1.data[38u]), _363), as_type<float>(ssbo_1.data[46u])));
    float _374 = as_type<float>(ssbo_3.data[436u]);
    bool _375 = _374 < _373;
    uint _378;
    if (_375)
    {
        _378 = ssbo_3.data[437u];
    }
    else
    {
        _378 = as_type<uint>(_189);
    }
    uint _383;
    if (!_375)
    {
        _383 = as_type<uint>(-as_type<float>(ssbo_3.data[438u]));
    }
    else
    {
        _383 = _378;
    }
    float _384 = as_type<float>(_185);
    float _386 = as_type<float>(_186);
    float _392 = spvFMul(spvFMul(_361, _368), 1.0 / fast::max(0.0, spvFSub(_384, _254)));
    float _393 = spvFMul(spvFMul(_355, _358), 1.0 / fast::max(0.0, spvFSub(_386, _256)));
    float _396 = 1.0 / _392;
    float _427 = 1.0 / spvFMul(fma(2.0, _396, -1.0), 1.0 / spvFMul(2.0, _396));
    float _428 = spvFMul(2.0, _392);
    float _429 = spvFMul(2.0, _393);
    float _430 = spvFSub(1.0, fast::clamp(spvFMul(spvFSub(_373, _374), as_type<float>(_383)), 0.0, 1.0));
    out.gl_Position.x = spvFAdd(spvFMul(as_type<float>(ssbo_1.data[0u]), _370), spvFAdd(spvFMul(as_type<float>(ssbo_1.data[4u]), _363), as_type<float>(ssbo_1.data[12u])));
    out.gl_Position.y = spvFAdd(spvFMul(as_type<float>(ssbo_1.data[1u]), _370), spvFAdd(spvFMul(as_type<float>(ssbo_1.data[5u]), _363), as_type<float>(ssbo_1.data[13u])));
    out.gl_Position.z = spvFAdd(spvFMul(as_type<float>(ssbo_1.data[2u]), _370), spvFAdd(spvFMul(as_type<float>(ssbo_1.data[6u]), _363), as_type<float>(ssbo_1.data[14u])));
    out.gl_Position.w = spvFAdd(spvFMul(as_type<float>(ssbo_1.data[3u]), _370), spvFAdd(spvFMul(as_type<float>(ssbo_1.data[7u]), _363), as_type<float>(ssbo_1.data[15u])));
    uint4 _457 = uint4(ssbo_1.data[4u], ssbo_1.data[4u + 1u], ssbo_1.data[4u + 2u], ssbo_1.data[4u + 3u]);
    float _465 = 1.0 / _393;
    float _470 = 1.0 / spvFMul(fma(2.0, _465, -1.0), 1.0 / spvFMul(2.0, _465));
    float _481 = as_type<float>(_457.z);
    float _487 = as_type<float>(_457.w);
    out.out_attr4.x = as_type<float>(ssbo_1.data[0u] + (gl_VertexIndex >> 2u));
    out.out_attr4.y = _363;
    out.out_attr4.z = _363;
    out.out_attr4.w = _363;
    out.out_attr3.x = exp2(spvFMul(2.2000000476837158203125, log2(abs(spvFMul(0.0039215688593685626983642578125, float(255u & ssbo_1.data[14u]))))));
    out.out_attr3.y = exp2(spvFMul(2.2000000476837158203125, log2(abs(spvFMul(0.0039215688593685626983642578125, float(extract_bits(ssbo_1.data[14u], 8u, 8u)))))));
    out.out_attr3.z = exp2(spvFMul(2.2000000476837158203125, log2(abs(spvFMul(0.0039215688593685626983642578125, float(extract_bits(ssbo_1.data[14u], 16u, 8u)))))));
    out.out_attr3.w = spvFMul(spvFMul(0.0039215688593685626983642578125, float(extract_bits(ssbo_1.data[14u], 24u, 8u))), _430);
    out.out_attr2.x = 1.0 / abs(spvFMul(_428, _427));
    out.out_attr2.y = 1.0 / abs(spvFMul(_429, _470));
    out.out_attr2.z = _370;
    out.out_attr2.w = _363;
    out.out_attr1.x = spvFAdd(spvFMul(_481, _392), spvFAdd(spvFMul(_481, _356), spvFAdd(spvFMul(_201, -spvFMul(spvFMul(_254, 1.0 / _384), _481)), as_type<float>(_457.x))));
    out.out_attr1.y = spvFAdd(spvFMul(_393, _487), spvFAdd(spvFMul(_487, _357), spvFAdd(spvFMul(-spvFMul(spvFMul(_256, 1.0 / _386), _487), _203), as_type<float>(_457.y))));
    out.out_attr1.z = spvFAdd(spvFMul(_428, _427), _356);
    out.out_attr1.w = spvFAdd(spvFMul(_429, _470), _357);
    out.out_attr0.x = exp2(spvFMul(2.2000000476837158203125, log2(abs(spvFMul(0.0039215688593685626983642578125, float(255u & ssbo_1.data[12u]))))));
    out.out_attr0.y = exp2(spvFMul(2.2000000476837158203125, log2(abs(spvFMul(0.0039215688593685626983642578125, float(extract_bits(ssbo_1.data[12u], 8u, 8u)))))));
    out.out_attr0.z = exp2(spvFMul(2.2000000476837158203125, log2(abs(spvFMul(0.0039215688593685626983642578125, float(extract_bits(ssbo_1.data[12u], 16u, 8u)))))));
    float2 test_quad[4] = { float2(-0.8, -0.8), float2(-0.8, 0.8), float2(0.8, 0.8), float2(0.8, -0.8) };
    out.gl_Position = float4(test_quad[_81], 0.0, 1.0);
    return out;
}


)MSL";

constexpr const char* kShaderPsUIPanel = R"MSL(
#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T>
[[clang::optnone]] T spvFMul(T l, T r)
{
    return fma(l, r, T(0));
}

template<typename T, int Cols, int Rows>
[[clang::optnone]] vec<T, Cols> spvFMulVectorMatrix(vec<T, Rows> v, matrix<T, Cols, Rows> m)
{
    vec<T, Cols> res = vec<T, Cols>(0);
    for (uint i = Rows; i > 0; --i)
    {
        vec<T, Cols> tmp(0);
        for (uint j = 0; j < Cols; ++j)
        {
            tmp[j] = m[j][i - 1];
        }
        res = fma(tmp, vec<T, Cols>(v[i - 1]), res);
    }
    return res;
}

template<typename T, int Cols, int Rows>
[[clang::optnone]] vec<T, Rows> spvFMulMatrixVector(matrix<T, Cols, Rows> m, vec<T, Cols> v)
{
    vec<T, Rows> res = vec<T, Rows>(0);
    for (uint i = Cols; i > 0; --i)
    {
        res = fma(m[i - 1], vec<T, Rows>(v[i - 1]), res);
    }
    return res;
}

template<typename T, int LCols, int LRows, int RCols, int RRows>
[[clang::optnone]] matrix<T, RCols, LRows> spvFMulMatrixMatrix(matrix<T, LCols, LRows> l, matrix<T, RCols, RRows> r)
{
    static_assert(LCols == RRows, "column-row configuration mismatch");
    matrix<T, RCols, LRows> res;
    for (uint i = 0; i < RCols; i++)
    {
        vec<T, LRows> tmp(0);
        for (uint j = 0; j < LCols; j++)
        {
            tmp = fma(vec<T, LRows>(r[i][j]), l[j], tmp);
        }
        res[i] = tmp;
    }
    return res;
}

template<typename T>
[[clang::optnone]] T spvFAdd(T l, T r)
{
    return fma(T(1), l, r);
}

template<typename T>
[[clang::optnone]] T spvFSub(T l, T r)
{
    return fma(T(-1), r, l);
}

struct full_result_i32x2
{
    int _m0;
    int _m1;
};

struct full_result_u32x2
{
    uint _m0;
    uint _m1;
};

struct frexp_result_f32
{
    float _m0;
    int _m1;
};

struct AuxData
{
    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    uint4 buf_offsets0;
    uint4 buf_offsets1;
    uint2 buf_offsets2;
};

struct _56
{
    uint data[1];
};

constant uint _76 = {};
constant uint _77 = {};
constant uint _78 = {};

struct main0_out
{
    float4 frag_color0 [[color(0)]];
};

struct main0_in
{
    float4 fs_in_attr0 [[user(locn0), flat]];
    float4 fs_in_attr1 [[user(locn1)]];
    float4 fs_in_attr2 [[user(locn2)]];
};

fragment main0_out main0(main0_in in [[stage_in]], const device _56& ssbo_1 [[buffer(0)]], const device _56& ssbo_2 [[buffer(1)]], texture2d<float> fs_img0 [[texture(0)]], sampler fs_samp0 [[sampler(0)]])
{
    main0_out out = {};
    uint _86 = 1u & ssbo_1.data[13u];
    bool _87 = 0u != _86;
    uint _101;
    uint _102;
    uint _103;
    uint _104;
    if (_87)
    {
        float4 _92 = fs_img0.sample(fs_samp0, float2(in.fs_in_attr1.x, in.fs_in_attr1.y));
        _101 = as_type<uint>(_92.x);
        _102 = as_type<uint>(_92.y);
        _103 = as_type<uint>(_92.z);
        _104 = as_type<uint>(_92.w);
    }
    else
    {
        _101 = _86;
        _102 = _76;
        _103 = _77;
        _104 = _78;
    }
    uint _119;
    uint _120;
    uint _121;
    uint _122;
    if (!_87)
    {
        float4 _110 = fs_img0.sample(fs_samp0, float2(in.fs_in_attr1.x, in.fs_in_attr1.y));
        _119 = as_type<uint>(_110.x);
        _120 = as_type<uint>(_110.y);
        _121 = as_type<uint>(_110.z);
        _122 = as_type<uint>(_110.w);
    }
    else
    {
        _119 = _101;
        _120 = _102;
        _121 = _103;
        _122 = _104;
    }
    float _174 = spvFSub(1.0, fast::clamp(spvFMul(in.fs_in_attr2.y, fast::max(spvFAdd(-1.0, in.fs_in_attr1.w), -in.fs_in_attr1.w)), 0.0, 1.0));
    float _213 = spvFMul(spvFMul(spvFMul(spvFMul(spvFMul(spvFMul(in.fs_in_attr0.w, fast::clamp(spvFAdd(spvFMul(as_type<float>(ssbo_2.data[6u]), in.fs_in_attr2.w), as_type<float>(ssbo_2.data[7u])), 0.0, 1.0)), fast::clamp(spvFAdd(spvFMul(as_type<float>(ssbo_2.data[4u]), in.fs_in_attr2.w), as_type<float>(ssbo_2.data[5u])), 0.0, 1.0)), fast::clamp(spvFAdd(spvFMul(as_type<float>(ssbo_2.data[2u]), in.fs_in_attr2.z), as_type<float>(ssbo_2.data[3u])), 0.0, 1.0)), fast::clamp(spvFAdd(spvFMul(as_type<float>(ssbo_2.data[0u]), in.fs_in_attr2.z), as_type<float>(ssbo_2.data[1u])), 0.0, 1.0)), spvFAdd(spvFMul(-fast::clamp(spvFMul(in.fs_in_attr2.x, fast::max(spvFAdd(-1.0, in.fs_in_attr1.z), -in.fs_in_attr1.z)), 0.0, 1.0), _174), _174)), as_type<float>(_122));
    out.frag_color0 = float4(1.0, 0.5, 0.0, 1.0);
    return out;
}


)MSL";

constexpr const char* kShaderPsUIText = R"MSL(
#pragma clang diagnostic ignored "-Wmissing-prototypes"

#include <metal_stdlib>
#include <simd/simd.h>

using namespace metal;

template<typename T>
[[clang::optnone]] T spvFMul(T l, T r)
{
    return fma(l, r, T(0));
}

template<typename T, int Cols, int Rows>
[[clang::optnone]] vec<T, Cols> spvFMulVectorMatrix(vec<T, Rows> v, matrix<T, Cols, Rows> m)
{
    vec<T, Cols> res = vec<T, Cols>(0);
    for (uint i = Rows; i > 0; --i)
    {
        vec<T, Cols> tmp(0);
        for (uint j = 0; j < Cols; ++j)
        {
            tmp[j] = m[j][i - 1];
        }
        res = fma(tmp, vec<T, Cols>(v[i - 1]), res);
    }
    return res;
}

template<typename T, int Cols, int Rows>
[[clang::optnone]] vec<T, Rows> spvFMulMatrixVector(matrix<T, Cols, Rows> m, vec<T, Cols> v)
{
    vec<T, Rows> res = vec<T, Rows>(0);
    for (uint i = Cols; i > 0; --i)
    {
        res = fma(m[i - 1], vec<T, Rows>(v[i - 1]), res);
    }
    return res;
}

template<typename T, int LCols, int LRows, int RCols, int RRows>
[[clang::optnone]] matrix<T, RCols, LRows> spvFMulMatrixMatrix(matrix<T, LCols, LRows> l, matrix<T, RCols, RRows> r)
{
    static_assert(LCols == RRows, "column-row configuration mismatch");
    matrix<T, RCols, LRows> res;
    for (uint i = 0; i < RCols; i++)
    {
        vec<T, LRows> tmp(0);
        for (uint j = 0; j < LCols; j++)
        {
            tmp = fma(vec<T, LRows>(r[i][j]), l[j], tmp);
        }
        res[i] = tmp;
    }
    return res;
}

template<typename T>
[[clang::optnone]] T spvFAdd(T l, T r)
{
    return fma(T(1), l, r);
}

template<typename T>
[[clang::optnone]] T spvFSub(T l, T r)
{
    return fma(T(-1), r, l);
}

template<typename T>
inline T spvQuadBroadcast(T value, uint lane)
{
    return quad_broadcast(value, lane);
}

template<>
inline bool spvQuadBroadcast(bool value, uint lane)
{
    return !!quad_broadcast((ushort)value, lane);
}

template<uint N>
inline vec<bool, N> spvQuadBroadcast(vec<bool, N> value, uint lane)
{
    return (vec<bool, N>)quad_broadcast((vec<ushort, N>)value, lane);
}

struct full_result_i32x2
{
    int _m0;
    int _m1;
};

struct full_result_u32x2
{
    uint _m0;
    uint _m1;
};

struct frexp_result_f32
{
    float _m0;
    int _m1;
};

struct AuxData
{
    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    uint4 buf_offsets0;
    uint4 buf_offsets1;
    uint2 buf_offsets2;
};

struct _56
{
    uint data[1];
};

struct main0_out
{
    float4 frag_color0 [[color(0)]];
};

struct main0_in
{
    float4 fs_in_attr0 [[user(locn0), flat]];
    float4 fs_in_attr1 [[user(locn1)]];
    float4 fs_in_attr2 [[user(locn2)]];
};

fragment main0_out main0(main0_in in [[stage_in]], const device _56& ssbo_1 [[buffer(0)]], const device _56& ssbo_2 [[buffer(1)]], texture2d<float> fs_img0 [[texture(0)]], sampler fs_samp0 [[sampler(0)]])
{
    main0_out out = {};
    uint _76 = as_type<uint>(in.fs_in_attr1.x);
    uint _79 = as_type<uint>(in.fs_in_attr1.y);
    uint _81 = spvQuadBroadcast(_76, 0u);
    uint _82 = spvQuadBroadcast(_76, 1u);
    uint _83 = spvQuadBroadcast(_79, 1u);
    uint _84 = spvQuadBroadcast(_79, 0u);
    uint _86 = spvQuadBroadcast(_76, 2u);
    uint _87 = spvQuadBroadcast(_79, 2u);
    float _90 = spvFSub(as_type<float>(_82), as_type<float>(_81));
    float _96 = fast::clamp(spvFAdd(spvFMul(1356.2913818359375, _90), -1.0), 0.0, 1.0);
    float4 _101 = fs_img0.sample(fs_samp0, float2(in.fs_in_attr1.x, in.fs_in_attr1.y));
    float _102 = _101.x;
    float _280;
    if (0.0 < _96)
    {
        float _108 = as_type<float>(_84);
        float _112 = spvFMul(_96, spvFSub(as_type<float>(_86), as_type<float>(_81)));
        float _113 = spvFMul(_96, spvFSub(as_type<float>(_87), _108));
        float _114 = spvFMul(_96, _90);
        float _115 = spvFMul(_96, spvFSub(as_type<float>(_83), _108));
        float _118 = spvFAdd(spvFMul(_112, 0.3499999940395355224609375), in.fs_in_attr1.x);
        float _120 = spvFAdd(spvFMul(_113, 0.3499999940395355224609375), in.fs_in_attr1.y);
        float _149 = spvFAdd(spvFMul(0.3499999940395355224609375, -_112), in.fs_in_attr1.x);
        float _156 = spvFAdd(spvFMul(0.3499999940395355224609375, -_113), in.fs_in_attr1.y);
        uint4 _179 = uint4(ssbo_1.data[4u], ssbo_1.data[4u + 1u], ssbo_1.data[4u + 2u], ssbo_1.data[4u + 3u]);
        float _185 = as_type<float>(_179.x);
        float _186 = spvFAdd(as_type<float>(_179.z), _185);
        float _188 = as_type<float>(_179.y);
        float _189 = spvFAdd(as_type<float>(_179.w), _188);
        float _278 = spvFMul(spvFAdd(spvFAdd(spvFAdd(spvFAdd(fs_img0.sample(fs_samp0, float2(fast::max(fast::min(spvFAdd(spvFMul(_114, 0.5), in.fs_in_attr1.x), _186), _185), fast::max(fast::min(spvFAdd(spvFMul(_115, 0.5), in.fs_in_attr1.y), _189), _188))).x, fs_img0.sample(fs_samp0, float2(fast::max(fast::min(spvFAdd(spvFMul(-0.5, _114), in.fs_in_attr1.x), _186), _185), fast::max(fast::min(spvFAdd(spvFMul(-0.5, _115), in.fs_in_attr1.y), _189), _188))).x), fs_img0.sample(fs_samp0, float2(fast::max(fast::min(spvFAdd(spvFMul(_112, 0.5), in.fs_in_attr1.x), _186), _185), fast::max(fast::min(spvFAdd(spvFMul(_113, 0.5), in.fs_in_attr1.y), _189), _188))).x), fs_img0.sample(fs_samp0, float2(fast::max(fast::min(spvFAdd(spvFMul(-0.5, _112), in.fs_in_attr1.x), _186), _185), fast::max(fast::min(spvFAdd(spvFMul(-0.5, _113), in.fs_in_attr1.y), _189), _188))).x), spvFAdd(spvFAdd(spvFAdd(fs_img0.sample(fs_samp0, float2(fast::max(fast::min(spvFAdd(spvFMul(_114, 0.3499999940395355224609375), _118), _186), _185), fast::max(fast::min(spvFAdd(spvFMul(_115, 0.3499999940395355224609375), _120), _189), _188))).x, fs_img0.sample(fs_samp0, float2(fast::max(fast::min(spvFAdd(spvFMul(0.3499999940395355224609375, -_114), _118), _186), _185), fast::max(fast::min(spvFAdd(spvFMul(0.3499999940395355224609375, -_115), _120), _189), _188))).x), fs_img0.sample(fs_samp0, float2(fast::max(fast::min(spvFAdd(spvFMul(_114, 0.3499999940395355224609375), _149), _186), _185), fast::max(fast::min(spvFAdd(spvFMul(_115, 0.3499999940395355224609375), _156), _189), _188))).x), fs_img0.sample(fs_samp0, float2(fast::max(fast::min(spvFAdd(spvFMul(0.3499999940395355224609375, -_114), _149), _186), _185), fast::max(fast::min(spvFAdd(spvFMul(0.3499999940395355224609375, -_115), _156), _189), _188))).x)), 0.100000001490116119384765625);
        _280 = spvFAdd(_278, spvFMul(0.20000000298023223876953125, _102));
    }
    else
    {
        _280 = _102;
    }
    float _328 = spvFSub(1.0, fast::clamp(spvFMul(in.fs_in_attr2.y, fast::max(spvFAdd(-1.0, in.fs_in_attr1.w), -in.fs_in_attr1.w)), 0.0, 1.0));
    float _353 = spvFMul(spvFMul(spvFMul(spvFMul(spvFMul(spvFMul(in.fs_in_attr0.w, fast::clamp(spvFAdd(spvFMul(as_type<float>(ssbo_2.data[6u]), in.fs_in_attr2.w), as_type<float>(ssbo_2.data[7u])), 0.0, 1.0)), fast::clamp(spvFAdd(spvFMul(as_type<float>(ssbo_2.data[4u]), in.fs_in_attr2.w), as_type<float>(ssbo_2.data[5u])), 0.0, 1.0)), fast::clamp(spvFAdd(spvFMul(as_type<float>(ssbo_2.data[2u]), in.fs_in_attr2.z), as_type<float>(ssbo_2.data[3u])), 0.0, 1.0)), fast::clamp(spvFAdd(spvFMul(as_type<float>(ssbo_2.data[0u]), in.fs_in_attr2.z), as_type<float>(ssbo_2.data[1u])), 0.0, 1.0)), spvFAdd(spvFMul(_328, -fast::clamp(spvFMul(in.fs_in_attr2.x, fast::max(spvFAdd(-1.0, in.fs_in_attr1.z), -in.fs_in_attr1.z)), 0.0, 1.0)), _328)), _280);
    out.frag_color0 = float4(0.0, 1.0, 1.0, 1.0);
    return out;
}


)MSL";

} // namespace Gpu::Metal::EmbeddedShaders
