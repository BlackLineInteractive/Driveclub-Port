#include "gpu/metal/mtl_renderer.h"

#import <Metal/Metal.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "core/memory.h"
#include "gpu/liverpool/registers.h"
#include "gpu/metal/mtl_device.h"
#include "gpu/metal/mtl_formats.h"
#include "gpu/shader/msl.h"
#include "gpu/shader/msl_prelude.h"
#include "gpu/shader/program.h"

namespace Gpu::Metal {

using namespace Liverpool;
using Shader::OutType;
using Shader::Program;
using Shader::Stage;
using Shader::TexDim;
using Shader::TexSample;
using Shader::TextureType;

namespace {

constexpr u64 kUploadChunk = 4ull << 20;

u64 Fnv(const void* data, size_t size, u64 h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

template <typename T>
u64 Hash(const T& v, u64 seed = 0xcbf29ce484222325ull) {
    return Fnv(&v, sizeof(T), seed);
}

bool Readable(u64 addr, u64 size) {
    Core::Memory::Vma vma;
    return addr && Core::Memory::Query(addr, false, &vma) && addr + size <= vma.start + vma.size;
}

// Scissor registers: x in [14:0], y in [30:16].
struct Rect {
    s32 x0, y0, x1, y1;
};
Rect ScissorReg(u32 tl, u32 br) {
    return {static_cast<s32>(tl & 0x7FFF), static_cast<s32>((tl >> 16) & 0x7FFF),
            static_cast<s32>(br & 0x7FFF), static_cast<s32>((br >> 16) & 0x7FFF)};
}
Rect Intersect(Rect a, Rect b) {
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

struct PipelineKey {
    u64 vs = 0;
    u64 ps = 0;
    u32 color_fmt[8] = {};
    u32 blend[8] = {};
    u32 write_mask = 0;
    u32 depth_fmt = 0;
};

constexpr const char* kCopyShader = R"(
#include <metal_stdlib>
using namespace metal;
struct V { float4 pos [[position]]; float2 uv; };
vertex V copy_vs(uint vid [[vertex_id]]) {
    V o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);
    return o;
}
fragment float4 debug_fs() { return float4(1.0, 0.0, 1.0, 1.0); }
fragment float4 copy_fs(V in [[stage_in]], texture2d<float> src [[texture(0)]],
                        constant float2& scale [[buffer(0)]]) {
    constexpr sampler s(filter::nearest, address::clamp_to_edge);
    return float4(src.sample(s, in.uv * scale).rgb, 1.0);
}
)";

} // namespace

struct MetalRenderer::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    std::mutex lock;

    // ---- guest memory view ----
    id<MTLBuffer> mem[2] = {nil, nil};
    u64 mem_split = 0;
    id<MTLBuffer> mem_map = nil;
    u64 mem_map_gen = ~0ull;

    // ---- recording ----
    id<MTLCommandBuffer> cmd = nil;
    id<MTLCommandBuffer> last_committed = nil;
    id<MTLRenderCommandEncoder> enc = nil;
    struct PassState {
        u64 color[8] = {};
        u64 depth = 0;
        bool operator==(const PassState&) const = default;
    } pass;
    u32 pass_width = 0, pass_height = 0;
    std::vector<id<MTLBuffer>> chunks;
    id<MTLBuffer> chunk = nil;
    u64 chunk_off = 0;

    // ---- caches ----
    struct ShaderEntry {
        std::unique_ptr<Program> prog;
        u32 head[4] = {};
    };
    std::unordered_map<u64, std::vector<ShaderEntry>> shaders;
    std::unordered_map<u64, id<MTLFunction>> functions;
    std::unordered_map<u64, id<MTLRenderPipelineState>> pipelines;
    std::unordered_map<u32, id<MTLDepthStencilState>> depth_states;
    std::unordered_map<u64, id<MTLSamplerState>> samplers;

    struct RenderTarget {
        id<MTLTexture> tex = nil;
        u64 addr = 0;
        u32 width = 0, height = 0;
        bool initialized = false;
    };
    std::unordered_map<u64, RenderTarget> color_targets; // by address
    std::unordered_map<u64, RenderTarget> depth_targets;

    struct TextureEntry {
        id<MTLTexture> tex = nil;
        u64 checksum = 0;
        u64 frame = 0;
        u64 addr = 0;
        u64 size = 0;
    };
    std::unordered_map<u64, TextureEntry> textures;
    std::unordered_map<u32, id<MTLTexture>> dummy_textures;
    id<MTLSamplerState> default_sampler = nil;
    id<MTLBuffer> quad_indices = nil;

    // ---- presentation ----
    id<MTLRenderPipelineState> copy_pipeline = nil;
    id<MTLTexture> frames[3] = {nil, nil, nil};
    u32 frame_index = 0;
    std::atomic<void*> latest_frame{nullptr};
    u64 frame_count = 0;

    // ---- diagnostics ----
    const char* dump_dir = nullptr;
    const char* dump_frames = nullptr; // DC_DUMP_FRAMES=dir: every flip as a PPM
    bool debug_clear = false;          // DC_DEBUG_CLEAR: new targets start dark blue
    id<MTLFunction> debug_fs = nil;    // DC_DEBUG_FLAT: every pixel shader outputs magenta
    bool trace = false;
    u64 draw_count = 0;
    std::set<std::string> reported;

    void ReportOnce(const std::string& what) {
        if (reported.insert(what).second) {
            DC_WARN("gpu: %s", what.c_str());
        }
    }

    // ================= setup =================

    void Init() {
        dump_dir = std::getenv("DC_DUMP_SHADERS");
        dump_frames = std::getenv("DC_DUMP_FRAMES");
        debug_clear = std::getenv("DC_DEBUG_CLEAR") != nullptr;
        trace = std::getenv("DC_GPU_TRACE") != nullptr;

        void* view = Core::Memory::DirectHostView();
        const u64 total = Core::Memory::kDirectSize;
        mem_split = (total / 2) & ~0xFFFFull;
        if (view) {
            mem[0] = [device newBufferWithBytesNoCopy:view
                                               length:mem_split
                                              options:MTLResourceStorageModeShared
                                          deallocator:nil];
            mem[1] = [device newBufferWithBytesNoCopy:static_cast<u8*>(view) + mem_split
                                               length:total - mem_split
                                              options:MTLResourceStorageModeShared
                                          deallocator:nil];
        }
        if (!mem[0] || !mem[1]) {
            DC_ERROR("gpu: cannot expose guest memory to Metal");
        }

        std::vector<u32> quads(0x10000 * 6);
        for (u32 q = 0; q < 0x10000; ++q) {
            const u32 b = q * 4;
            const u32 idx[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
            std::memcpy(&quads[q * 6], idx, sizeof(idx));
        }
        quad_indices = [device newBufferWithBytes:quads.data()
                                           length:quads.size() * 4
                                          options:MTLResourceStorageModeShared];

        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        default_sampler = [device newSamplerStateWithDescriptor:sd];

        NSError* err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@(kCopyShader) options:nil error:&err];
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        if (std::getenv("DC_DEBUG_FLAT")) {
            debug_fs = [lib newFunctionWithName:@"debug_fs"];
        }
        pd.vertexFunction = [lib newFunctionWithName:@"copy_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"copy_fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        copy_pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&err];
        if (!copy_pipeline) {
            DC_ERROR("gpu: copy pipeline: %s", err.localizedDescription.UTF8String);
        }
    }

    void UpdateMemMap() {
        const u64 gen = Core::Memory::DirectMapGeneration();
        if (gen == mem_map_gen && mem_map) {
            return;
        }
        mem_map_gen = gen;
        Shader::GpuMemMap map{};
        map.split = mem_split;
        for (const auto& r : Core::Memory::DirectMappings()) {
            if (map.count == Shader::GpuMemMap::kMaxRanges) {
                ReportOnce("too many direct mappings for the GPU range table");
                break;
            }
            map.ranges[map.count++] = {r.start, r.size, r.phys};
        }
        // A fresh buffer: work already recorded keeps reading the old table.
        mem_map = [device newBufferWithBytes:&map length:sizeof(map) options:MTLResourceStorageModeShared];
    }

    // ================= recording =================

    void EnsureCommandBuffer() {
        if (!cmd) {
            cmd = [queue commandBuffer];
        }
    }

    void EndPass() {
        if (enc) {
            [enc endEncoding];
            enc = nil;
        }
        pass = {};
    }

    void Commit(bool wait) {
        EndPass();
        if (!cmd) {
            if (wait) {
                WaitLast();
            }
            return;
        }
        if (!chunks.empty() || chunk) {
            // Keep upload memory alive until the GPU is done with it.
            __block std::vector<id<MTLBuffer>> keep = std::move(chunks);
            if (chunk) {
                keep.push_back(chunk);
            }
            [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
              keep.clear();
            }];
            chunks.clear();
            chunk = nil;
            chunk_off = 0;
        }
        [cmd commit];
        last_committed = cmd;
        cmd = nil;
        if (wait) {
            WaitLast();
        }
    }

    // Command buffers on one queue complete in order: the last one is enough.
    void WaitLast() {
        if (last_committed) {
            [last_committed waitUntilCompleted];
            last_committed = nil;
        }
    }

    // Bump-allocates transient GPU-visible memory for this command buffer.
    std::pair<id<MTLBuffer>, u64> Upload(const void* data, u64 size) {
        size = (size + 15) & ~15ull;
        if (!chunk || chunk_off + size > chunk.length) {
            if (chunk) {
                chunks.push_back(chunk);
            }
            chunk = [device newBufferWithLength:std::max(kUploadChunk, size) options:MTLResourceStorageModeShared];
            chunk_off = 0;
        }
        const u64 off = chunk_off;
        if (data) {
            std::memcpy(static_cast<u8*>(chunk.contents) + off, data, size);
        }
        chunk_off += size;
        return {chunk, off};
    }

    // ================= shaders =================

    Program* GetProgram(Stage stage, u64 addr, const u32* ud, u32 num_user_sgprs, u32 vgpr_comp_cnt) {
        if (!Readable(addr, 16)) {
            return nullptr;
        }
        const u32* code = reinterpret_cast<const u32*>(addr);
        auto& list = shaders[addr ^ (static_cast<u64>(stage) << 60)];
        for (auto& e : list) {
            if (std::memcmp(e.head, code, sizeof(e.head)) != 0) {
                continue;
            }
            const Program& p = *e.prog;
            if (p.fetch_ud >= 0) {
                const u64 f = u64(ud[p.fetch_ud]) | (u64(ud[p.fetch_ud + 1]) << 32);
                if (f != p.fetch_address) {
                    continue;
                }
            }
            return e.prog.get();
        }
        Shader::BuildInput in{stage, addr, ud, num_user_sgprs, vgpr_comp_cnt};
        auto prog = Shader::BuildProgram(in);
        if (!prog) {
            ReportOnce("cannot decode shader at " + std::to_string(addr));
            return nullptr;
        }
        if (dump_dir) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/%s_%llx_%016llx.bin", dump_dir,
                          stage == Stage::Vertex ? "vs" : stage == Stage::Fragment ? "ps" : "cs", addr, prog->hash);
            const u32 size = Shader::Gcn::ProgramSize(code);
            std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(code), size);
        }
        ShaderEntry e;
        std::memcpy(e.head, code, sizeof(e.head));
        e.prog = std::move(prog);
        list.push_back(std::move(e));
        return list.back().prog.get();
    }

    id<MTLFunction> GetFunction(const Program& p, const Shader::MslOptions& o, u64 key) {
        auto it = functions.find(key);
        if (it != functions.end()) {
            return it->second;
        }
        std::vector<std::string> unsupported;
        const std::string src = Shader::GenerateMsl(p, o, &unsupported);
        for (const auto& u : unsupported) {
            ReportOnce("shader op not translated: " + u);
        }
        MTLCompileOptions* opts = [MTLCompileOptions new];
        opts.mathMode = MTLMathModeSafe;
        opts.languageVersion = MTLLanguageVersion3_0;
        NSError* err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@(src.c_str()) options:opts error:&err];
        if (dump_dir) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s/%s_%016llx_%016llx.metal", dump_dir,
                          p.stage == Stage::Vertex ? "vs" : "ps", p.hash, key);
            std::ofstream(path) << src;
        }
        id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"main0"] : nil;
        if (!fn) {
            DC_ERROR("gpu: MSL compile failed for shader %#llx: %s", p.address,
                     err ? err.localizedDescription.UTF8String : "?");
        }
        functions[key] = fn;
        return fn;
    }

    // ================= textures =================

    id<MTLTexture> Dummy(TextureType tt) {
        const u32 key = static_cast<u32>(tt.dim) | (static_cast<u32>(tt.sample) << 4);
        auto it = dummy_textures.find(key);
        if (it != dummy_textures.end()) {
            return it->second;
        }
        MTLTextureDescriptor* d = [MTLTextureDescriptor new];
        d.textureType = tt.dim == TexDim::Tex2D ? MTLTextureType2D
                      : tt.dim == TexDim::Tex2DArray ? MTLTextureType2DArray : MTLTextureType3D;
        d.pixelFormat = tt.sample == TexSample::Float ? MTLPixelFormatRGBA8Unorm
                      : tt.sample == TexSample::Uint ? MTLPixelFormatRGBA8Uint : MTLPixelFormatRGBA8Sint;
        d.width = d.height = 1;
        d.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> t = [device newTextureWithDescriptor:d];
        const u32 white = tt.sample == TexSample::Float ? 0xFFFFFFFFu : 0x01010101u;
        [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:&white bytesPerRow:4 bytesPerImage:4];
        dummy_textures[key] = t;
        return t;
    }

    // Cheap change detection: a few dwords spread over the texture.
    static u64 SparseChecksum(u64 addr, u64 size) {
        u64 h = 0xcbf29ce484222325ull;
        const u64 step = std::max<u64>(4, (size / 64) & ~3ull);
        for (u64 off = 0; off + 4 <= size; off += step) {
            u32 v;
            std::memcpy(&v, reinterpret_cast<const void*>(addr + off), 4);
            h = (h ^ v) * 0x100000001b3ull;
        }
        return h;
    }

    // Decodes a T# and returns a Metal texture for it.
    id<MTLTexture> GetTexture(const u32 t[8], TextureType* type_out) {
        const u64 addr = (u64(t[0]) | (u64(t[1] & 0xFF) << 32)) << 8;
        const u32 dfmt = (t[1] >> 20) & 0x3F;
        const u32 nfmt = (t[1] >> 26) & 0xF;
        const u32 width = (t[2] & 0x3FFF) + 1;
        const u32 height = ((t[2] >> 14) & 0x3FFF) + 1;
        const u32 swz = t[3] & 0xFFF;
        const u32 tiling = (t[3] >> 20) & 0x1F;
        const u32 ttype = t[3] >> 28;
        const u32 depth = (t[4] & 0x1FFF) + 1;
        const u32 pitch = ((t[4] >> 13) & 0x3FFF) + 1;
        const u32 last_array = (t[5] >> 13) & 0x1FFF;

        const TextureFormatInfo fi = TextureFormat(dfmt, nfmt);
        TextureType tt;
        tt.sample = fi.sample;
        if (ttype == 10) {
            tt.dim = TexDim::Tex3D;
        } else if (ttype == 11 || ttype == 12 || ttype == 13 || ttype == 15) {
            tt.dim = TexDim::Tex2DArray;
        }
        *type_out = tt;
        if (!addr || ttype < 8) {
            return Dummy(tt);
        }

        // Render-to-texture: sample the render target directly.
        auto rt = color_targets.find(addr);
        if (rt != color_targets.end() && tt.dim == TexDim::Tex2D) {
            return rt->second.tex;
        }

        if (fi.format == MTLPixelFormatInvalid) {
            ReportOnce("texture format dfmt=" + std::to_string(dfmt) + " nfmt=" + std::to_string(nfmt) +
                       " not supported");
            return Dummy(tt);
        }
        const u32 layers = tt.dim == TexDim::Tex2DArray ? std::max<u32>(last_array + 1, ttype == 11 ? 6 : 1)
                         : tt.dim == TexDim::Tex3D ? depth : 1;
        const u32 bw = fi.compressed ? (pitch + 3) / 4 : pitch;
        const u32 bh = fi.compressed ? (height + 3) / 4 : height;
        const u64 row = u64(bw) * fi.bytes_per_block;
        const u64 image = row * bh;
        const u64 size = image * layers;
        if (!Readable(addr, size)) {
            ReportOnce("texture memory not mapped");
            return Dummy(tt);
        }

        u64 key = Hash(addr);
        for (u32 k = 1; k < 6; ++k) {
            key = Hash(t[k], key);
        }
        TextureEntry& e = textures[key];
        if (e.tex && e.frame == frame_count) {
            return e.tex;
        }
        const u64 sum = SparseChecksum(addr, size);
        if (e.tex && sum == e.checksum) {
            e.frame = frame_count;
            return e.tex;
        }
        if (tiling != 8 && tiling != 0x1F) {
            ReportOnce("tiled texture (tile index " + std::to_string(tiling) + ") uploaded as linear");
        }

        if (!e.tex) {
            MTLTextureDescriptor* d = [MTLTextureDescriptor new];
            d.textureType = tt.dim == TexDim::Tex2D ? MTLTextureType2D
                          : tt.dim == TexDim::Tex2DArray ? MTLTextureType2DArray : MTLTextureType3D;
            d.pixelFormat = fi.format;
            d.width = width;
            d.height = height;
            if (tt.dim == TexDim::Tex3D) {
                d.depth = layers;
            } else if (tt.dim == TexDim::Tex2DArray) {
                d.arrayLength = layers;
            }
            d.usage = MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModeManaged;
            d.swizzle = MTLTextureSwizzleChannelsMake(Swizzle(swz), Swizzle(swz >> 3), Swizzle(swz >> 6),
                                                      Swizzle(swz >> 9));
            e.tex = [device newTextureWithDescriptor:d];
            if (!e.tex) {
                return Dummy(tt);
            }
        }
        const auto* src = reinterpret_cast<const u8*>(addr);
        for (u32 l = 0; l < layers; ++l) {
            const MTLRegion region = tt.dim == TexDim::Tex3D ? MTLRegionMake3D(0, 0, l, width, height, 1)
                                                             : MTLRegionMake2D(0, 0, width, height);
            [e.tex replaceRegion:region
                     mipmapLevel:0
                           slice:tt.dim == TexDim::Tex2DArray ? l : 0
                       withBytes:src + image * l
                     bytesPerRow:row
                   bytesPerImage:image];
        }
        e.checksum = sum;
        e.frame = frame_count;
        e.addr = addr;
        e.size = size;
        return e.tex;
    }

    id<MTLSamplerState> GetSampler(const u32 s[4]) {
        const u64 key = Fnv(s, 16);
        auto it = samplers.find(key);
        if (it != samplers.end()) {
            return it->second;
        }
        MTLSamplerDescriptor* d = [MTLSamplerDescriptor new];
        d.sAddressMode = AddressMode(s[0]);
        d.tAddressMode = AddressMode(s[0] >> 3);
        d.rAddressMode = AddressMode(s[0] >> 6);
        const u32 aniso = (s[0] >> 9) & 7;
        const u32 mag = (s[2] >> 20) & 3, min = (s[2] >> 22) & 3, mip = (s[2] >> 26) & 3;
        d.magFilter = (mag & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.minFilter = (min & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        d.mipFilter = mip == 0 ? MTLSamplerMipFilterNotMipmapped
                    : mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
        if (min >= 2 || mag >= 2) {
            d.maxAnisotropy = std::min<u32>(16, 1u << aniso);
        }
        d.lodMinClamp = static_cast<float>(s[1] & 0xFFF) / 256.0f;
        d.lodMaxClamp = static_cast<float>((s[1] >> 12) & 0xFFF) / 256.0f;
        const u32 border = (s[3] >> 30) & 3;
        d.borderColor = border == 0 ? MTLSamplerBorderColorTransparentBlack
                      : border == 1 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite;
        id<MTLSamplerState> st = [device newSamplerStateWithDescriptor:d];
        samplers[key] = st ? st : default_sampler;
        return samplers[key];
    }

    // ================= render targets =================

    RenderTarget& GetColorTarget(u64 addr, MTLPixelFormat fmt, u32 width, u32 height) {
        RenderTarget& rt = color_targets[addr];
        if (!rt.tex || rt.width != width || rt.height != height || rt.tex.pixelFormat != fmt) {
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                         width:width
                                                                                        height:height
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
            d.storageMode = MTLStorageModePrivate;
            rt = {[device newTextureWithDescriptor:d], addr, width, height, false};
            DC_INFO("gpu: render target %#llx %ux%u fmt %lu", addr, width, height, (unsigned long)fmt);
        }
        return rt;
    }

    RenderTarget& GetDepthTarget(u64 addr, MTLPixelFormat fmt, u32 width, u32 height) {
        RenderTarget& rt = depth_targets[addr];
        if (!rt.tex || rt.width != width || rt.height != height || rt.tex.pixelFormat != fmt) {
            MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                                         width:width
                                                                                        height:height
                                                                                     mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModePrivate;
            rt = {[device newTextureWithDescriptor:d], addr, width, height, false};
            DC_INFO("gpu: depth target %#llx %ux%u", addr, width, height);
        }
        return rt;
    }

    void BeginPass(RenderTarget* colors[8], RenderTarget* depth, bool clear_depth, float depth_value) {
        EndPass();
        EnsureCommandBuffer();
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        PassState ps;
        for (u32 k = 0; k < 8; ++k) {
            if (!colors[k]) {
                continue;
            }
            RenderTarget& rt = *colors[k];
            rp.colorAttachments[k].texture = rt.tex;
            rp.colorAttachments[k].loadAction = rt.initialized ? MTLLoadActionLoad : MTLLoadActionClear;
            rp.colorAttachments[k].clearColor = debug_clear ? MTLClearColorMake(0.1, 0.1, 0.3, 1) : MTLClearColorMake(0, 0, 0, 0);
            rp.colorAttachments[k].storeAction = MTLStoreActionStore;
            rt.initialized = true;
            ps.color[k] = rt.addr;
            pass_width = rt.width;
            pass_height = rt.height;
        }
        if (depth) {
            rp.depthAttachment.texture = depth->tex;
            rp.depthAttachment.loadAction = (clear_depth || !depth->initialized) ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth = clear_depth ? depth_value : 1.0;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            depth->initialized = true;
            ps.depth = depth->addr;
            if (!ps.color[0] && !ps.color[1]) {
                pass_width = depth->width;
                pass_height = depth->height;
            }
        }
        enc = [cmd renderCommandEncoderWithDescriptor:rp];
        pass = ps;
    }

    // ================= draws =================

    id<MTLDepthStencilState> GetDepthState(u32 db_control) {
        auto it = depth_states.find(db_control);
        if (it != depth_states.end()) {
            return it->second;
        }
        MTLDepthStencilDescriptor* d = [MTLDepthStencilDescriptor new];
        const bool z_enable = (db_control >> 1) & 1;
        d.depthCompareFunction = z_enable ? CompareFunc(db_control >> 4) : MTLCompareFunctionAlways;
        d.depthWriteEnabled = z_enable && ((db_control >> 2) & 1);
        id<MTLDepthStencilState> st = [device newDepthStencilStateWithDescriptor:d];
        depth_states[db_control] = st;
        return st;
    }

    void Draw(const RegFile& r, const DrawCall& dc);
    void Flip(const FlipInfo& f);

    // DC_DEBUG_MEMTEST: reads `addr` through the shader memory path and
    // compares with what the CPU sees.
    void MemTest(u64 addr) {
        static bool done = false;
        if (done || !Readable(addr, 16)) {
            return;
        }
        done = true;
        std::string src = Shader::kMslPrelude;
        src += "kernel void memtest(device uint* gm0 [[buffer(1)]], device uint* gm1 [[buffer(2)]], "
               "constant DcMap& gmap [[buffer(3)]], constant ulong& addr [[buffer(4)]], device uint* out [[buffer(5)]]) {"
               " DcMem mem = {gm0, gm1, &gmap}; for (uint i = 0; i < 4; ++i) out[i] = dc_ld(mem, addr + i * 4);"
               " out[4] = gmap.count; out[5] = uint(gmap.split >> 20); }";
        NSError* err = nil;
        id<MTLLibrary> lib = [device newLibraryWithSource:@(src.c_str()) options:nil error:&err];
        id<MTLComputePipelineState> cp = [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"memtest"] error:&err];
        if (!cp) {
            DC_ERROR("memtest: %s", err.localizedDescription.UTF8String);
            return;
        }
        UpdateMemMap();
        id<MTLBuffer> out = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> c = [queue commandBuffer];
        id<MTLComputeCommandEncoder> e = [c computeCommandEncoder];
        [e setComputePipelineState:cp];
        [e setBuffer:mem[0] offset:0 atIndex:1];
        [e setBuffer:mem[1] offset:0 atIndex:2];
        [e setBuffer:mem_map offset:0 atIndex:3];
        [e setBytes:&addr length:8 atIndex:4];
        [e setBuffer:out offset:0 atIndex:5];
        [e dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [e endEncoding];
        [c commit];
        [c waitUntilCompleted];
        const u32* g = static_cast<const u32*>(out.contents);
        const u32* cpu = reinterpret_cast<const u32*>(addr);
        DC_INFO("memtest %#llx: gpu %08x %08x %08x %08x | cpu %08x %08x %08x %08x | ranges %u split %u MB (%s)",
                addr, g[0], g[1], g[2], g[3], cpu[0], cpu[1], cpu[2], cpu[3], g[4], g[5],
                c.error ? c.error.localizedDescription.UTF8String : "ok");
    }

    // Reads a presented frame back and writes it as a PPM (debug only).
    void DumpFrame(id<MTLTexture> tex) {
        const u32 w = static_cast<u32>(tex.width), h = static_cast<u32>(tex.height);
        id<MTLBuffer> buf = [device newBufferWithLength:u64(w) * h * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
        [b copyFromTexture:tex
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:buf
               destinationOffset:0
          destinationBytesPerRow:w * 4
        destinationBytesPerImage:u64(w) * h * 4];
        [b endEncoding];
        Commit(true);
        EnsureCommandBuffer();
        char path[512];
        std::snprintf(path, sizeof(path), "%s/frame_%04llu.ppm", dump_frames, frame_count);
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << w << " " << h << "\n255\n";
        const auto* px = static_cast<const u8*>(buf.contents);
        std::vector<u8> rgb(u64(w) * h * 3);
        for (u64 i = 0; i < u64(w) * h; ++i) { // BGRA -> RGB
            rgb[i * 3] = px[i * 4 + 2];
            rgb[i * 3 + 1] = px[i * 4 + 1];
            rgb[i * 3 + 2] = px[i * 4];
        }
        out.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    }
};

void MetalRenderer::Impl::Draw(const RegFile& r, const DrawCall& dc) {
    ++draw_count;
    const u64 vs_addr = r.ProgramAddr(Reg::SPI_SHADER_PGM_LO_VS);
    const u64 ps_addr = r.ProgramAddr(Reg::SPI_SHADER_PGM_LO_PS);
    const u32 cb_mode = Bits(r[Reg::CB_COLOR_CONTROL], 4, 3);
    const u32 db_render = r[Reg::DB_RENDER_CONTROL];
    const u32 db_control = r[Reg::DB_DEPTH_CONTROL];
    const PrimType prim = r.Primitive();

    if (trace) {
        DC_INFO("gpu: draw #%llu prim=%#x count=%u idx=%d vs=%#llx ps=%#llx cb_mode=%u db_render=%#x "
                "db_ctl=%#x cb0=%#llx info=%#x mask=%#x",
                draw_count, static_cast<u32>(prim), dc.count, dc.indexed, vs_addr, ps_addr, cb_mode, db_render,
                db_control, u64(r.ColorTarget(0, Reg::CB_BASE)) << 8, r.ColorTarget(0, Reg::CB_INFO),
                r[Reg::CB_TARGET_MASK]);
    }
    if (!vs_addr) {
        return;
    }

    // ---- attachments ----
    RenderTarget* colors[8] = {};
    MTLPixelFormat color_fmt[8] = {};
    OutType out_type[8] = {};
    u32 col_format = ps_addr ? r[Reg::SPI_SHADER_COL_FORMAT] : 0;
    const u32 target_mask = r[Reg::CB_TARGET_MASK];
    bool any_color = false;
    if (cb_mode == 1 && ps_addr) {
        for (u32 k = 0; k < kNumColorTargets; ++k) {
            const u32 info = r.ColorTarget(k, Reg::CB_INFO);
            const u64 base = u64(r.ColorTarget(k, Reg::CB_BASE)) << 8;
            const u32 fmt = ColorInfo::Format(info);
            if (!fmt || !base || !((target_mask >> (4 * k)) & 0xF) || !((col_format >> (4 * k)) & 0xF)) {
                col_format &= ~(0xFu << (4 * k));
                continue;
            }
            const u32 pitch = ((r.ColorTarget(k, Reg::CB_PITCH) & 0x7FF) + 1) * 8;
            const u32 slice = ((r.ColorTarget(k, Reg::CB_SLICE) & 0x3FFFFF) + 1) * 64;
            const u32 height = slice / pitch;
            const ColorFormatInfo cf = ColorTargetFormat(fmt, ColorInfo::NumberType(info));
            colors[k] = &GetColorTarget(base, cf.format, pitch, height);
            color_fmt[k] = cf.format;
            out_type[k] = cf.out;
            any_color = true;
        }
    } else {
        col_format = 0;
    }

    RenderTarget* depth = nullptr;
    const u32 z_info = r[Reg::DB_Z_INFO];
    const u32 zfmt = z_info & 3;
    const bool depth_clear = db_render & 1;
    const bool z_used = ((db_control >> 1) & 1) || depth_clear;
    MTLPixelFormat depth_fmt = MTLPixelFormatInvalid;
    if (zfmt && z_used) {
        const u64 zbase = u64(r[Reg::DB_Z_WRITE_BASE]) << 8;
        const u32 size = r[Reg::DB_DEPTH_SIZE];
        u32 w = ((size & 0x7FF) + 1) * 8, h = (((size >> 11) & 0x7FF) + 1) * 8;
        for (u32 k = 0; k < 8; ++k) { // Metal wants equally sized attachments
            if (colors[k]) {
                w = colors[k]->width;
                h = colors[k]->height;
                break;
            }
        }
        depth_fmt = zfmt == 1 ? MTLPixelFormatDepth16Unorm : MTLPixelFormatDepth32Float;
        if (zbase) {
            depth = &GetDepthTarget(zbase, depth_fmt, w, h);
        } else {
            depth_fmt = MTLPixelFormatInvalid;
        }
    }
    if (!any_color && !depth) {
        if (trace) DC_INFO("gpu:   skipped: no attachments");
        return;
    }

    PassState want;
    for (u32 k = 0; k < 8; ++k) {
        want.color[k] = colors[k] ? colors[k]->addr : 0;
    }
    want.depth = depth ? depth->addr : 0;
    if (depth_clear && depth) {
        BeginPass(colors, depth, true, r.F(Reg::DB_DEPTH_CLEAR));
        return; // the draw only exists to clear depth
    }
    if (!enc || !(want == pass)) {
        BeginPass(colors, depth, false, 1.0f);
    }

    // ---- shaders ----
    const u32* vs_ud = r.UserData(Reg::SPI_SHADER_USER_DATA_VS_0);
    const u32* ps_ud = r.UserData(Reg::SPI_SHADER_USER_DATA_PS_0);
    Program* vs = GetProgram(Stage::Vertex, vs_addr, vs_ud, Bits(r[Reg::SPI_SHADER_PGM_RSRC2_VS], 1, 5),
                             Bits(r[Reg::SPI_SHADER_PGM_RSRC1_VS], 24, 2));
    Program* ps = ps_addr && any_color ? GetProgram(Stage::Fragment, ps_addr, ps_ud,
                                                    Bits(r[Reg::SPI_SHADER_PGM_RSRC2_PS], 1, 5), 0)
                                       : nullptr;
    if (!vs || (ps_addr && any_color && !ps)) {
        if (trace) DC_INFO("gpu:   skipped: shader decode failed");
        return;
    }

    // ---- resources ----
    struct Bound {
        id<MTLTexture> tex;
        id<MTLSamplerState> smp;
    };
    auto resolve = [&](const Program& p, const u32* ud, std::vector<TextureType>& types, std::vector<Bound>& out) {
        for (const auto& img : p.images) {
            u32 t[8], s[4] = {};
            for (u32 k = 0; k < 8; ++k) {
                t[k] = Shader::ResolveSgpr(img.tsharp[k], ud);
            }
            if (img.has_sampler) {
                for (u32 k = 0; k < 4; ++k) {
                    s[k] = Shader::ResolveSgpr(img.ssharp[k], ud);
                }
            }
            TextureType tt;
            id<MTLTexture> tex = GetTexture(t, &tt);
            types.push_back(tt);
            out.push_back({tex, img.has_sampler ? GetSampler(s) : nil});
        }
    };
    std::vector<TextureType> vs_types, ps_types;
    std::vector<Bound> vs_res, ps_res;
    resolve(*vs, vs_ud, vs_types, vs_res);
    if (ps) {
        resolve(*ps, ps_ud, ps_types, ps_res);
    }

    // ---- functions ----
    const bool rect = prim == PrimType::RectList;
    Shader::MslOptions vo;
    vo.rect_list = rect;
    vo.textures = vs_types;
    u64 vs_key = Hash(vs->hash);
    vs_key = Hash(rect, vs_key);
    vs_key = Fnv(vs_types.data(), vs_types.size() * sizeof(TextureType), vs_key);
    id<MTLFunction> vfn = GetFunction(*vs, vo, vs_key);

    id<MTLFunction> ffn = nil;
    u64 ps_key = 0;
    if (ps) {
        Shader::MslOptions fo;
        for (u32 k = 0; k < 32; ++k) {
            fo.ps_input_cntl[k] = r[Reg::SPI_PS_INPUT_CNTL_0 + k];
        }
        fo.vs_param_mask = vs->param_mask;
        fo.ps_input_addr = r[Reg::SPI_PS_INPUT_ADDR];
        fo.col_format = col_format;
        for (u32 k = 0; k < 8; ++k) {
            fo.mrt_type[k] = out_type[k];
        }
        fo.textures = ps_types;
        ps_key = Hash(ps->hash);
        ps_key = Fnv(fo.ps_input_cntl.data(), sizeof(fo.ps_input_cntl), ps_key);
        ps_key = Hash(fo.vs_param_mask, ps_key);
        ps_key = Hash(fo.ps_input_addr, ps_key);
        ps_key = Hash(fo.col_format, ps_key);
        ps_key = Fnv(fo.mrt_type.data(), sizeof(fo.mrt_type), ps_key);
        ps_key = Fnv(ps_types.data(), ps_types.size() * sizeof(TextureType), ps_key);
        ffn = GetFunction(*ps, fo, ps_key);
    }
    if (!vfn || (ps && !ffn)) {
        if (trace) DC_INFO("gpu:   skipped: shader compile failed");
        return;
    }

    // ---- pipeline ----
    PipelineKey pk;
    pk.vs = vs_key;
    pk.ps = ps_key;
    for (u32 k = 0; k < 8; ++k) {
        pk.color_fmt[k] = static_cast<u32>(color_fmt[k]);
        pk.blend[k] = colors[k] ? r[Reg::CB_BLEND0_CONTROL + k] : 0;
    }
    pk.write_mask = target_mask;
    pk.depth_fmt = static_cast<u32>(depth_fmt);
    const u64 pkey = Hash(pk);
    id<MTLRenderPipelineState> pso = nil;
    if (auto it = pipelines.find(pkey); it != pipelines.end()) {
        pso = it->second;
    } else {
        MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vfn;
        d.fragmentFunction = debug_fs && ffn && out_type[0] == OutType::Float ? debug_fs : ffn;
        for (u32 k = 0; k < 8; ++k) {
            if (!colors[k]) {
                continue;
            }
            MTLRenderPipelineColorAttachmentDescriptor* ca = d.colorAttachments[k];
            ca.pixelFormat = color_fmt[k];
            const u32 b = pk.blend[k];
            const u32 wm = (target_mask >> (4 * k)) & 0xF;
            ca.writeMask = ((wm & 1) ? MTLColorWriteMaskRed : 0) | ((wm & 2) ? MTLColorWriteMaskGreen : 0) |
                           ((wm & 4) ? MTLColorWriteMaskBlue : 0) | ((wm & 8) ? MTLColorWriteMaskAlpha : 0);
            const bool blendable = out_type[k] == OutType::Float && color_fmt[k] != MTLPixelFormatRGBA32Float;
            if ((b >> 30) & 1 && blendable && !debug_fs) {
                ca.blendingEnabled = YES;
                ca.sourceRGBBlendFactor = BlendFactor(b & 0x1F);
                ca.rgbBlendOperation = BlendOp((b >> 5) & 7);
                ca.destinationRGBBlendFactor = BlendFactor((b >> 8) & 0x1F);
                if ((b >> 29) & 1) {
                    ca.sourceAlphaBlendFactor = BlendFactor((b >> 16) & 0x1F);
                    ca.alphaBlendOperation = BlendOp((b >> 21) & 7);
                    ca.destinationAlphaBlendFactor = BlendFactor((b >> 24) & 0x1F);
                } else {
                    ca.sourceAlphaBlendFactor = ca.sourceRGBBlendFactor;
                    ca.alphaBlendOperation = ca.rgbBlendOperation;
                    ca.destinationAlphaBlendFactor = ca.destinationRGBBlendFactor;
                }
            }
        }
        d.depthAttachmentPixelFormat = depth_fmt;
        NSError* err = nil;
        pso = [device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pso) {
            DC_ERROR("gpu: pipeline (vs %#llx ps %#llx): %s", vs_addr, ps_addr,
                     err ? err.localizedDescription.UTF8String : "?");
        }
        pipelines[pkey] = pso;
    }
    if (!pso) {
        if (trace) DC_INFO("gpu:   skipped: no pipeline");
        return;
    }

    // ---- fixed function ----
    Rect sc{0, 0, static_cast<s32>(pass_width), static_cast<s32>(pass_height)};
    sc = Intersect(sc, ScissorReg(r[Reg::PA_SC_SCREEN_SCISSOR_TL], r[Reg::PA_SC_SCREEN_SCISSOR_BR]));
    sc = Intersect(sc, ScissorReg(r[Reg::PA_SC_WINDOW_SCISSOR_TL], r[Reg::PA_SC_WINDOW_SCISSOR_BR]));
    sc = Intersect(sc, ScissorReg(r[Reg::PA_SC_GENERIC_SCISSOR_TL], r[Reg::PA_SC_GENERIC_SCISSOR_BR]));
    sc = Intersect(sc, ScissorReg(r[Reg::PA_SC_VPORT_SCISSOR_0_TL], r[Reg::PA_SC_VPORT_SCISSOR_0_TL + 1]));
    if (trace) {
        DC_INFO("gpu:   vte=%#x vp=[%g %g | %g %g | %g %g] scissor=(%d,%d)-(%d,%d) [scr %#x %#x win %#x %#x gen %#x %#x vp %#x %#x mode0 %#x] pass=%ux%u clip=%#x su=%#x",
                r[Reg::PA_CL_VTE_CNTL], r.F(Reg::PA_CL_VPORT_XSCALE), r.F(Reg::PA_CL_VPORT_XSCALE + 1),
                r.F(Reg::PA_CL_VPORT_XSCALE + 2), r.F(Reg::PA_CL_VPORT_XSCALE + 3), r.F(Reg::PA_CL_VPORT_XSCALE + 4),
                r.F(Reg::PA_CL_VPORT_XSCALE + 5), sc.x0, sc.y0, sc.x1, sc.y1, r[Reg::PA_SC_SCREEN_SCISSOR_TL],
                r[Reg::PA_SC_SCREEN_SCISSOR_BR], r[Reg::PA_SC_WINDOW_SCISSOR_TL], r[Reg::PA_SC_WINDOW_SCISSOR_BR],
                r[Reg::PA_SC_GENERIC_SCISSOR_TL], r[Reg::PA_SC_GENERIC_SCISSOR_BR], r[Reg::PA_SC_VPORT_SCISSOR_0_TL],
                r[Reg::PA_SC_VPORT_SCISSOR_0_TL + 1], r[0xA292], pass_width, pass_height,
                r[Reg::PA_CL_CLIP_CNTL], r[Reg::PA_SU_SC_MODE_CNTL]);
    }
    if (sc.x1 <= sc.x0 || sc.y1 <= sc.y0) {
        return;
    }

    [enc setRenderPipelineState:pso];
    [enc setViewport:MTLViewport{0, 0, static_cast<double>(pass_width), static_cast<double>(pass_height), 0, 1}];
    [enc setScissorRect:MTLScissorRect{static_cast<NSUInteger>(sc.x0), static_cast<NSUInteger>(sc.y0),
                                       static_cast<NSUInteger>(sc.x1 - sc.x0), static_cast<NSUInteger>(sc.y1 - sc.y0)}];
    if (depth) {
        [enc setDepthStencilState:GetDepthState(db_control & 0x7F)];
    }
    const u32 su = r[Reg::PA_SU_SC_MODE_CNTL];
    const bool cull_front = su & 1, cull_back = (su >> 1) & 1;
    [enc setCullMode:(cull_front && cull_back) ? MTLCullModeNone
                     : cull_front ? MTLCullModeFront
                     : cull_back ? MTLCullModeBack : MTLCullModeNone];
    [enc setFrontFacingWinding:((su >> 2) & 1) ? MTLWindingClockwise : MTLWindingCounterClockwise];
    [enc setBlendColorRed:r.F(Reg::CB_BLEND_RED)
                    green:r.F(Reg::CB_BLEND_RED + 1)
                     blue:r.F(Reg::CB_BLEND_RED + 2)
                    alpha:r.F(Reg::CB_BLEND_RED + 3)];

    Shader::GpuDrawConsts consts{};
    const u32 vte = r[Reg::PA_CL_VTE_CNTL];
    const float xs = r.F(Reg::PA_CL_VPORT_XSCALE), xo = r.F(Reg::PA_CL_VPORT_XSCALE + 1);
    const float ys = r.F(Reg::PA_CL_VPORT_XSCALE + 2), yo = r.F(Reg::PA_CL_VPORT_XSCALE + 3);
    const float zs = r.F(Reg::PA_CL_VPORT_XSCALE + 4), zo = r.F(Reg::PA_CL_VPORT_XSCALE + 5);
    consts.vp_scale[0] = (vte & 1) ? xs : 1.0f;
    consts.vp_offset[0] = (vte & 2) ? xo : 0.0f;
    consts.vp_scale[1] = (vte & 4) ? ys : 1.0f;
    consts.vp_offset[1] = (vte & 8) ? yo : 0.0f;
    consts.vp_scale[2] = (vte & 16) ? zs : 1.0f;
    consts.vp_offset[2] = (vte & 32) ? zo : 0.0f;
    consts.rt_scale[0] = 2.0f / static_cast<float>(pass_width);
    consts.rt_scale[1] = 2.0f / static_cast<float>(pass_height);
    consts.vte = vte;

    // ---- bindings ----
    if (std::getenv("DC_DEBUG_MEMTEST")) {
        MemTest(u64(vs_ud[4]) | (u64(vs_ud[5] & 0xFFFF) << 32));
    }
    UpdateMemMap();
    [enc setVertexBytes:vs_ud length:64 atIndex:Shader::Binding::UserData];
    [enc setVertexBuffer:mem[0] offset:0 atIndex:Shader::Binding::Mem0];
    [enc setVertexBuffer:mem[1] offset:0 atIndex:Shader::Binding::Mem1];
    [enc setVertexBuffer:mem_map offset:0 atIndex:Shader::Binding::MemMap];
    [enc setVertexBytes:&consts length:sizeof(consts) atIndex:Shader::Binding::Draw];
    for (u32 k = 0; k < vs_res.size(); ++k) {
        [enc setVertexTexture:vs_res[k].tex atIndex:k];
        if (vs_res[k].smp) {
            [enc setVertexSamplerState:vs_res[k].smp atIndex:k];
        }
    }
    if (ps) {
        [enc setFragmentBytes:ps_ud length:64 atIndex:Shader::Binding::UserData];
        [enc setFragmentBuffer:mem[0] offset:0 atIndex:Shader::Binding::Mem0];
        [enc setFragmentBuffer:mem[1] offset:0 atIndex:Shader::Binding::Mem1];
        [enc setFragmentBuffer:mem_map offset:0 atIndex:Shader::Binding::MemMap];
        for (u32 k = 0; k < ps_res.size(); ++k) {
            [enc setFragmentTexture:ps_res[k].tex atIndex:k];
            if (ps_res[k].smp) {
                [enc setFragmentSamplerState:ps_res[k].smp atIndex:k];
            }
        }
    }

    // ---- primitive assembly ----
    const u32 base_vertex = r[Reg::VGT_INDX_OFFSET];
    const NSUInteger instances = std::max<u32>(dc.instances, 1);
    MTLPrimitiveType mprim = MTLPrimitiveTypeTriangle;
    bool to_list = false; // fans and quads are rebuilt as triangle lists
    switch (prim) {
    case PrimType::PointList: mprim = MTLPrimitiveTypePoint; break;
    case PrimType::LineList: mprim = MTLPrimitiveTypeLine; break;
    case PrimType::LineStrip: mprim = MTLPrimitiveTypeLineStrip; break;
    case PrimType::TriList: mprim = MTLPrimitiveTypeTriangle; break;
    case PrimType::TriStrip: mprim = MTLPrimitiveTypeTriangleStrip; break;
    case PrimType::TriFan:
    case PrimType::Polygon:
    case PrimType::QuadList:
        to_list = true;
        break;
    case PrimType::RectList:
        if (dc.indexed) {
            ReportOnce("indexed RECTLIST draws are not supported");
            return;
        }
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:(dc.count / 3) * 6 instanceCount:instances];
        return;
    default:
        ReportOnce("primitive type " + std::to_string(static_cast<u32>(prim)) + " not supported");
        return;
    }

    if (!dc.indexed && !to_list) {
        [enc drawPrimitives:mprim vertexStart:base_vertex vertexCount:dc.count instanceCount:instances];
        return;
    }
    if (!dc.indexed && prim == PrimType::QuadList) {
        const u32 quads = std::min<u32>(dc.count / 4, 0x10000);
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:quads * 6
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:quad_indices
                 indexBufferOffset:0
                     instanceCount:instances
                        baseVertex:base_vertex
                      baseInstance:0];
        return;
    }

    // Gather this draw's indices (guest buffer or implicit 0..n-1).
    std::vector<u32> idx(dc.count);
    if (dc.indexed) {
        const u64 src = dc.index_base + u64(dc.index_offset) * dc.index_size;
        if (!Readable(src, u64(dc.count) * dc.index_size)) {
            ReportOnce("index buffer not mapped");
            return;
        }
        if (dc.index_size == 4) {
            std::memcpy(idx.data(), reinterpret_cast<const void*>(src), dc.count * 4ull);
        } else {
            const auto* s16 = reinterpret_cast<const u16*>(src);
            for (u32 k = 0; k < dc.count; ++k) {
                idx[k] = s16[k];
            }
        }
    } else {
        for (u32 k = 0; k < dc.count; ++k) {
            idx[k] = k;
        }
    }
    if (to_list) {
        std::vector<u32> list;
        if (prim == PrimType::QuadList) {
            for (u32 q = 0; q + 3 < dc.count; q += 4) {
                list.insert(list.end(), {idx[q], idx[q + 1], idx[q + 2], idx[q], idx[q + 2], idx[q + 3]});
            }
        } else {
            for (u32 k = 1; k + 1 < dc.count; ++k) {
                list.insert(list.end(), {idx[0], idx[k], idx[k + 1]});
            }
        }
        idx.swap(list);
        mprim = MTLPrimitiveTypeTriangle;
    }
    if (idx.empty()) {
        return;
    }
    auto [buf, off] = Upload(idx.data(), idx.size() * 4);
    [enc drawIndexedPrimitives:mprim
                    indexCount:idx.size()
                     indexType:MTLIndexTypeUInt32
                   indexBuffer:buf
             indexBufferOffset:off
                 instanceCount:instances
                    baseVertex:base_vertex
                  baseInstance:0];
}

void MetalRenderer::Impl::Flip(const FlipInfo& f) {
    ++frame_count;
    EndPass();
    EnsureCommandBuffer();

    __strong id<MTLTexture>& dst = frames[frame_index];
    if (!dst || dst.width != f.width || dst.height != f.height) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:f.width
                                                                                    height:f.height
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModePrivate;
        dst = [device newTextureWithDescriptor:d];
    }

    id<MTLTexture> src = nil;
    float scale[2] = {1.0f, 1.0f};
    auto rt = color_targets.find(f.address);
    if (rt != color_targets.end() && rt->second.tex) {
        id<MTLTexture> t = rt->second.tex;
        // Scan-out shows the stored bytes: sample sRGB targets without decoding.
        if (t.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB) {
            t = [t newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
        }
        src = t;
        scale[0] = static_cast<float>(f.width) / static_cast<float>(rt->second.width);
        scale[1] = static_cast<float>(f.height) / static_cast<float>(rt->second.height);
    } else if (Readable(f.address, u64(f.pitch) * f.height * 4)) {
        // Nothing rendered there on the GPU: the CPU wrote the image.
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:f.width
                                                                                    height:f.height
                                                                                 mipmapped:NO];
        d.storageMode = MTLStorageModeManaged;
        d.usage = MTLTextureUsageShaderRead;
        src = [device newTextureWithDescriptor:d];
        [src replaceRegion:MTLRegionMake2D(0, 0, f.width, f.height)
               mipmapLevel:0
                 withBytes:reinterpret_cast<const void*>(f.address)
               bytesPerRow:f.pitch * 4];
    }
    if (src && copy_pipeline) {
        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = dst;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cmd renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:copy_pipeline];
        [e setFragmentTexture:src atIndex:0];
        [e setFragmentBytes:scale length:sizeof(scale) atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    if (src && dump_frames) {
        DumpFrame(dst);
    }
    Commit(false);
    if (src) {
        latest_frame.store((__bridge void*)dst, std::memory_order_release);
        frame_index = (frame_index + 1) % 3;
    }
    if (frame_count <= 3 || frame_count % 300 == 0) {
        DC_INFO("gpu: flip #%llu %#llx %ux%u (%s), %llu draws so far", frame_count, f.address, f.width, f.height,
                rt != color_targets.end() ? "gpu target" : "cpu memory", draw_count);
    }
}

MetalRenderer::MetalRenderer(Device& device) : impl(std::make_unique<Impl>()) {
    impl->device = (__bridge id<MTLDevice>)device.Handle();
    impl->queue = (__bridge id<MTLCommandQueue>)device.Queue();
    impl->Init();
}

MetalRenderer::~MetalRenderer() {
    std::scoped_lock lk(impl->lock);
    impl->Commit(true);
}

void MetalRenderer::Draw(const RegFile& regs, const DrawCall& draw) {
    std::scoped_lock lk(impl->lock);
    @autoreleasepool {
        impl->Draw(regs, draw);
    }
}

void MetalRenderer::Dispatch(const RegFile& regs, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    std::scoped_lock lk(impl->lock);
    impl->ReportOnce("compute dispatches are not implemented yet");
}

void MetalRenderer::OnSubmit() {
    std::scoped_lock lk(impl->lock);
    @autoreleasepool {
        impl->Commit(false);
    }
}

void MetalRenderer::WaitIdle() {
    std::scoped_lock lk(impl->lock);
    @autoreleasepool {
        impl->Commit(true);
    }
}

void MetalRenderer::Flip(const FlipInfo& flip) {
    std::scoped_lock lk(impl->lock);
    @autoreleasepool {
        impl->Flip(flip);
    }
}

void* MetalRenderer::LatestFrame() const {
    return impl->latest_frame.load(std::memory_order_acquire);
}

} // namespace Gpu::Metal
