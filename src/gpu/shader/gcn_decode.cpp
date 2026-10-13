#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_map>

#include "gpu/shader/gcn.h"

namespace Gpu::Shader::Gcn {

namespace {

constexpr u32 Bits(u32 v, u32 lo, u32 n) {
    return (v >> lo) & ((1u << n) - 1);
}

Enc Classify(u32 w) {
    if ((w >> 23) == 0x17D) return Enc::SOP1;
    if ((w >> 23) == 0x17E) return Enc::SOPC;
    if ((w >> 23) == 0x17F) return Enc::SOPP;
    if ((w >> 28) == 0xB) return Enc::SOPK;
    if ((w >> 30) == 0x2) return Enc::SOP2;
    if ((w >> 27) == 0x18) return Enc::SMRD;
    if ((w >> 25) == 0x3F) return Enc::VOP1;
    if ((w >> 25) == 0x3E) return Enc::VOPC;
    if ((w >> 31) == 0) return Enc::VOP2;
    switch (w >> 26) {
    case 0x34: return Enc::VOP3;
    case 0x32: return Enc::VINTRP;
    case 0x36: return Enc::DS;
    case 0x38: return Enc::MUBUF;
    case 0x3A: return Enc::MTBUF;
    case 0x3C: return Enc::MIMG;
    case 0x3E: return Enc::EXP;
    default: return Enc::Unknown;
    }
}

bool IsVop3b(u16 op) {
    switch (static_cast<Vop>(op)) {
    case Vop::V_ADD_I32:
    case Vop::V_SUB_I32:
    case Vop::V_SUBREV_I32:
    case Vop::V_ADDC_U32:
    case Vop::V_SUBB_U32:
    case Vop::V_SUBBREV_U32:
    case Vop::V_DIV_SCALE_F32:
    case Vop::V_DIV_SCALE_F64:
    case Vop::V_MAD_U64_U32:
    case Vop::V_MAD_I64_I32:
        return true;
    default:
        return false;
    }
}

// Pulls in a trailing literal if any 32-bit-encoding source asks for one.
void TakeLiteral(const u32* code, Inst& i, int nsrc) {
    for (int s = 0; s < nsrc; ++s) {
        if (i.src[s] == Operand::Literal) {
            i.literal = code[1];
            i.size = 2;
            return;
        }
    }
}

} // namespace

bool Decode(const u32* code, u32 pc, Inst& i) {
    i = Inst{};
    i.pc = pc;
    const u32 w = code[0];
    i.enc = Classify(w);
    switch (i.enc) {
    case Enc::SOP2:
        i.op = Bits(w, 23, 7);
        i.dst = Bits(w, 16, 7);
        i.src[0] = Bits(w, 0, 8);
        i.src[1] = Bits(w, 8, 8);
        TakeLiteral(code, i, 2);
        return true;
    case Enc::SOPK:
        i.op = Bits(w, 23, 5);
        i.dst = Bits(w, 16, 7);
        i.simm16 = static_cast<s16>(Bits(w, 0, 16));
        if (static_cast<Sopk>(i.op) == Sopk::S_SETREG_IMM32_B32) {
            i.literal = code[1];
            i.size = 2;
        }
        return true;
    case Enc::SOP1:
        i.op = Bits(w, 8, 8);
        i.dst = Bits(w, 16, 7);
        i.src[0] = Bits(w, 0, 8);
        TakeLiteral(code, i, 1);
        return true;
    case Enc::SOPC:
        i.op = Bits(w, 16, 7);
        i.src[0] = Bits(w, 0, 8);
        i.src[1] = Bits(w, 8, 8);
        TakeLiteral(code, i, 2);
        return true;
    case Enc::SOPP:
        i.op = Bits(w, 16, 7);
        i.simm16 = static_cast<s16>(Bits(w, 0, 16));
        return true;
    case Enc::SMRD:
        i.op = Bits(w, 22, 5);
        i.dst = Bits(w, 15, 7);
        i.sbase = static_cast<u8>(Bits(w, 9, 6) * 2);
        i.imm = Bits(w, 8, 1);
        i.smrd_offset = Bits(w, 0, 8);
        if (!i.imm && i.smrd_offset == Operand::Literal) {
            i.imm = true;
            i.smrd_offset = code[1];
            i.size = 2;
        }
        return true;
    case Enc::VOP2:
        i.op = static_cast<u16>(256 + Bits(w, 25, 6));
        i.dst = Bits(w, 17, 8);
        i.src[0] = Bits(w, 0, 9);
        i.src[1] = static_cast<u16>(Operand::Vgpr0 + Bits(w, 9, 8));
        i.sdst = Operand::VccLo;
        switch (static_cast<Vop>(i.op)) {
        case Vop::V_CNDMASK_B32:
        case Vop::V_ADDC_U32:
        case Vop::V_SUBB_U32:
        case Vop::V_SUBBREV_U32:
            i.src[2] = Operand::VccLo;
            break;
        case Vop::V_MADMK_F32:
        case Vop::V_MADAK_F32:
            i.literal = code[1];
            i.size = 2;
            break;
        default:
            break;
        }
        if (i.size == 1) {
            TakeLiteral(code, i, 1);
        }
        return true;
    case Enc::VOP1:
        i.op = static_cast<u16>(384 + Bits(w, 9, 8));
        i.dst = Bits(w, 17, 8);
        i.src[0] = Bits(w, 0, 9);
        TakeLiteral(code, i, 1);
        return true;
    case Enc::VOPC:
        i.op = Bits(w, 17, 8);
        i.sdst = Operand::VccLo;
        i.src[0] = Bits(w, 0, 9);
        i.src[1] = static_cast<u16>(Operand::Vgpr0 + Bits(w, 9, 8));
        TakeLiteral(code, i, 1);
        return true;
    case Enc::VOP3: {
        const u32 w1 = code[1];
        i.size = 2;
        i.vop3 = true;
        i.op = Bits(w, 17, 9);
        i.src[0] = Bits(w1, 0, 9);
        i.src[1] = Bits(w1, 9, 9);
        i.src[2] = Bits(w1, 18, 9);
        i.omod = Bits(w1, 27, 2);
        i.neg = Bits(w1, 29, 3);
        if (i.op < 256) { // VOPC: the 8-bit dst field is the scalar mask destination
            i.sdst = Bits(w, 0, 8);
            i.abs = Bits(w, 8, 3);
            i.clamp = Bits(w, 11, 1);
        } else if (IsVop3b(i.op)) {
            i.dst = Bits(w, 0, 8);
            i.sdst = Bits(w, 8, 7);
        } else {
            i.dst = Bits(w, 0, 8);
            i.abs = Bits(w, 8, 3);
            i.clamp = Bits(w, 11, 1);
        }
        return true;
    }
    case Enc::VINTRP:
        i.src[0] = static_cast<u16>(Operand::Vgpr0 + Bits(w, 0, 8));
        i.chan = Bits(w, 8, 2);
        i.attr = Bits(w, 10, 6);
        i.op = Bits(w, 16, 2);
        i.dst = Bits(w, 18, 8);
        return true;
    case Enc::DS: {
        const u32 w1 = code[1];
        i.size = 2;
        i.offset0 = Bits(w, 0, 8);
        i.offset1 = Bits(w, 8, 8);
        i.gds = Bits(w, 17, 1);
        i.op = Bits(w, 18, 8);
        i.vaddr = Bits(w1, 0, 8);
        i.data0 = Bits(w1, 8, 8);
        i.data1 = Bits(w1, 16, 8);
        i.dst = Bits(w1, 24, 8);
        return true;
    }
    case Enc::MUBUF:
    case Enc::MTBUF: {
        const u32 w1 = code[1];
        i.size = 2;
        i.offset = Bits(w, 0, 12);
        i.offen = Bits(w, 12, 1);
        i.idxen = Bits(w, 13, 1);
        i.glc = Bits(w, 14, 1);
        i.addr64 = Bits(w, 15, 1);
        if (i.enc == Enc::MUBUF) {
            i.lds = Bits(w, 16, 1);
            i.op = Bits(w, 18, 7);
        } else {
            i.op = Bits(w, 16, 3);
            i.dfmt = Bits(w, 19, 4);
            i.nfmt = Bits(w, 23, 3);
        }
        i.vaddr = Bits(w1, 0, 8);
        i.vdata = Bits(w1, 8, 8);
        i.srsrc = static_cast<u8>(Bits(w1, 16, 5) * 4);
        i.slc = Bits(w1, 22, 1);
        i.tfe = Bits(w1, 23, 1);
        i.soffset = Bits(w1, 24, 8);
        return true;
    }
    case Enc::MIMG: {
        const u32 w1 = code[1];
        i.size = 2;
        i.dmask = Bits(w, 8, 4);
        i.unorm = Bits(w, 12, 1);
        i.glc = Bits(w, 13, 1);
        i.da = Bits(w, 14, 1);
        i.r128 = Bits(w, 15, 1);
        i.tfe = Bits(w, 16, 1);
        i.lwe = Bits(w, 17, 1);
        i.op = Bits(w, 18, 7);
        i.slc = Bits(w, 25, 1);
        i.vaddr = Bits(w1, 0, 8);
        i.vdata = Bits(w1, 8, 8);
        i.srsrc = static_cast<u8>(Bits(w1, 16, 5) * 4);
        i.ssamp = static_cast<u8>(Bits(w1, 21, 5) * 4);
        return true;
    }
    case Enc::EXP: {
        const u32 w1 = code[1];
        i.size = 2;
        i.en = Bits(w, 0, 4);
        i.target = Bits(w, 4, 6);
        i.compr = Bits(w, 10, 1);
        i.done = Bits(w, 11, 1);
        i.vm = Bits(w, 12, 1);
        for (int k = 0; k < 4; ++k) {
            i.vsrc[k] = Bits(w1, 8 * k, 8);
        }
        return true;
    }
    default:
        return false;
    }
}

std::vector<Inst> DecodeProgram(const u32* code, u32 max_dwords, bool stop_at_setpc) {
    std::vector<Inst> out;
    u32 furthest_target = 0;
    u32 dw = 0;
    while (dw < max_dwords) {
        Inst i;
        if (!Decode(code + dw, dw * 4, i)) {
            break;
        }
        out.push_back(i);
        dw += i.size;
        if (IsBranch(i)) {
            furthest_target = std::max(furthest_target, BranchTarget(i));
        }
        if (i.enc == Enc::SOPP && static_cast<Sopp>(i.op) == Sopp::S_ENDPGM &&
            dw * 4 > furthest_target) {
            break;
        }
        if (stop_at_setpc && i.enc == Enc::SOP1 && static_cast<Sop1>(i.op) == Sop1::S_SETPC_B64 &&
            dw * 4 > furthest_target) {
            break;
        }
    }
    return out;
}

u32 ProgramSize(const u32* code) {
    const auto insts = DecodeProgram(code);
    if (insts.empty()) {
        return 0;
    }
    return insts.back().pc + insts.back().size * 4;
}

const char* OpName(Enc enc, u16 op) {
#define GCN_NAME_ENTRY(name, value) {value, #name},
    static const std::unordered_map<u16, const char*> sop2{GCN_SOP2(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> sopk{GCN_SOPK(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> sop1{GCN_SOP1(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> sopc{GCN_SOPC(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> sopp{GCN_SOPP(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> smrd{GCN_SMRD(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> vop{GCN_VOP(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> vintrp{GCN_VINTRP(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> mubuf{GCN_MUBUF(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> mtbuf{GCN_MTBUF(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> mimg{GCN_MIMG(GCN_NAME_ENTRY)};
    static const std::unordered_map<u16, const char*> ds{GCN_DS(GCN_NAME_ENTRY)};
#undef GCN_NAME_ENTRY
    const std::unordered_map<u16, const char*>* table = nullptr;
    switch (enc) {
    case Enc::SOP2: table = &sop2; break;
    case Enc::SOPK: table = &sopk; break;
    case Enc::SOP1: table = &sop1; break;
    case Enc::SOPC: table = &sopc; break;
    case Enc::SOPP: table = &sopp; break;
    case Enc::SMRD: table = &smrd; break;
    case Enc::VOP1:
    case Enc::VOP2:
    case Enc::VOP3: table = &vop; break;
    case Enc::VOPC: {
        // Compare opcodes are a base plus a condition; name the base.
        u16 base = op;
        if (op < 0x80) {
            base = op & ~15u;
        } else if ((op & 0xF) != 8) {
            base = op & ~7u;
        }
        auto it = vop.find(base);
        return it != vop.end() ? it->second : nullptr;
    }
    case Enc::VINTRP: table = &vintrp; break;
    case Enc::MUBUF: table = &mubuf; break;
    case Enc::MTBUF: table = &mtbuf; break;
    case Enc::MIMG: table = &mimg; break;
    case Enc::DS: table = &ds; break;
    case Enc::EXP: return "EXP";
    default: return "UNKNOWN";
    }
    if (enc == Enc::VOP3 && op < 256) {
        return OpName(Enc::VOPC, op);
    }
    auto it = table->find(op);
    return it != table->end() ? it->second : nullptr;
}

namespace {

std::string Src(u16 code, u32 literal) {
    char buf[32];
    if (code <= Operand::SgprMax) {
        std::snprintf(buf, sizeof(buf), "s%u", code);
    } else if (code >= Operand::Vgpr0) {
        std::snprintf(buf, sizeof(buf), "v%u", code - Operand::Vgpr0);
    } else if (code == Operand::VccLo) {
        return "vcc_lo";
    } else if (code == Operand::VccHi) {
        return "vcc_hi";
    } else if (code == Operand::M0) {
        return "m0";
    } else if (code == Operand::ExecLo) {
        return "exec_lo";
    } else if (code == Operand::ExecHi) {
        return "exec_hi";
    } else if (code >= Operand::Zero && code <= Operand::IntPosMax) {
        std::snprintf(buf, sizeof(buf), "%d", code - Operand::Zero);
    } else if (code > Operand::IntPosMax && code <= Operand::IntNegMax) {
        std::snprintf(buf, sizeof(buf), "%d", -static_cast<int>(code - Operand::IntPosMax));
    } else if (code >= Operand::FloatHalf && code <= Operand::FloatNegFour) {
        static const char* f[] = {"0.5", "-0.5", "1.0", "-1.0", "2.0", "-2.0", "4.0", "-4.0"};
        return f[code - Operand::FloatHalf];
    } else if (code == Operand::Literal) {
        std::snprintf(buf, sizeof(buf), "0x%x", literal);
    } else if (code == Operand::Scc) {
        return "scc";
    } else if (code == Operand::Vccz) {
        return "vccz";
    } else if (code == Operand::Execz) {
        return "execz";
    } else {
        std::snprintf(buf, sizeof(buf), "?%u", code);
    }
    return buf;
}

} // namespace

std::string Disassemble(const Inst& i) {
    char buf[256];
    const char* name = OpName(i.enc, i.op);
    std::string n = name ? name : "?";
    if (name && (i.enc == Enc::VOPC || (i.enc == Enc::VOP3 && i.op < 256)) && (i.op & 0xF) != 8) {
        static const char* fc[] = {"_f", "_lt", "_eq", "_le", "_gt", "_lg", "_ge", "_o",
                                   "_u", "_nge", "_nlg", "_ngt", "_nle", "_neq", "_nlt", "_tru"};
        static const char* ic[] = {"_f", "_lt", "_eq", "_le", "_gt", "_ne", "_ge", "_t"};
        n += i.op < 0x80 ? fc[i.op & 15] : ic[i.op & 7];
    }
    if (!name) {
        std::snprintf(buf, sizeof(buf), "op%u", i.op);
        n += buf;
    }
    std::string args;
    auto add = [&](const std::string& s) {
        if (!args.empty()) {
            args += ", ";
        }
        args += s;
    };
    switch (i.enc) {
    case Enc::SOP2:
    case Enc::SOP1:
        add(Src(i.dst, 0));
        add(Src(i.src[0], i.literal));
        if (i.enc == Enc::SOP2) {
            add(Src(i.src[1], i.literal));
        }
        break;
    case Enc::SOPC:
        add(Src(i.src[0], i.literal));
        add(Src(i.src[1], i.literal));
        break;
    case Enc::SOPK:
        add(Src(i.dst, 0));
        add(std::to_string(i.simm16));
        break;
    case Enc::SOPP:
        if (IsBranch(i)) {
            std::snprintf(buf, sizeof(buf), "L_%04x", BranchTarget(i));
            add(buf);
        } else if (i.simm16) {
            add(std::to_string(i.simm16));
        }
        break;
    case Enc::SMRD:
        std::snprintf(buf, sizeof(buf), "s%u, s[%u:%u], %s%u", i.dst, i.sbase, i.sbase + 1,
                      i.imm ? "" : "s", i.smrd_offset);
        add(buf);
        break;
    case Enc::VOP1:
    case Enc::VOP2:
    case Enc::VOPC:
    case Enc::VOP3: {
        if (i.op >= 256) {
            add(Src(static_cast<u16>(Operand::Vgpr0 + i.dst), 0));
        }
        if (i.op < 256 || (i.vop3 && i.sdst && i.op >= 293 && i.op <= 298)) {
            add(Src(i.sdst, 0));
        }
        int nsrc = 2;
        if (i.op >= 384) {
            nsrc = 1;
        } else if (i.op >= 320 && i.op < 384) {
            nsrc = 3;
        }
        if (i.src[2] != 0 && i.op >= 256 && i.op < 320 && !i.vop3) {
            nsrc = 3;
        }
        if (i.vop3 && i.op >= 256 && i.op < 320) {
            nsrc = (i.op == static_cast<u16>(Vop::V_CNDMASK_B32) || (i.op >= 296 && i.op <= 298)) ? 3 : 2;
        }
        for (int s = 0; s < nsrc; ++s) {
            std::string v = Src(i.src[s], i.literal);
            if (i.abs & (1 << s)) {
                v = "|" + v + "|";
            }
            if (i.neg & (1 << s)) {
                v = "-" + v;
            }
            add(v);
        }
        if (static_cast<Vop>(i.op) == Vop::V_MADMK_F32 || static_cast<Vop>(i.op) == Vop::V_MADAK_F32) {
            std::snprintf(buf, sizeof(buf), "0x%x", i.literal);
            add(buf);
        }
        if (i.clamp) {
            add("clamp");
        }
        if (i.omod) {
            add("omod" + std::to_string(i.omod));
        }
        break;
    }
    case Enc::VINTRP:
        std::snprintf(buf, sizeof(buf), "v%u, v%u, attr%u.%c", i.dst, i.src[0] - Operand::Vgpr0,
                      i.attr, "xyzw"[i.chan]);
        add(buf);
        break;
    case Enc::MUBUF:
    case Enc::MTBUF:
        std::snprintf(buf, sizeof(buf), "v%u, v%u, s[%u:%u], %s offset:%u%s%s%s", i.vdata, i.vaddr,
                      i.srsrc, i.srsrc + 3, Src(i.soffset, 0).c_str(), i.offset,
                      i.idxen ? " idxen" : "", i.offen ? " offen" : "", i.glc ? " glc" : "");
        add(buf);
        if (i.enc == Enc::MTBUF) {
            std::snprintf(buf, sizeof(buf), "dfmt:%u nfmt:%u", i.dfmt, i.nfmt);
            add(buf);
        }
        break;
    case Enc::MIMG:
        std::snprintf(buf, sizeof(buf), "v%u, v%u, s[%u:%u], s[%u:%u] dmask:0x%x%s%s", i.vdata,
                      i.vaddr, i.srsrc, i.srsrc + 7, i.ssamp, i.ssamp + 3, i.dmask,
                      i.unorm ? " unorm" : "", i.da ? " da" : "");
        add(buf);
        break;
    case Enc::DS:
        std::snprintf(buf, sizeof(buf), "v%u, v%u, v%u, v%u off0:%u off1:%u", i.dst, i.vaddr, i.data0,
                      i.data1, i.offset0, i.offset1);
        add(buf);
        break;
    case Enc::EXP: {
        const char* t = "?";
        char tb[16];
        if (i.target < 8) {
            std::snprintf(tb, sizeof(tb), "mrt%u", i.target);
            t = tb;
        } else if (i.target == ExpTarget::MrtZ) {
            t = "mrtz";
        } else if (i.target == ExpTarget::Null) {
            t = "null";
        } else if (i.target >= ExpTarget::Pos0 && i.target < ExpTarget::Pos0 + 4) {
            std::snprintf(tb, sizeof(tb), "pos%u", i.target - ExpTarget::Pos0);
            t = tb;
        } else if (i.target >= ExpTarget::Param0) {
            std::snprintf(tb, sizeof(tb), "param%u", i.target - ExpTarget::Param0);
            t = tb;
        }
        std::snprintf(buf, sizeof(buf), "%s, v%u, v%u, v%u, v%u en:0x%x%s%s%s", t, i.vsrc[0],
                      i.vsrc[1], i.vsrc[2], i.vsrc[3], i.en, i.compr ? " compr" : "",
                      i.done ? " done" : "", i.vm ? " vm" : "");
        add(buf);
        break;
    }
    default:
        break;
    }
    for (auto& c : n) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::snprintf(buf, sizeof(buf), "%04x: ", i.pc);
    return std::string(buf) + n + (args.empty() ? "" : " " + args);
}

} // namespace Gpu::Shader::Gcn
