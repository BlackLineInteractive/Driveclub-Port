#pragma once

// A decoded guest shader plus what the backend needs to know about it:
// which user-data words feed its resource descriptors, what it exports,
// which PS inputs it reads.
//
// Descriptors (T#, S#) live in scalar registers that the shader fills from
// user data or loads from memory pointed to by user data. The host API has to
// bind textures before the draw, so each image instruction records where its
// descriptor words came from ("provenance"); the renderer evaluates that
// against the draw's user data to find the actual texture.

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "gpu/shader/gcn.h"

namespace Gpu::Shader {

enum class Stage : u8 { Vertex, Fragment, Compute };

struct SgprSource {
    enum Kind : u8 {
        Unknown,
        UserData,    // user SGPR `ud`
        PointerLoad, // dword at (64-bit pointer in user SGPRs ud, ud+1) + offset
        BufferLoad,  // dword at V#(user SGPRs ud..ud+3).base + offset
    };
    Kind kind = Unknown;
    u8 ud = 0;
    u32 offset = 0; // bytes

    bool operator==(const SgprSource&) const = default;
};

struct ImageResource {
    std::array<SgprSource, 8> tsharp{};
    std::array<SgprSource, 4> ssharp{};
    bool has_sampler = false;
    bool storage = false; // image_store / atomics
    bool compare = false; // depth-compare sampling
};

struct Program {
    Stage stage = Stage::Vertex;
    u64 address = 0;      // guest address of the main program
    u64 fetch_address = 0; // inlined fetch subroutine, if any
    u64 hash = 0;         // of the code bytes (plus fetch code)
    u32 num_user_sgprs = 0;
    u32 vgpr_comp_cnt = 0; // VS: how many system VGPRs follow the vertex index

    std::vector<Gcn::Inst> code;
    std::vector<u32> labels; // byte offsets that need a case label (sorted)
    bool has_branches = false;
    bool writes_exec = false;

    // Image instruction pc -> index into `images`.
    std::vector<std::pair<u32, u32>> image_slot_by_pc;
    std::vector<ImageResource> images;

    // VS exports.
    u32 param_mask = 0;
    bool exports_position = false;
    // PS usage.
    u32 attr_mask = 0;      // PS attributes read by v_interp_*
    u32 attr_flat_mask = 0; // ... of which read through v_interp_mov
    u8 mrt_mask = 0;
    bool exports_depth = false;
    bool uses_quad_ops = false;

    std::vector<std::string> unsupported; // opcodes we could not translate

    u32 ImageSlot(u32 pc) const;
};

struct BuildInput {
    Stage stage;
    u64 address;
    const u32* user_data; // the stage's 16 user-data registers
    u32 num_user_sgprs;
    u32 vgpr_comp_cnt;
};

// Decodes and analyzes a program. Returns null if the code is unreadable.
std::unique_ptr<Program> BuildProgram(const BuildInput& in);

// Evaluates a descriptor dword at draw time.
u32 ResolveSgpr(const SgprSource& src, const u32* user_data);

} // namespace Gpu::Shader
