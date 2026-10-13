// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/ir/program.h"

#include <spirv_cross/spirv_msl.hpp>
#include "shader_recompiler/frontend/decode.h"
#include <magic_enum/magic_enum.hpp>

#include "common/logging/classes.h"
#include "common/logging/log.h"

// Stubs for Common functions needed by shader_recompiler
namespace Common::Log {
std::array<Level, NUM_LOG_CLASSES> g_class_levels{};
void VLog(Class, Level, const char* file, int line, const char* func, fmt::string_view format, fmt::format_args args) {
    std::string s = fmt::vformat(format, args);
    std::printf("[shader_recompiler] %s:%d (%s): %s\n", file ? file : "", line, func ? func : "", s.c_str());
}
} // namespace Common::Log

#include "common/path_util.h"
#include "common/io_file.h"
#include "core/emulator_settings.h"

EmulatorSettingsImpl::EmulatorSettingsImpl() = default;
EmulatorSettingsImpl::~EmulatorSettingsImpl() = default;

std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::GetInstance() {
    static auto inst = std::make_shared<EmulatorSettingsImpl>();
    return inst;
}

namespace Common::FS {
const std::filesystem::path& GetUserPath(PathType) {
    static const std::filesystem::path p = "/tmp";
    return p;
}
IOFile::IOFile(const std::filesystem::path&, FileAccessMode, FileType, FileShareFlag) {}
IOFile::~IOFile() = default;
} // namespace Common::FS

void assert_fail_impl() {
    __builtin_trap();
}

[[noreturn]] void unreachable_impl() {
    __builtin_trap();
}

void assert_fail_debug_msg(const char* msg) {
    std::fprintf(stderr, "assertion failed: %s\n", msg);
    __builtin_trap();
}

int main(int argc, char** argv) {
    const char* vs_path = argc > 1 ? argv[1] : "/tmp/vs_ui.bin";
    const char* ps_path = argc > 2 ? argv[2] : "/tmp/ps_ui.bin";

    std::ifstream vs_file(vs_path, std::ios::binary | std::ios::ate);
    if (!vs_file.is_open()) {
        std::cerr << "Failed to open " << vs_path << "\n";
        return 1;
    }
    size_t vs_size = vs_file.tellg();
    vs_file.seekg(0);
    std::vector<u32> vs_code(vs_size / sizeof(u32));
    vs_file.read(reinterpret_cast<char*>(vs_code.data()), vs_size);

    // Truncate at S_ENDPGM (0xbf810000)
    for (size_t i = 0; i < vs_code.size(); ++i) {
        if (vs_code[i] == 0xbf810000) {
            vs_code.resize(i + 1);
            break;
        }
    }
    std::cout << "Loaded VS: " << vs_code.size() << " dwords (trimmed to S_ENDPGM)\n";

    {
        Shader::Gcn::GcnCodeSlice slice(vs_code.data(), vs_code.data() + vs_code.size());
        Shader::Gcn::GcnDecodeContext ctx;
        u32 pc = 0;
        while (!slice.atEnd()) {
            u32 cur_pc = pc;
            auto inst = ctx.decodeInstruction(slice);
            pc += inst.length * 4;
            std::cout << "0x" << std::hex << cur_pc << ": " << magic_enum::enum_name(inst.opcode);
            for (u32 s = 0; s < inst.src_count; ++s) {
                std::cout << " src" << s << "=" << inst.src[s].code;
            }
            for (u32 d = 0; d < inst.dst_count; ++d) {
                std::cout << " dst" << d << "=" << inst.dst[d].code;
            }
            std::cout << std::dec << "\n";
            if (inst.opcode == Shader::Gcn::Opcode::S_ENDPGM) break;
        }
    }

    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.support_float64 = false;
    profile.subgroup_size = 32;

    // Translate VS
    {
        Shader::Pools pools;
        Shader::Info info{};
        info.hw_stage = Shader::HwStage::Vertex;
        info.sw_stage = Shader::SwStage::Vertex;

        static const std::array<u32, 16> vs_ud = {
            0xd0f90, 0x100003, 0x1f, 0x20077fac,
            0xd01b0, 0x100003, 0x6f, 0x20077fac,
            0, 0, 0, 0, 0, 0, 0, 0
        };
        info.user_data = vs_ud;
        info.flattened_ud_buf.assign(vs_ud.begin(), vs_ud.end());

        Shader::RuntimeInfo rinfo{};
        rinfo.Initialize(Shader::HwStage::Vertex, Shader::SwStage::Vertex);
        rinfo.props.num_user_data = 8;
        rinfo.props.num_allocated_vgprs = 16;
        rinfo.hw.vs.num_outputs = 5;

        std::cout << "Translating VS to IR...\n";
        auto ir = Shader::TranslateProgram(vs_code, pools, info, rinfo, profile);
        for (const auto& b : ir.blocks) {
            for (const auto& inst : *b) {
                const auto op = inst.GetOpcode();
                if (op == Shader::IR::Opcode::ReadConstBuffer ||
                    op == Shader::IR::Opcode::LoadBufferU32 ||
                    op == Shader::IR::Opcode::LoadBufferU32x4 ||
                    op == Shader::IR::Opcode::GetUserData) {
                    std::cout << "IR: " << Shader::IR::NameOf(op);
                    for (u32 a = 0; a < inst.NumArgs(); ++a) {
                        std::cout << " arg" << a << "=";
                        auto arg = inst.Arg(a);
                        if (arg.IsImmediate()) std::cout << "imm(" << arg.U32() << ")";
                        else if (arg.TryInst()) std::cout << "inst(" << Shader::IR::NameOf(arg.TryInst()->GetOpcode()) << ")";
                        else std::cout << "type(" << (int)arg.Type() << ")";
                    }
                    std::cout << "\n";
                }
            }
        }

        std::cout << "Emitting VS SPIR-V...\n";
        Shader::Backend::Bindings bindings{};
        auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rinfo, ir, bindings);
        std::cout << "SPIR-V size: " << spv.size() << " dwords\n";
        for (size_t i = 0; i < info.buffers.size(); ++i) {
            auto& b = info.buffers[i];
            std::cout << "VS Buffer " << i << ": type=" << (int)b.buffer_type
                      << " base_offset=" << b.sharp_fetch.offsets[0]
                      << " load_mask=" << (int)b.sharp_fetch.load_mask
                      << " is_written=" << b.is_written
                      << " summary=" << (int)b.sharp_fetch.summary << "\n";
            for (int k = 0; k < 4; ++k) {
                std::cout << "   offset[" << k << "]=" << b.sharp_fetch.offsets[k]
                          << " imm[" << k << "]=" << b.sharp_fetch.immediates[k] << "\n";
            }
        }

        std::cout << "Converting VS to MSL...\n";
        spirv_cross::CompilerMSL msl(spv.data(), spv.size());
        spirv_cross::CompilerMSL::Options msl_opts;
        msl_opts.set_msl_version(3, 0);
        msl.set_msl_options(msl_opts);

        auto resources = msl.get_shader_resources();
        for (auto& r : resources.storage_buffers) {
            uint32_t set = msl.get_decoration(r.id, spv::DecorationDescriptorSet);
            uint32_t binding = msl.get_decoration(r.id, spv::DecorationBinding);
            std::cout << "Storage buffer: name=" << r.name << " id=" << r.id
                      << " set=" << set << " binding=" << binding << "\n";
        }

        std::string msl_code = msl.compile();
        std::ofstream ofs("/tmp/vs_ui.metal");
        ofs << msl_code;
        std::cout << "Saved /tmp/vs_ui.metal (" << msl_code.size() << " bytes)\n";
        std::cout << std::flush;
        return 0;
    }

    // Translate PS
    std::ifstream ps_file(ps_path, std::ios::binary | std::ios::ate);
    if (ps_file.is_open()) {
        size_t ps_size = ps_file.tellg();
        ps_file.seekg(0);
        std::vector<u32> ps_code(ps_size / sizeof(u32));
        ps_file.read(reinterpret_cast<char*>(ps_code.data()), ps_size);

        // Truncate at S_ENDPGM (0xbf810000)
        for (size_t i = 0; i < ps_code.size(); ++i) {
            if (ps_code[i] == 0xbf810000) {
                ps_code.resize(i + 1);
                break;
            }
        }

        std::cout << "Loaded PS: " << ps_code.size() << " dwords (trimmed to S_ENDPGM)\n";
        Shader::Pools pools;
        Shader::Info info{};
        info.hw_stage = Shader::HwStage::Fragment;
        info.sw_stage = Shader::SwStage::Fragment;

        static const std::array<u32, 16> ps_ud = {
            0xd0e60, 0x100003, 0x2, 0x20077fac,
            0x48a8003, 0x26300000, 0x7001c007, 0x96d30fac,
            0, 0, 0, 0, 0, 0, 0, 0
        };
        info.user_data = ps_ud;
        info.flattened_ud_buf.assign(ps_ud.begin(), ps_ud.end());

        Shader::RuntimeInfo rinfo{};
        rinfo.Initialize(Shader::HwStage::Fragment, Shader::SwStage::Fragment);
        rinfo.props.num_user_data = 8;
        rinfo.props.num_allocated_vgprs = 16;
        rinfo.hw.fs.num_inputs = 4;
        for (u32 i = 0; i < 4; i++) {
            rinfo.hw.fs.inputs[i].param_index = i;
        }
        rinfo.hw.fs.en_flags.persp_center_ena = 1;
        rinfo.hw.fs.addr_flags.persp_center_ena = 1;
        rinfo.hw.fs.color_buffers[0].num_format = AmdGpu::NumberFormat::Float;
        rinfo.hw.fs.color_buffers[0].export_format = AmdGpu::ShaderExportFormat::ABGR_FP16;

        std::cout << "Translating PS to IR...\n";
        auto ir = Shader::TranslateProgram(ps_code, pools, info, rinfo, profile);
        for (const auto& b : ir.blocks) {
            std::cout << Shader::IR::DumpBlock(*b) << "\n";
        }

        std::cout << "Emitting PS SPIR-V...\n";
        Shader::Backend::Bindings bindings{};
        auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, rinfo, ir, bindings);
        std::cout << "SPIR-V size: " << spv.size() << " dwords\n";
        for (size_t i = 0; i < info.buffers.size(); ++i) {
            auto& b = info.buffers[i];
            std::cout << "PS Buffer " << i << ": type=" << (int)b.buffer_type
                      << " base_offset=" << b.sharp_fetch.offsets[0] << "\n";
        }
        for (size_t i = 0; i < info.images.size(); ++i) {
            auto& img = info.images[i];
            std::cout << "PS Image " << i << ": base_offset=" << img.sharp_fetch.offsets[0] << "\n";
        }

        std::cout << "Converting PS to MSL...\n";
        spirv_cross::CompilerMSL msl(spv.data(), spv.size());
        spirv_cross::CompilerMSL::Options msl_opts;
        msl_opts.set_msl_version(3, 0);
        msl.set_msl_options(msl_opts);

        std::string msl_code = msl.compile();
        const char* out_path = argc > 3 ? argv[3] : "/tmp/ps_ui.metal";
        std::ofstream ofs(out_path);
        ofs << msl_code;
        std::cout << "Saved " << out_path << " (" << msl_code.size() << " bytes)\n";
    }

    return 0;
}
