// Developer tool: disassembles a raw GCN shader binary (as dumped by the
// renderer with DC_DUMP_SHADERS=dir), translates it to MSL and compiles that
// with Metal to catch translator bugs without running the game.
//
//   gcn_dis <shader.bin> [vs|ps] [--msl]

#import <Metal/Metal.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "gpu/shader/gcn.h"
#include "gpu/shader/msl.h"
#include "core/memory.h"
#include "gpu/shader/program.h"

// The tool has no guest memory: nothing is mapped.
namespace Core::Memory {
bool Query(VAddr, bool, Vma*) {
    return false;
}
} // namespace Core::Memory

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gcn_dis <shader.bin> [vs|ps|cs] [--msl]\n");
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
    std::vector<u32> code((bytes.size() + 3) / 4 + 2, 0);
    std::memcpy(code.data(), bytes.data(), bytes.size());

    using namespace Gpu::Shader;
    const bool ps = argc > 2 && std::strcmp(argv[2], "ps") == 0;
    const bool cs = argc > 2 && std::strcmp(argv[2], "cs") == 0;
    const bool print_msl = argc > 3 && std::strcmp(argv[3], "--msl") == 0;

    u32 ud[16] = {};
    BuildInput in{cs ? Stage::Compute : ps ? Stage::Fragment : Stage::Vertex, 0x10000000, ud, 16, cs ? 2u : 3u,
                  code.data()};
    in.compute_rsrc2 = cs ? (7u << 7) : 0; // tests: all group ids enabled
    auto prog = BuildProgram(in);
    if (!prog) {
        std::fprintf(stderr, "cannot decode\n");
        return 1;
    }
    for (const auto& i : prog->code) {
        std::printf("%s\n", Gcn::Disassemble(i).c_str());
    }

    MslOptions o;
    o.vs_param_mask = 0xFFFFFFFF;
    o.ps_input_addr = 0x2; // PERSP_CENTER
    o.col_format = 0x4;    // MRT0 FP16
    for (u32 k = 0; k < 32; ++k) {
        o.ps_input_cntl[k] = k;
    }
    o.textures.resize(prog->images.size());
    std::vector<std::string> unsupported;
    const std::string src = GenerateMsl(*prog, o, &unsupported);
    if (print_msl) {
        std::printf("%s\n", src.c_str());
    }
    for (const auto& u : unsupported) {
        std::printf("unsupported: %s\n", u.c_str());
    }

    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        MTLCompileOptions* opts = [MTLCompileOptions new];
        opts.mathMode = MTLMathModeSafe;
        NSError* err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                               options:opts
                                                 error:&err];
        if (!lib) {
            std::printf("MSL compile FAILED:\n%s\n", err.localizedDescription.UTF8String);
            return 2;
        }
        std::printf("MSL compiled OK (%zu bytes of source)\n", src.size());
    }
    return 0;
}
