#pragma once

// GCN -> Metal Shading Language translation.
//
// Every GCN wave lane becomes one Metal invocation. Scalar and vector
// registers turn into local variables; 64-bit lane masks (EXEC, VCC, v_cmp
// results) are kept per lane as all-ones / all-zero words, so the bitwise
// scalar ops that combine masks work unchanged. Guest memory is reached
// through two Metal buffers that alias the whole direct-memory object, with a
// page table translating guest virtual addresses to offsets.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "common/types.h"
#include "gpu/shader/program.h"

namespace Gpu::Shader {

// Fixed resource bindings shared by every generated shader.
namespace Binding {
constexpr u32 UserData = 0; // constant uint[16]
constexpr u32 Mem0 = 1;     // device uint*: direct memory [0, split)
constexpr u32 Mem1 = 2;     // device uint*: direct memory [split, end)
constexpr u32 MemMap = 3;   // constant DcMap
constexpr u32 Draw = 4;     // constant DcDraw (vertex stage)
constexpr u32 Gds = 5;      // device uint[16K]: global data share (compute stage)
} // namespace Binding

// Host-side mirrors of the MSL structs in the prelude.
// Header of the page table bound at Binding::MemMap; `pages` s32 entries
// follow it, each the page's (phys - va) in 16 KB units, or kUnmapped.
struct GpuMemMap {
    static constexpr u32 kMinShift = 14;
    static constexpr u32 kMaxShift = 21;
    static constexpr s32 kUnmapped = INT32_MIN;
    u64 base;
    u64 pages;
    u64 split;
    u32 shift;
    u32 pad;
    u64 limit; // physical size
};
struct alignas(16) GpuDrawConsts {
    float vp_scale[4];  // viewport scale (1 where the transform is disabled)
    float vp_offset[4]; // viewport offset (0 where disabled)
    float rt_scale[2];  // 2 / render target size
    u32 vte;            // PA_CL_VTE_CNTL
    u32 pad;
};

enum class TexDim : u8 { Tex2D, Tex2DArray, Tex3D };
enum class TexSample : u8 { Float, Uint, Sint };

struct TextureType {
    TexDim dim = TexDim::Tex2D;
    TexSample sample = TexSample::Float;
    u8 compare = 3; // depth-compare sampling: S# DEPTH_COMPARE_FUNC (ref <op> texel), default LEQUAL
    bool cube = false; // a cube (Tex2DArray of faces) sampled with V_CUBE* coordinates
    bool operator==(const TextureType&) const = default;
};

enum class OutType : u8 { Float, Uint, Sint };

constexpr u32 kProbeWords = 256;
constexpr u32 kProbeBinding = 7;

struct MslOptions {
    // Vertex.
    bool rect_list = false; // expand RECTLIST: 3 vertices -> 2 triangles
    bool debug_capture = false; // write positions to buffer(6) (debugging)
    // Fragment.
    std::array<u32, 32> ps_input_cntl{}; // SPI_PS_INPUT_CNTL_n
    u32 vs_param_mask = 0;               // params the paired VS exports
    u32 ps_input_addr = 0;               // SPI_PS_INPUT_ADDR
    u32 col_format = 0;                  // SPI_SHADER_COL_FORMAT
    std::array<OutType, 8> mrt_type{};
    bool depth_target = true; // false drops depth exports (no depth attachment)
    // Register probe (debugging): before the instruction at `probe_pc`, the
    // fragment at pixel (probe_x, probe_y) -- for compute, the lane with
    // threadgroup x == probe_x and thread index == probe_y -- writes its
    // registers to buffer(7): [0] hit, [1] exec, [2] vcc, [3] m0, [8 + n] vN
    // (n < 96), [104 + n] sN (n < 104). kProbeWords per slot.
    u32 probe_pc = ~0u;
    u32 probe_x = 0, probe_y = 0;
    // Both.
    std::vector<TextureType> textures; // per Program::images slot
};

// Returns the MSL source; the entry point is "main0". Opcodes that could not
// be translated are appended to `unsupported`.
std::string GenerateMsl(const Program& p, const MslOptions& o, std::vector<std::string>* unsupported);

} // namespace Gpu::Shader
