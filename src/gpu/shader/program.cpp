#include "gpu/shader/program.h"

#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "core/memory.h"

namespace Gpu::Shader {

using namespace Gcn;

namespace {

// Fetch-subroutine instructions get pcs in their own range so they never
// collide with branch targets of the main program.
constexpr u32 kFetchPcBase = 0x100000;

bool Readable(u64 addr, u64 size) {
    Core::Memory::Vma vma;
    return addr && Core::Memory::Query(addr, false, &vma) && addr + size <= vma.start + vma.size;
}

u64 HashCode(const std::vector<Inst>& insts, const u32* base, u64 seed) {
    // FNV-1a over the instruction words.
    u64 h = seed ^ 0xcbf29ce484222325ull;
    for (const Inst& i : insts) {
        for (u32 k = 0; k < i.size; ++k) {
            h ^= base[i.pc / 4 + k];
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

bool IsSgpr(u16 code) {
    return code <= Operand::SgprMax;
}

// Tracks where each scalar register's value came from.
class Provenance {
public:
    explicit Provenance(u32 num_user_sgprs) {
        for (u32 i = 0; i < num_user_sgprs && i < state.size(); ++i) {
            state[i] = {SgprSource::UserData, static_cast<u8>(i), 0};
        }
    }

    const SgprSource& Get(u32 reg) const {
        static const SgprSource unknown{};
        return reg < state.size() ? state[reg] : unknown;
    }

    void Clobber(u32 reg, u32 count = 1) {
        for (u32 k = 0; k < count; ++k) {
            if (reg + k < state.size()) {
                state[reg + k] = {};
            }
        }
    }

    void Step(const Inst& i) {
        switch (i.enc) {
        case Enc::SMRD: {
            const auto op = static_cast<Smrd>(i.op);
            u32 count = 0;
            bool buffer = false;
            switch (op) {
            case Smrd::S_LOAD_DWORD: count = 1; break;
            case Smrd::S_LOAD_DWORDX2: count = 2; break;
            case Smrd::S_LOAD_DWORDX4: count = 4; break;
            case Smrd::S_LOAD_DWORDX8: count = 8; break;
            case Smrd::S_LOAD_DWORDX16: count = 16; break;
            case Smrd::S_BUFFER_LOAD_DWORD: count = 1; buffer = true; break;
            case Smrd::S_BUFFER_LOAD_DWORDX2: count = 2; buffer = true; break;
            case Smrd::S_BUFFER_LOAD_DWORDX4: count = 4; buffer = true; break;
            case Smrd::S_BUFFER_LOAD_DWORDX8: count = 8; buffer = true; break;
            case Smrd::S_BUFFER_LOAD_DWORDX16: count = 16; buffer = true; break;
            default: return;
            }
            const SgprSource& lo = Get(i.sbase);
            const SgprSource& hi = Get(i.sbase + 1u);
            const bool base_is_ud = lo.kind == SgprSource::UserData &&
                                    hi.kind == SgprSource::UserData && hi.ud == lo.ud + 1;
            for (u32 k = 0; k < count; ++k) {
                SgprSource s{};
                if (base_is_ud && i.imm) {
                    s.kind = buffer ? SgprSource::BufferLoad : SgprSource::PointerLoad;
                    s.ud = lo.ud;
                    s.offset = i.smrd_offset * 4 + k * 4;
                }
                if (i.dst + k < state.size()) {
                    state[i.dst + k] = s;
                }
            }
            return;
        }
        case Enc::SOP1: {
            const auto op = static_cast<Sop1>(i.op);
            if (op == Sop1::S_MOV_B32 && IsSgpr(i.src[0])) {
                Set(i.dst, Get(i.src[0]));
                return;
            }
            if (op == Sop1::S_MOV_B64 && IsSgpr(i.src[0])) {
                const SgprSource a = Get(i.src[0]), b = Get(i.src[0] + 1u);
                Set(i.dst, a);
                Set(i.dst + 1u, b);
                return;
            }
            Clobber(i.dst, 2);
            return;
        }
        case Enc::SOP2:
        case Enc::SOPK:
            Clobber(i.dst, 2);
            return;
        case Enc::VOPC:
        case Enc::VOP2:
        case Enc::VOP1:
        case Enc::VOP3:
            if (i.op < 256 || i.vop3) {
                if (i.sdst <= Operand::SgprMax) {
                    Clobber(i.sdst, 2);
                }
            }
            if (static_cast<Vop>(i.op) == Vop::V_READFIRSTLANE_B32 ||
                static_cast<Vop>(i.op) == Vop::V_READLANE_B32) {
                Clobber(i.dst);
            }
            return;
        default:
            return;
        }
    }

private:
    void Set(u32 reg, const SgprSource& s) {
        if (reg < state.size()) {
            state[reg] = s;
        }
    }

    std::array<SgprSource, 104> state{};
};

bool WritesExec(const Inst& i) {
    auto is_exec = [](u16 code) { return code == Operand::ExecLo || code == Operand::ExecHi; };
    switch (i.enc) {
    case Enc::SOP2:
    case Enc::SOPK:
        return is_exec(i.dst);
    case Enc::SOP1: {
        const auto op = static_cast<Sop1>(i.op);
        if (op >= Sop1::S_AND_SAVEEXEC_B64 && op <= Sop1::S_XNOR_SAVEEXEC_B64) {
            return true;
        }
        return is_exec(i.dst);
    }
    case Enc::VOPC:
    case Enc::VOP3:
        if (i.op < 256) {
            const u16 base = i.op < 0x80 ? (i.op & 0x70) : (i.op & 0xF0);
            // V_CMPX_* families: 0x10, 0x30, 0x50, 0x70, 0x90, 0xB0, 0xD0, 0xF0.
            return (base & 0x10) != 0 || is_exec(i.sdst);
        }
        return i.vop3 && is_exec(i.sdst);
    default:
        return false;
    }
}

bool IsImageSample(u16 op) {
    return op >= static_cast<u16>(Mimg::IMAGE_SAMPLE) && op <= static_cast<u16>(Mimg::IMAGE_GET_LOD);
}

bool IsImageCompare(u16 op) {
    switch (static_cast<Mimg>(op)) {
    case Mimg::IMAGE_SAMPLE_C: case Mimg::IMAGE_SAMPLE_C_CL: case Mimg::IMAGE_SAMPLE_C_D:
    case Mimg::IMAGE_SAMPLE_C_D_CL: case Mimg::IMAGE_SAMPLE_C_L: case Mimg::IMAGE_SAMPLE_C_B:
    case Mimg::IMAGE_SAMPLE_C_B_CL: case Mimg::IMAGE_SAMPLE_C_LZ: case Mimg::IMAGE_SAMPLE_C_O:
    case Mimg::IMAGE_SAMPLE_C_CL_O: case Mimg::IMAGE_SAMPLE_C_D_O: case Mimg::IMAGE_SAMPLE_C_D_CL_O:
    case Mimg::IMAGE_SAMPLE_C_L_O: case Mimg::IMAGE_SAMPLE_C_B_O: case Mimg::IMAGE_SAMPLE_C_B_CL_O:
    case Mimg::IMAGE_SAMPLE_C_LZ_O: case Mimg::IMAGE_GATHER4_C: case Mimg::IMAGE_GATHER4_C_CL:
    case Mimg::IMAGE_GATHER4_C_L: case Mimg::IMAGE_GATHER4_C_B: case Mimg::IMAGE_GATHER4_C_B_CL:
    case Mimg::IMAGE_GATHER4_C_LZ: case Mimg::IMAGE_GATHER4_C_LZ_O:
        return true;
    default:
        return false;
    }
}

// Replaces S_SWAPPC_B64 (call into the fetch subroutine) with the subroutine's
// code. The callee address comes from user data, so it is known per draw.
bool InlineFetch(Program& p, const BuildInput& in) {
    for (size_t idx = 0; idx < p.code.size(); ++idx) {
        const Inst& call = p.code[idx];
        if (call.enc != Enc::SOP1 || static_cast<Sop1>(call.op) != Sop1::S_SWAPPC_B64) {
            continue;
        }
        Provenance prov(in.num_user_sgprs);
        for (size_t k = 0; k < idx; ++k) {
            prov.Step(p.code[k]);
        }
        const SgprSource lo = prov.Get(call.src[0]);
        const SgprSource hi = prov.Get(call.src[0] + 1u);
        if (lo.kind != SgprSource::UserData || hi.kind != SgprSource::UserData) {
            DC_WARN("shader %#llx: fetch subroutine address is not in user data", p.address);
            return false;
        }
        const u64 target = u64(in.user_data[lo.ud]) | (u64(in.user_data[hi.ud]) << 32);
        if (!Readable(target, 16)) {
            DC_WARN("shader %#llx: fetch subroutine %#llx is not mapped", p.address, target);
            return false;
        }
        const u32* fcode = reinterpret_cast<const u32*>(target);
        auto fetch = DecodeProgram(fcode, 4096, true);
        if (!fetch.empty() && fetch.back().enc == Enc::SOP1 &&
            static_cast<Sop1>(fetch.back().op) == Sop1::S_SETPC_B64) {
            fetch.pop_back();
        }
        p.hash = HashCode(fetch, fcode, p.hash);
        for (Inst& f : fetch) {
            f.pc += kFetchPcBase;
        }
        p.fetch_address = target;
        p.fetch_ud = lo.ud;
        p.code.erase(p.code.begin() + static_cast<long>(idx));
        p.code.insert(p.code.begin() + static_cast<long>(idx), fetch.begin(), fetch.end());
        return true;
    }
    return true;
}

} // namespace

u32 Program::ImageSlot(u32 pc) const {
    for (const auto& [ipc, slot] : image_slot_by_pc) {
        if (ipc == pc) {
            return slot;
        }
    }
    return 0;
}

std::unique_ptr<Program> BuildProgram(const BuildInput& in) {
    if (!in.code && !Readable(in.address, 64)) {
        return nullptr;
    }
    auto p = std::make_unique<Program>();
    p->stage = in.stage;
    p->address = in.address;
    p->num_user_sgprs = in.num_user_sgprs;
    p->vgpr_comp_cnt = in.vgpr_comp_cnt;

    const u32* code = in.code ? in.code : reinterpret_cast<const u32*>(in.address);
    p->code = DecodeProgram(code);
    if (p->code.empty()) {
        return nullptr;
    }
    p->hash = HashCode(p->code, code, static_cast<u64>(in.stage));
    if (!InlineFetch(*p, in)) {
        return nullptr;
    }

    Provenance prov(in.num_user_sgprs);
    for (const Inst& i : p->code) {
        if (IsBranch(i)) {
            p->has_branches = true;
            p->labels.push_back(BranchTarget(i));
        }
        p->writes_exec |= WritesExec(i);

        switch (i.enc) {
        case Enc::EXP:
            if (i.target >= ExpTarget::Param0) {
                p->param_mask |= 1u << (i.target - ExpTarget::Param0);
            } else if (i.target == ExpTarget::Pos0) {
                p->exports_position = true;
            } else if (i.target < 8) {
                p->mrt_mask |= static_cast<u8>(1u << i.target);
            } else if (i.target == ExpTarget::MrtZ) {
                p->exports_depth = true;
            }
            break;
        case Enc::VINTRP:
            p->attr_mask |= 1u << i.attr;
            if (static_cast<Vintrp>(i.op) == Vintrp::V_INTERP_MOV_F32) {
                p->attr_flat_mask |= 1u << i.attr;
            }
            break;
        case Enc::DS:
            if (static_cast<Ds>(i.op) == Ds::DS_SWIZZLE_B32) {
                p->uses_quad_ops = true;
            }
            break;
        case Enc::MIMG: {
            ImageResource r;
            for (u32 k = 0; k < 8; ++k) {
                r.tsharp[k] = prov.Get(i.srsrc + k);
            }
            r.has_sampler = IsImageSample(i.op);
            if (r.has_sampler) {
                for (u32 k = 0; k < 4; ++k) {
                    r.ssharp[k] = prov.Get(i.ssamp + k);
                }
            }
            r.storage = i.op >= static_cast<u16>(Mimg::IMAGE_STORE) &&
                        i.op < static_cast<u16>(Mimg::IMAGE_SAMPLE) &&
                        static_cast<Mimg>(i.op) != Mimg::IMAGE_GET_RESINFO;
            r.compare = IsImageCompare(i.op);
            u32 slot = 0;
            for (; slot < p->images.size(); ++slot) {
                const ImageResource& e = p->images[slot];
                if (e.tsharp == r.tsharp && e.ssharp == r.ssharp) {
                    break;
                }
            }
            if (slot == p->images.size()) {
                p->images.push_back(r);
            } else {
                p->images[slot].storage |= r.storage;
                p->images[slot].compare |= r.compare;
                p->images[slot].has_sampler |= r.has_sampler;
            }
            p->image_slot_by_pc.emplace_back(i.pc, slot);
            break;
        }
        default:
            break;
        }
        prov.Step(i);
    }
    std::sort(p->labels.begin(), p->labels.end());
    p->labels.erase(std::unique(p->labels.begin(), p->labels.end()), p->labels.end());
    return p;
}

u32 ResolveSgpr(const SgprSource& src, const u32* ud) {
    switch (src.kind) {
    case SgprSource::UserData:
        return ud[src.ud];
    case SgprSource::PointerLoad: {
        const u64 base = (u64(ud[src.ud]) | (u64(ud[src.ud + 1]) << 32)) & 0xFFFFFFFFFFFFull;
        const u64 addr = base + src.offset;
        if (!Readable(addr, 4)) {
            return 0;
        }
        u32 v;
        std::memcpy(&v, reinterpret_cast<const void*>(addr), 4);
        return v;
    }
    case SgprSource::BufferLoad: {
        const u64 base = u64(ud[src.ud]) | (u64(ud[src.ud + 1] & 0xFFFF) << 32);
        const u64 addr = base + src.offset;
        if (!Readable(addr, 4)) {
            return 0;
        }
        u32 v;
        std::memcpy(&v, reinterpret_cast<const void*>(addr), 4);
        return v;
    }
    default:
        return 0;
    }
}

} // namespace Gpu::Shader
