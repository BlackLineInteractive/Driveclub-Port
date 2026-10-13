// Developer tool: disassembles a raw GCN shader binary (e.g. one dumped with
// DC_DUMP_SHADERS) and optionally prints the generated MSL.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "gpu/shader/gcn.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gcn_dis <shader.bin>\n");
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<u32> code((bytes.size() + 3) / 4 + 2, 0);
    std::memcpy(code.data(), bytes.data(), bytes.size());
    const auto insts = Gpu::Shader::Gcn::DecodeProgram(code.data(), static_cast<u32>(bytes.size() / 4));
    for (const auto& i : insts) {
        std::printf("%s\n", Gpu::Shader::Gcn::Disassemble(i).c_str());
    }
    return 0;
}
