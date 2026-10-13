#pragma once

// GCN -> Metal Shading Language translation.
//
// Every GCN wave lane becomes one Metal invocation. Scalar and vector
// registers turn into local variables; 64-bit lane masks (EXEC, VCC, v_cmp
// results) are kept per lane as all-ones / all-zero words, so the bitwise
// scalar ops that combine masks work unchanged. Guest memory is reached
// through two Metal buffers that alias the whole direct-memory object, with a
// small range table translating guest virtual addresses to offsets.

#include <array>
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
} // namespace Binding

// Host-side mirrors of the MSL structs in the prelude.
struct alignas(8) GpuRange {
    u64 start;
    u64 size;
    u64 phys;
};
struct alignas(8) GpuMemMap {
    static constexpr u32 kMaxRanges = 32;
    u32 count;
    u32 pad;
    u64 split;
    GpuRange ranges[kMaxRanges];
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
    bool operator==(const TextureType&) const = default;
};

enum class OutType : u8 { Float, Uint, Sint };

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
    // Both.
    std::vector<TextureType> textures; // per Program::images slot
};

// Returns the MSL source; the entry point is "main0". Opcodes that could not
// be translated are appended to `unsupported`.
std::string GenerateMsl(const Program& p, const MslOptions& o, std::vector<std::string>* unsupported);

} // namespace Gpu::Shader
