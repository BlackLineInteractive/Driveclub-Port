#include "gpu/metal/mtl_renderer.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/log.h"
#include "core/memory.h"
#include "gpu/liverpool/pm4.h"
#include "gpu/liverpool/registers.h"
#include "gpu/liverpool/tiling.h"
#include "gpu/metal/mtl_compiler.h"
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
constexpr u64 kGpuAddrMask =
    (1ull << 40) - 1; // Liverpool virtual addresses are 40 bits
constexpr u32 kNopSizeHint = 0xC0001000; // one-dword NOP carrying a target size

u64 Fnv(const void *data, size_t size, u64 h = 0xcbf29ce484222325ull) {
  const auto *p = static_cast<const u8 *>(data);
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

std::string Hex(u64 v) {
  char b[24];
  std::snprintf(b, sizeof(b), "%#llx", static_cast<unsigned long long>(v));
  return b;
}

template <typename T> u64 Hash(const T &v, u64 seed = 0xcbf29ce484222325ull) {
  return Fnv(&v, sizeof(T), seed);
}

bool Readable(u64 addr, u64 size) {
  Core::Memory::Vma vma;
  return addr && Core::Memory::Query(addr, false, &vma) &&
         addr + size <= vma.start + vma.size;
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
  return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1),
          std::min(a.y1, b.y1)};
}

float Half(u32 h) {
  const u32 e = (h >> 10) & 0x1F, m = h & 0x3FF;
  const float v = e == 0    ? std::ldexp(static_cast<float>(m), -24)
                  : e == 31 ? (m ? NAN : INFINITY)
                            : std::ldexp(static_cast<float>(m | 0x400),
                                         static_cast<int>(e) - 25);
  return (h & 0x8000) ? -v : v;
}

// Unsigned small float with a 5-bit exponent and `mbits` of mantissa
// (R11G11B10).
float SmallFloat(u32 v, u32 mbits) {
  const u32 e = (v >> mbits) & 0x1F, m = v & ((1u << mbits) - 1);
  if (e == 0) {
    return std::ldexp(static_cast<float>(m), -14 - static_cast<int>(mbits));
  }
  return e == 31
             ? INFINITY
             : std::ldexp(static_cast<float>(m | (1u << mbits)),
                          static_cast<int>(e) - 15 - static_cast<int>(mbits));
}

// CB_COLOR_CLEAR_WORD0/1 (the target's packed format) as a Metal clear color.
MTLClearColor ClearColor(u32 info, u32 w0, u32 w1) {
  const u32 fmt = ColorInfo::Format(info), nt = ColorInfo::NumberType(info);
  const bool snorm = nt == NumFormat::Snorm,
             integer = nt == NumFormat::Uint || nt == NumFormat::Sint;
  auto n8 = [&](u32 v) {
    return integer ? double(v & 0xFF)
           : snorm ? std::max(-1.0, s8(v) / 127.0)
                   : (v & 0xFF) / 255.0;
  };
  auto n16 = [&](u32 v) {
    v &= 0xFFFF;
    return nt == NumFormat::Float ? double(Half(v))
           : integer              ? double(v)
           : snorm                ? std::max(-1.0, s16(v) / 32767.0)
                                  : v / 65535.0;
  };
  auto f32 = [&](u32 v) {
    float f;
    std::memcpy(&f, &v, 4);
    return integer ? double(v) : double(f);
  };
  double c[4] = {0, 0, 0, integer ? 1.0 : 1.0};
  switch (fmt) {
  case ColorFormat::C8:
    c[0] = n8(w0);
    break;
  case ColorFormat::C8_8:
    c[0] = n8(w0);
    c[1] = n8(w0 >> 8);
    break;
  case ColorFormat::C8_8_8_8:
    for (u32 k = 0; k < 4; ++k) {
      c[k] = n8(w0 >> (8 * k));
    }
    if (ColorInfo::CompSwap(info) == 1) { // BGRA in memory
      std::swap(c[0], c[2]);
    }
    break;
  case ColorFormat::C16:
    c[0] = n16(w0);
    break;
  case ColorFormat::C16_16:
    c[0] = n16(w0);
    c[1] = n16(w0 >> 16);
    break;
  case ColorFormat::C16_16_16_16:
    c[0] = n16(w0);
    c[1] = n16(w0 >> 16);
    c[2] = n16(w1);
    c[3] = n16(w1 >> 16);
    break;
  case ColorFormat::C32:
    c[0] = f32(w0);
    break;
  case ColorFormat::C32_32:
    c[0] = f32(w0);
    c[1] = f32(w1);
    break;
  case ColorFormat::C10_11_11:
    c[0] = SmallFloat(w0 & 0x7FF, 6);
    c[1] = SmallFloat((w0 >> 11) & 0x7FF, 6);
    c[2] = SmallFloat(w0 >> 22, 5);
    break;
  case ColorFormat::C11_11_10:
    c[0] = SmallFloat(w0 & 0x3FF, 5);
    c[1] = SmallFloat((w0 >> 10) & 0x7FF, 6);
    c[2] = SmallFloat(w0 >> 21, 6);
    break;
  case ColorFormat::C2_10_10_10:
    c[0] = (w0 & 0x3FF) / 1023.0;
    c[1] = ((w0 >> 10) & 0x3FF) / 1023.0;
    c[2] = ((w0 >> 20) & 0x3FF) / 1023.0;
    c[3] = (w0 >> 30) / 3.0;
    break;
  default:
    break;
  }
  return MTLClearColorMake(c[0], c[1], c[2], c[3]);
}

struct PipelineKey {
  u64 vs = 0;
  u64 ps = 0;
  u32 color_fmt[8] = {};
  u32 blend[8] = {};
  u32 write_mask = 0;
  u32 depth_fmt = 0;
  u32 shader_mask = 0;
};

constexpr const char *kCopyShader = R"(
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
fragment float4 dump_fs(V in [[stage_in]], texture2d<float> src [[texture(0)]], constant float2& opt [[buffer(1)]]) {
    constexpr sampler s(filter::nearest, address::clamp_to_edge);
    float3 c = (opt.x != 0.0 ? float3(src.sample(s, in.uv).a) * 0.5 : src.sample(s, in.uv).rgb) * opt.y;
    if (any(isnan(c)) || any(isinf(c))) return float4(1.0, 0.0, 1.0, 1.0);
    return float4(max(c, 0.0) / (1.0 + max(c, 0.0)) * 2.0, 1.0);
}
kernel void fill32(device uint* dst [[buffer(0)]], constant uint2& vc [[buffer(1)]],
                   uint i [[thread_position_in_grid]]) {
    if (i < vc.y) dst[i] = vc.x;
}
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
  std::vector<Core::Memory::DirectRange> mem_ranges; // sorted by start
  id<MTLComputePipelineState> fill_pipeline = nil;

  // ---- recording ----
  id<MTLCommandBuffer> cmd = nil;
  id<MTLCommandBuffer> last_committed = nil;
  id<MTLRenderCommandEncoder> enc = nil;
  struct PassState {
    u64 color[8] = {};
    u32 slice[8] = {};
    u64 depth = 0;
    bool operator==(const PassState &) const = default;
  } pass;
  u32 pass_width = 0, pass_height = 0;
  std::vector<id<MTLBuffer>> chunks;
  id<MTLBuffer> chunk = nil;
  u64 chunk_off = 0;
  u32 work_since_commit = 0; // draws + dispatches in the open command buffer
  static constexpr u32 kCommitEvery = 400;

  // Signals: an event value encoded between passes, so fences do not cut
  // command buffers. Callbacks run in order on the listener's queue.
  id<MTLSharedEvent> signal_event = nil;
  MTLSharedEventListener *signal_listener = nil;
  u64 signal_value = 0;
  std::mutex signal_lock;
  std::deque<std::pair<u64, std::function<void()>>> signal_queue;

  // ---- caches ----
  struct ShaderEntry {
    std::unique_ptr<Program> prog;
    u32 head[4] = {};
  };
  std::unordered_map<u64, std::vector<ShaderEntry>> shaders;
  std::unordered_map<u64, id<MTLRenderPipelineState>> pipelines;

  // Pipelines compile on `compiler`'s threads; draws whose pipeline is not
  // ready yet are skipped (DC_SYNC_SHADERS waits instead), dispatches wait.
  std::unique_ptr<ShaderCompiler> compiler;
  bool sync_shaders = false;
  std::unordered_set<u64> pending;
  std::mutex done_lock;
  std::condition_variable done_cv;
  std::vector<std::pair<u64, id<MTLRenderPipelineState>>> done_render;
  std::vector<std::pair<u64, id<MTLComputePipelineState>>> done_compute;
  std::unordered_map<u64, id<MTLDepthStencilState>> depth_states;
  std::unordered_map<u64, id<MTLSamplerState>> samplers;

  // Guest surfaces rendered on the host GPU stay there. The same address can
  // be bound with different sizes or formats (aliased memory); each variant
  // keeps its own texture, and readers get the one written last.
  struct RenderTarget {
    id<MTLTexture> tex = nil;
    u64 addr = 0;
    u32 width = 0, height = 0;
    u32 depth_or_layers = 1;
    TexDim dim = TexDim::Tex2D;
    bool initialized = false;
    id<MTLTexture> bounds_copy =
        nil; // depth as of bounds_seq, for the depth bounds test
    u64 bounds_seq = 0;
    bool clear_pending = false; // fast-cleared (CMASK/HTILE): next pass clears
    MTLClearColor clear_color{};
    double clear_depth = 1.0;
    u32 clear_stencil = 0;
    u64 written = 0;    // write_seq of the last pass/dispatch writing it
    u32 bind_slice = 0; // array slice the CB registers point at (CB_COLOR_VIEW)
    u32 bind_level = 0; // mip level, when the CB address is a level of this one
    u32 levels = 1; // mip levels (compute shaders build mip chains in place)
    std::unordered_map<u64, id<MTLTexture>>
        views; // (base, count, format) -> view
  };
  using TargetMap =
      std::unordered_map<u64, std::vector<std::unique_ptr<RenderTarget>>>;
  TargetMap color_targets; // by address
  TargetMap depth_targets;
  u64 write_seq = 0;

  // Fast-clear metadata surfaces (CMASK / HTILE) -> the surface they describe.
  struct Meta {
    u64 target = 0;
    bool depth = false;
  };
  std::unordered_map<u64, Meta> metas;

  // Render targets whose address is a mip level of a texture's memory (a T#
  // with levels): the game renders straight into that level, e.g. the 256x256
  // cubes of level 1 of the 512x512 environment cube array. Like shadPS4's
  // ImageInfo::MipOf, such a target is that level of the larger image.
  struct MipAlias {
    u64 parent = 0;
    u32 level = 0;
    u32 width = 0, height = 0;
    MTLPixelFormat fmt = MTLPixelFormatInvalid;
  };
  std::unordered_map<u64, MipAlias> mip_alias; // by level address
  std::vector<std::unique_ptr<RenderTarget>> retired; // merged into a parent

  // Render target sizes the Gnm driver hides in a NOP after the CB/DB
  // register writes (width | height << 16); 0 when absent.
  u32 cb_extent[8] = {};
  u32 db_extent = 0;

  // Guest textures are re-uploaded when their memory changes. Each keeps
  // hashes of its 64 KB chunks; a texture that never changed has one chunk
  // checked per frame in turn, one that did (glyph caches, streamed or
  // CPU-animated textures) is checked in full every frame it is used.
  static constexpr u64 kChunk = 64 << 10;
  struct TextureEntry {
    id<MTLTexture> tex = nil;
    id<MTLTexture> base_view = nil; // levels from the T# BASE_LEVEL
    std::vector<u64> chunks;
    u32 next_chunk = 0;
    bool hot = false;
    u64 frame = 0;
    u64 addr = 0;
    u64 size = 0;
  };
  std::unordered_map<u64, TextureEntry> textures;
  std::unordered_map<u32, id<MTLTexture>> dummy_textures;
  id<MTLSamplerState> default_sampler = nil;
  id<MTLBuffer> quad_indices = nil;
  id<MTLBuffer> gds = nil; // 64 KB global data share

  // ---- constant-engine snapshots ----
  // The CE dumps resource tables into a small ring that the guest rewrites
  // while draws that read it may still be queued on the host GPU. A draw
  // whose user data points into a dump gets a pointer to a copy in `arena`
  // (guest memory the renderer owns) taken when the draw is recorded.
  struct ConstDump {
    u32 bytes = 0;
    u64 gen = 0;
  };
  std::map<u64, ConstDump> const_dumps; // by guest address
  u64 dump_gen = 0;
  struct Snapshot {
    u64 gen = 0;
    u64 va = 0;
  };
  std::unordered_map<u64, Snapshot>
      snapshots; // dump address -> its latest copy
  static constexpr u64 kArenaSize = 16ull << 20;
  u64 arena_va = 0;
  u64 arena_head = 0; // bytes handed out so far (wraps modulo kArenaSize)
  std::atomic<u64> arena_released{0}; // head values the GPU is done with
  u64 arena_commit_head = 0; // head when the last command buffer was committed

  // ---- presentation ----
  id<MTLRenderPipelineState> copy_pipeline = nil;
  id<MTLRenderPipelineState> dump_pipeline =
      nil; // DC_DUMP_PASSES: x / (1 + x), NaN/Inf magenta
  id<MTLTexture> frames[3] = {nil, nil, nil};
  u32 frame_index = 0;
  std::atomic<void *> latest_frame{nullptr};
  u64 frame_count = 0;

  // ---- diagnostics ----
  const char *dump_dir = nullptr;
  const char *dump_frames = nullptr; // DC_DUMP_FRAMES=dir: flips as PPMs
  u64 dump_every = 1;                // DC_DUMP_EVERY=n: only every n-th flip
  bool debug_clear = false; // DC_DEBUG_CLEAR: new targets start dark blue
  u64 dump_texture =
      0; // DC_DUMP_TEXTURE=<addr>: write its uploads next to the frame dumps
  u64 debug_vsout = 0; // DC_DEBUG_VSOUT=<vs address>: dump its first positions
  id<MTLBuffer> debug_vs_buf = nil;
  id<MTLFunction> debug_fs =
      nil; // DC_DEBUG_FLAT: every pixel shader outputs magenta
  bool trace = false;
  u64 trace_frame = 0; // DC_TRACE_FRAME=n: DC_GPU_TRACE for flip n only
  bool dump_passes =
      false; // DC_DUMP_PASSES: with DC_TRACE_FRAME, every pass's targets
  struct PassDump {
    id<MTLBuffer> buf;
    u32 w, h;
    std::string name;
  };
  std::vector<PassDump> pass_dumps;
  std::vector<u64> peek_addrs; // DC_PEEK_CS=<cs>: its V# buffers, printed after
                               // the traced frame
  // DC_PROBE=<shader>:<pc>:<x>,<y>: registers of one fragment (compute: group
  // x, thread y) before instruction pc, for every draw/dispatch of the traced
  // frame using that shader.
  struct Probe {
    u64 shader = 0;
    u32 pc = ~0u, x = 0, y = 0;
  } probe;
  id<MTLBuffer> probe_buf = nil;
  std::vector<std::string>
      probe_slots; // what each slot of probe_buf belongs to
  static constexpr u32 kProbeSlots = 4096;
  id<MTLTexture> pass_tex[9] = {};
  u64 pass_addr[9] = {};
  bool stats = false;   // DC_GPU_STATS: per-second GPU time and submit counts
  bool profile = false; // DC_GPU_PROFILE: GPU time per shader (one command
                        // buffer per call)
  struct ProfileEntry {
    double seconds = 0;
    u64 calls = 0;
  };
  std::mutex profile_lock;
  std::unordered_map<std::string, ProfileEntry> profile_data;
  std::atomic<u64> gpu_ns{0};
  u64 stat_hashed = 0;
  u64 stat_commits = 0, stat_waits = 0, stat_dispatches = 0, stat_draws = 0;
  double stat_wait_s = 0;
  std::chrono::steady_clock::time_point stat_t0 =
      std::chrono::steady_clock::now();
  u64 draw_count = 0;
  std::set<std::string> reported;

  void ReportOnce(const std::string &what) {
    if (reported.insert(what).second) {
      DC_WARN("gpu: %s", what.c_str());
    }
  }

  // ================= setup =================

  void Init() {
    dump_dir = std::getenv("DC_DUMP_SHADERS");
    dump_frames = std::getenv("DC_DUMP_FRAMES");
    if (const char *e = std::getenv("DC_DUMP_EVERY")) {
      dump_every = std::max<u64>(1, std::strtoull(e, nullptr, 10));
    }
    debug_clear = std::getenv("DC_DEBUG_CLEAR") != nullptr;
    if (const char *v = std::getenv("DC_DEBUG_VSOUT")) {
      debug_vsout = std::strtoull(v, nullptr, 16);
    }
    trace = std::getenv("DC_GPU_TRACE") != nullptr;
    dump_passes = std::getenv("DC_DUMP_PASSES") != nullptr;
    if (const char *e = std::getenv("DC_TRACE_FRAME")) {
      trace_frame = std::strtoull(e, nullptr, 10);
    }
    if (const char *e = std::getenv("DC_DUMP_TEXTURE")) {
      dump_texture = std::strtoull(e, nullptr, 16);
    }
    sync_shaders = std::getenv("DC_SYNC_SHADERS") != nullptr;
    if (const char *e = std::getenv("DC_PROBE")) {
      char *q = nullptr;
      probe.shader = std::strtoull(e, &q, 16);
      if (!std::strcmp(
              e,
              "*:nan")) { // every pixel shader's first non-finite color export
        probe.shader = ~0ull;
        probe.pc = Shader::kProbeExportNan;
        probe.x = ~0u;
        q = const_cast<char *>(e + std::strlen(e));
      }
      if (*q == ':') {
        probe.pc = std::strtoul(q + 1, &q, 16);
      }
      // '*': any pixel; 'nan<k>': the first whose vk is NaN/Inf
      if (*q == ':' && q[1] == '*') {
        probe.x = ~0u;
      } else if (*q == ':' && !std::strncmp(q + 1, "nan", 3)) {
        probe.x = ~1u;
        probe.y = std::strtoul(q + 4, &q, 10);
      } else {
        probe.x = *q == ':' ? std::strtoul(q + 1, &q, 10) : 0;
      }
      if (*q == ',') {
        probe.y = std::strtoul(q + 1, &q, 10);
      }
      probe_buf =
          [device newBufferWithLength:kProbeSlots * Shader::kProbeWords * 4
                              options:MTLResourceStorageModeShared];
      std::memset(probe_buf.contents, 0, probe_buf.length);
    }
    stats = std::getenv("DC_GPU_STATS") != nullptr;
    profile = std::getenv("DC_GPU_PROFILE") != nullptr;
    compiler = std::make_unique<ShaderCompiler>(device, 4);
    signal_event = [device newSharedEvent];
    signal_listener = [[MTLSharedEventListener alloc]
        initWithDispatchQueue:dispatch_queue_create("gpu signals",
                                                    DISPATCH_QUEUE_SERIAL)];

    void *view = Core::Memory::DirectHostView();
    const u64 total = Core::Memory::kDirectSize;
    mem_split = (total / 2) & ~0xFFFFull;
    if (view) {
      mem[0] = [device newBufferWithBytesNoCopy:view
                                         length:mem_split
                                        options:MTLResourceStorageModeShared
                                    deallocator:nil];
      mem[1] =
          [device newBufferWithBytesNoCopy:static_cast<u8 *>(view) + mem_split
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

    gds = [device newBufferWithLength:0x10000
                              options:MTLResourceStorageModeShared];
    std::memset(gds.contents, 0, 0x10000);

    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
    sd.minFilter = MTLSamplerMinMagFilterLinear;
    sd.magFilter = MTLSamplerMinMagFilterLinear;
    default_sampler = [device newSamplerStateWithDescriptor:sd];

    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:@(kCopyShader)
                                              options:nil
                                                error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    if (std::getenv("DC_DEBUG_FLAT")) {
      debug_fs = [lib newFunctionWithName:@"debug_fs"];
    }
    pd.vertexFunction = [lib newFunctionWithName:@"copy_vs"];
    pd.fragmentFunction = [lib newFunctionWithName:@"copy_fs"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    copy_pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&err];
    pd.fragmentFunction = [lib newFunctionWithName:@"dump_fs"];
    dump_pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&err];
    fill_pipeline = [device
        newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill32"]
                                      error:&err];
    if (!copy_pipeline) {
      DC_ERROR("gpu: copy pipeline: %s", err.localizedDescription.UTF8String);
    }
  }

  // Metal compiles lazily and stalls the GPU thread meanwhile; name the slow
  // ones.
  static void ReportSlowCompile(const char *what, u64 a, u64 b,
                                std::chrono::steady_clock::time_point t0) {
    const double s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    if (s > 0.5) {
      DC_WARN("gpu: %s %#llx/%#llx took %.1f s to compile", what, a, b, s);
    }
  }

  void UpdateMemMap() {
    const u64 gen = Core::Memory::DirectMapGeneration();
    if (gen == mem_map_gen && mem_map) {
      return;
    }
    mem_map_gen = gen;
    mem_ranges = Core::Memory::DirectMappings();
    std::sort(mem_ranges.begin(), mem_ranges.end(),
              [](const auto &x, const auto &y) { return x.start < y.start; });
    const auto &ranges = mem_ranges;
    // Largest page size at which no page holds two mappings.
    u32 shift = Shader::GpuMemMap::kMaxShift;
    for (; shift > Shader::GpuMemMap::kMinShift; --shift) {
      const u64 page = 1ull << shift;
      u64 prev_end = 0;
      bool ok = true;
      for (const auto &r : ranges) {
        ok = ok && (r.start & (page - 1)) == 0 &&
             ((prev_end + page - 1) & ~(page - 1)) <= r.start;
        prev_end = r.start + r.size;
      }
      if (ok) {
        break;
      }
    }
    const u64 page = 1ull << shift;
    const u64 lo = ranges.empty() ? 0 : ranges.front().start & ~(page - 1);
    const u64 hi =
        ranges.empty() ? 0 : ranges.back().start + ranges.back().size;
    const u64 pages = (hi - lo + page - 1) >> shift;
    constexpr u64 header = sizeof(Shader::GpuMemMap) / 4;
    std::vector<s32> table(header + pages, Shader::GpuMemMap::kUnmapped);
    auto &map = *reinterpret_cast<Shader::GpuMemMap *>(table.data());
    map = {lo, pages, mem_split, shift, 0, Core::Memory::kDirectSize};
    for (const auto &r : ranges) {
      if (((r.start ^ r.phys) & ((1ull << Shader::GpuMemMap::kMinShift) - 1)) !=
          0) {
        ReportOnce("direct mapping not 16 KB aligned");
      }
      const s32 delta = static_cast<s32>(
          (static_cast<s64>(r.phys) - static_cast<s64>(r.start)) >>
          Shader::GpuMemMap::kMinShift);
      for (u64 va = r.start & ~(page - 1); va < r.start + r.size; va += page) {
        table[header + ((va - lo) >> shift)] = delta;
      }
    }
    // A fresh buffer: work already recorded keeps reading the old table.
    mem_map = [device newBufferWithBytes:table.data()
                                  length:table.size() * 4
                                 options:MTLResourceStorageModeShared];
  }

  // Guest range [va, va + size) as a range of one of the `mem` buffers.
  bool Locate(u64 va, u64 size, id<MTLBuffer> *buf, u64 *off) {
    UpdateMemMap();
    auto it =
        std::upper_bound(mem_ranges.begin(), mem_ranges.end(), va,
                         [](u64 a, const auto &r) { return a < r.start; });
    if (it == mem_ranges.begin()) {
      return false;
    }
    --it;
    if (va + size > it->start + it->size) {
      return false;
    }
    const u64 phys = it->phys + (va - it->start);
    if (phys + size <= mem_split) {
      *buf = mem[0];
      *off = phys;
    } else if (phys >= mem_split) {
      *buf = mem[1];
      *off = phys - mem_split;
    } else {
      return false;
    }
    return *buf != nil;
  }

  // CP DMA on the host GPU, in order with the work around it.
  bool DmaData(u32 dst_sel, u64 dst, u32 src_sel, u64 src, u32 bytes) {
    if (trace) {
      DC_INFO("gpu: dma %u:%#llx <- %u:%#llx, %u bytes", dst_sel, dst, src_sel,
              src, bytes);
    }
    if ((bytes & 3) || (dst & 3) || (src_sel != 2 && (src & 3))) {
      return false;
    }
    id<MTLBuffer> dbuf = nil, sbuf = nil;
    u64 doff = 0, soff = 0;
    if (dst_sel == 1) {
      if (dst + bytes > gds.length) {
        return false;
      }
      dbuf = gds;
      doff = dst;
    } else if (!Locate(dst, bytes, &dbuf, &doff)) {
      return false;
    }
    if (src_sel == 1) {
      if (src + bytes > gds.length) {
        return false;
      }
      sbuf = gds;
      soff = src;
    } else if (src_sel != 2 && !Locate(src, bytes, &sbuf, &soff)) {
      return false;
    }
    const u32 value = static_cast<u32>(src);
    if (src_sel == 2 && dst_sel != 1 && value != ~0u && !expanding_htile &&
        MarkMetaCleared(dst, bytes)) {
      EndPass(); // the next pass over the surface loads its clear value
    }
    EndPass();
    EnsureCommandBuffer();
    if (sbuf) {
      id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
      [b copyFromBuffer:sbuf
               sourceOffset:soff
                   toBuffer:dbuf
          destinationOffset:doff
                       size:bytes];
      [b endEncoding];
    } else if (value == (value & 0xFF) * 0x01010101u) {
      id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
      [b fillBuffer:dbuf
              range:NSMakeRange(doff, bytes)
              value:static_cast<u8>(value)];
      [b endEncoding];
    } else if (fill_pipeline) {
      const u32 vc[2] = {value, bytes / 4};
      id<MTLComputeCommandEncoder> e = [cmd computeCommandEncoder];
      [e setComputePipelineState:fill_pipeline];
      [e setBuffer:dbuf offset:doff atIndex:0];
      [e setBytes:vc length:sizeof(vc) atIndex:1];
      [e dispatchThreads:MTLSizeMake(bytes / 4, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
      [e endEncoding];
    } else {
      return false;
    }
    return true;
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
      if (dump_passes && trace && dump_frames) {
        DumpPass();
      }
    }
    pass = {};
  }

  // DC_DUMP_PASSES: small BGRA copies of the targets of the pass just ended.
  void DumpPass() {
    for (u32 k = 0; k < 9; ++k) {
      id<MTLTexture> t = pass_tex[k];
      pass_tex[k] = nil;
      if (!t || !copy_pipeline) {
        continue;
      }
      if (t.textureType ==
          MTLTextureType2DArray) { // each slice (cube faces) on its own
        for (NSUInteger sl = 0; sl < std::min<NSUInteger>(t.arrayLength, 12);
             ++sl) {
          DumpTexture([t newTextureViewWithPixelFormat:t.pixelFormat
                                           textureType:MTLTextureType2D
                                                levels:NSMakeRange(0, 1)
                                                slices:NSMakeRange(sl, 1)],
                      k, pass_addr[k]);
        }
      } else if (t.textureType == MTLTextureType2D) {
        DumpTexture(t, k, pass_addr[k]);
      }
    }
  }

  void DumpTexture(id<MTLTexture> t, u32 k, u64 addr) {
    const MTLPixelFormat f = t.pixelFormat;
    if (k < 8 &&
        (f == MTLPixelFormatRGBA8Uint || f == MTLPixelFormatR32Uint ||
         f == MTLPixelFormatRG32Uint || f == MTLPixelFormatRGBA32Uint ||
         f == MTLPixelFormatR16Uint || f == MTLPixelFormatR8Uint ||
         f == MTLPixelFormatRG16Uint || f == MTLPixelFormatRGBA16Uint)) {
      return;
    }
    const u32 w = std::max<u32>(1, static_cast<u32>(t.width) / 4),
              h = std::max<u32>(1, static_cast<u32>(t.height) / 4);
    MTLTextureDescriptor *d = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:w
                                    height:h
                                 mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> dst = [device newTextureWithDescriptor:d];
    MTLRenderPassDescriptor *rp =
        [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cmd renderCommandEncoderWithDescriptor:rp];
    const float scale[2] = {1, 1};
    [e setRenderPipelineState:dump_pipeline];
    // DC_DUMP_ALPHA: show alpha; DC_DUMP_GAIN=<x>: scale before the x/(1+x)
    // display curve
    static const float dump_opt[2] = {
        std::getenv("DC_DUMP_ALPHA") ? 1.0f : 0.0f,
        std::getenv("DC_DUMP_GAIN")
            ? std::strtof(std::getenv("DC_DUMP_GAIN"), nullptr)
            : 1.0f};
    [e setFragmentBytes:dump_opt length:sizeof(dump_opt) atIndex:1];
    [e setFragmentTexture:t atIndex:0];
    [e setFragmentBytes:scale length:sizeof(scale) atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    id<MTLBuffer> buf =
        [device newBufferWithLength:u64(w) * h * 4
                            options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
    [b copyFromTexture:dst
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(w, h, 1)
                        toBuffer:buf
               destinationOffset:0
          destinationBytesPerRow:w * 4
        destinationBytesPerImage:u64(w) * h * 4];
    [b endEncoding];
    char name[128];
    std::snprintf(name, sizeof(name), "pass_%04zu_%s%u_%llx_d%llu",
                  pass_dumps.size(), k == 8 ? "z" : "c", k, addr, draw_count);
    pass_dumps.push_back({buf, w, h, name});
  }

  void WritePassDumps() {
    for (const auto &pd : pass_dumps) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s/%s.ppm", dump_frames,
                    pd.name.c_str());
      std::ofstream out(path, std::ios::binary);
      out << "P6\n" << pd.w << " " << pd.h << "\n255\n";
      const auto *px = static_cast<const u8 *>(pd.buf.contents);
      for (u64 i = 0; i < u64(pd.w) * pd.h; ++i) {
        const char rgb[3] = {static_cast<char>(px[i * 4 + 2]),
                             static_cast<char>(px[i * 4 + 1]),
                             static_cast<char>(px[i * 4])};
        out.write(rgb, 3);
      }
    }
    pass_dumps.clear();
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
    if (arena_va) {
      const u64 head = arena_commit_head = arena_head;
      [cmd addCompletedHandler:^(id<MTLCommandBuffer>) {
        arena_released.store(head, std::memory_order_release);
      }];
    }
    if (stats) {
      ++stat_commits;
      [cmd addCompletedHandler:^(id<MTLCommandBuffer> c) {
        gpu_ns += static_cast<u64>((c.GPUEndTime - c.GPUStartTime) * 1e9);
      }];
    }
    [cmd commit];
    last_committed = cmd;
    cmd = nil;
    work_since_commit = 0;
    if (wait) {
      WaitLast();
    }
  }

  void NoteConstDump(u64 addr, u32 bytes) {
    // A new dump replaces whatever older dumps it overlaps.
    auto it = const_dumps.lower_bound(addr);
    if (it != const_dumps.begin() &&
        std::prev(it)->first + std::prev(it)->second.bytes > addr) {
      --it;
    }
    while (it != const_dumps.end() && it->first < addr + bytes) {
      it = const_dumps.erase(it);
    }
    const_dumps[addr] = {bytes, ++dump_gen};
  }

  bool InitArena() {
    u64 phys = 0;
    VAddr va = 0;
    if (Core::Memory::AllocateDirect(0, Core::Memory::kDirectSize, kArenaSize,
                                     64 << 10, 0, &phys) != 0 ||
        Core::Memory::MapDirect(
            &va, kArenaSize,
            Core::Memory::kProtCpuRead | Core::Memory::kProtCpuWrite |
                Core::Memory::kProtGpuRead,
            0, phys, 64 << 10, "gpu constant snapshots") != 0) {
      ReportOnce("cannot allocate the constant snapshot arena");
      return false;
    }
    arena_va = va;
    return true;
  }

  // Guest address of `bytes` of arena space the GPU is not using.
  u64 ArenaAlloc(u64 bytes) {
    bytes = (bytes + 255) & ~255ull;
    if (!arena_va && !InitArena()) {
      return 0;
    }
    u64 off = arena_head % kArenaSize;
    if (off + bytes > kArenaSize) {
      arena_head += kArenaSize - off; // do not split a copy across the wrap
      off = 0;
    }
    if (arena_head + bytes - arena_released.load(std::memory_order_acquire) >
        kArenaSize) {
      // Ring full of in-flight copies (rare): wait for what was committed.
      // A draw may be half recorded here, so the open command buffer stays.
      WaitLast();
      arena_released = arena_commit_head;
    }
    arena_head += bytes;
    return arena_va + off;
  }

  // Points user-data pointers that land in a CE dump at a snapshot of it.
  void PatchUserData(u32 *ud) {
    if (const_dumps.empty()) {
      return;
    }
    for (u32 k = 0; k + 1 < 16; ++k) {
      const u64 v =
          ud[k] | (u64(ud[k + 1] & 0xFFF) << 32); // pointer or V# base
      auto it = const_dumps.upper_bound(v);
      if (it == const_dumps.begin()) {
        continue;
      }
      --it;
      const u64 off = v - it->first;
      if (off >= it->second.bytes) {
        continue;
      }
      Snapshot &snap = snapshots[it->first];
      if (snap.gen != it->second.gen || !snap.va) {
        snap.va = ArenaAlloc(it->second.bytes);
        if (!snap.va) {
          return;
        }
        std::memcpy(reinterpret_cast<void *>(snap.va),
                    reinterpret_cast<const void *>(it->first),
                    it->second.bytes);
        snap.gen = it->second.gen;
      }
      const u64 nv = snap.va + off;
      ud[k] = static_cast<u32>(nv);
      ud[k + 1] = (ud[k + 1] & ~0xFFFu) | static_cast<u32>(nv >> 32);
      ++k;
    }
  }

  // DC_GPU_PROFILE: puts the work since the last cut in its own command
  // buffer and charges its GPU time to `what`.
  void ProfileCut(std::string what) {
    if (!cmd) {
      return;
    }
    EndPass();
    [cmd addCompletedHandler:^(id<MTLCommandBuffer> c) {
      std::scoped_lock lk(profile_lock);
      auto &e = profile_data[what];
      e.seconds += c.GPUEndTime - c.GPUStartTime;
      ++e.calls;
    }];
    Commit(false);
  }

  void ProfileReport() {
    std::vector<std::pair<std::string, ProfileEntry>> v;
    {
      std::scoped_lock lk(profile_lock);
      v.assign(profile_data.begin(), profile_data.end());
      profile_data.clear();
    }
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) {
      return a.second.seconds > b.second.seconds;
    });
    double total = 0;
    for (const auto &[k, e] : v) {
      total += e.seconds;
    }
    DC_INFO("gpu profile: %.1f ms GPU total", total * 1e3);
    for (size_t k = 0; k < v.size() && k < 15; ++k) {
      DC_INFO("gpu profile: %7.1f ms %6llu calls  %s",
              v[k].second.seconds * 1e3, (unsigned long long)v[k].second.calls,
              v[k].first.c_str());
    }
  }

  // Command buffers on one queue complete in order, and their completion
  // handlers run in order on Metal's serial completion queue.
  void Signal(std::function<void()> done) {
    EndPass();
    EnsureCommandBuffer();
    const u64 v = ++signal_value;
    {
      std::scoped_lock lk(signal_lock);
      signal_queue.emplace_back(v, std::move(done));
    }
    [cmd encodeSignalEvent:signal_event value:v];
    [signal_event notifyListener:signal_listener
                         atValue:v
                           block:^(id<MTLSharedEvent> e, uint64_t) {
                             RunSignals(e.signaledValue);
                           }];
  }

  // Listener queue: runs, in order, the callbacks of every value reached.
  void RunSignals(u64 reached) {
    for (;;) {
      std::function<void()> fn;
      {
        std::scoped_lock lk(signal_lock);
        if (signal_queue.empty() || signal_queue.front().first > reached) {
          return;
        }
        fn = std::move(signal_queue.front().second);
        signal_queue.pop_front();
      }
      fn();
    }
  }

  // Command buffers on one queue complete in order: the last one is enough.
  void WaitLast() {
    if (last_committed) {
      const auto t0 = std::chrono::steady_clock::now();
      [last_committed waitUntilCompleted];
      if (stats) {
        ++stat_waits;
        stat_wait_s +=
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
                .count();
      }
      last_committed = nil;
    }
  }

  // Bump-allocates transient GPU-visible memory for this command buffer.
  std::pair<id<MTLBuffer>, u64> Upload(const void *data, u64 size) {
    size = (size + 15) & ~15ull;
    if (!chunk || chunk_off + size > chunk.length) {
      if (chunk) {
        chunks.push_back(chunk);
      }
      chunk = [device newBufferWithLength:std::max(kUploadChunk, size)
                                  options:MTLResourceStorageModeShared];
      chunk_off = 0;
    }
    const u64 off = chunk_off;
    if (data) {
      std::memcpy(static_cast<u8 *>(chunk.contents) + off, data, size);
    }
    chunk_off += size;
    return {chunk, off};
  }

  // ================= shaders =================

  Program *GetProgram(Stage stage, u64 addr, const u32 *ud, u32 num_user_sgprs,
                      u32 vgpr_comp_cnt, u32 compute_rsrc2 = 0) {
    if (!Readable(addr, 16)) {
      return nullptr;
    }
    const u32 *code = reinterpret_cast<const u32 *>(addr);
    auto &list = shaders[addr ^ (static_cast<u64>(stage) << 60)];
    for (auto &e : list) {
      if (std::memcmp(e.head, code, sizeof(e.head)) != 0) {
        continue;
      }
      const Program &p = *e.prog;
      if (p.fetch_ud >= 0) {
        const u64 f = u64(ud[p.fetch_ud]) | (u64(ud[p.fetch_ud + 1]) << 32);
        if (f != p.fetch_address) {
          continue;
        }
      }
      return e.prog.get();
    }
    Shader::BuildInput in{stage, addr, ud, num_user_sgprs, vgpr_comp_cnt};
    in.compute_rsrc2 = compute_rsrc2;
    auto prog = Shader::BuildProgram(in);
    if (!prog) {
      ReportOnce("cannot decode shader at " + std::to_string(addr));
      return nullptr;
    }
    if (dump_dir) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s/%s_%llx_%016llx.bin", dump_dir,
                    stage == Stage::Vertex     ? "vs"
                    : stage == Stage::Fragment ? "ps"
                                               : "cs",
                    addr, prog->hash);
      const u32 size = Shader::Gcn::ProgramSize(code);
      std::ofstream(path, std::ios::binary)
          .write(reinterpret_cast<const char *>(code), size);
    }
    ShaderEntry e;
    std::memcpy(e.head, code, sizeof(e.head));
    e.prog = std::move(prog);
    list.push_back(std::move(e));
    return list.back().prog.get();
  }

  // MSL for `p`; GPU thread only (reports and dumps).
  std::string MslSource(const Program &p, const Shader::MslOptions &o,
                        u64 key) {
    std::vector<std::string> unsupported;
    std::string src = Shader::GenerateMsl(p, o, &unsupported);
    for (const auto &u : unsupported) {
      ReportOnce("shader op not translated: " + u);
    }
    if (dump_dir) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s/%s_%016llx_%016llx.metal", dump_dir,
                    p.stage == Stage::Vertex     ? "vs"
                    : p.stage == Stage::Fragment ? "ps"
                                                 : "cs",
                    p.hash, key);
      std::ofstream(path) << src;
    }
    return src;
  }

  // Moves finished pipelines into the caches.
  void TakeCompiled() {
    std::scoped_lock lk(done_lock);
    for (auto &[k, pso] : done_render) {
      pipelines[k] = pso;
      pending.erase(k);
    }
    for (auto &[k, pso] : done_compute) {
      compute_pipelines[k] = pso;
      pending.erase(k);
    }
    done_render.clear();
    done_compute.clear();
  }

  // Blocks until the pipeline `key` has been compiled.
  void WaitCompiled(u64 key) {
    while (pending.count(key)) {
      {
        std::unique_lock lk(done_lock);
        done_cv.wait(
            lk, [&] { return !done_render.empty() || !done_compute.empty(); });
      }
      TakeCompiled();
    }
  }

  // ================= textures =================

  // Stand-in for textures we cannot provide: white, or zero for what the
  // hardware itself reads as zero (null or INVALID-format descriptors).
  id<MTLTexture> Dummy(TextureType tt, bool zero = false) {
    const u32 key = static_cast<u32>(tt.dim) |
                    (static_cast<u32>(tt.sample) << 4) | (zero ? 0x100u : 0u);
    auto it = dummy_textures.find(key);
    if (it != dummy_textures.end()) {
      return it->second;
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = tt.dim == TexDim::Tex2D        ? MTLTextureType2D
                    : tt.dim == TexDim::Tex2DArray ? MTLTextureType2DArray
                                                   : MTLTextureType3D;
    d.pixelFormat = tt.sample == TexSample::Float  ? MTLPixelFormatRGBA8Unorm
                    : tt.sample == TexSample::Uint ? MTLPixelFormatRGBA8Uint
                                                   : MTLPixelFormatRGBA8Sint;
    d.width = d.height = 1;
    if (tt.dim == TexDim::Tex3D) {
      d.depth = 1;
    } else if (tt.dim == TexDim::Tex2DArray) {
      d.arrayLength = 1;
    }
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    id<MTLTexture> t = [device newTextureWithDescriptor:d];
    const u32 white = zero                            ? 0u
                      : tt.sample == TexSample::Float ? 0xFFFFFFFFu
                                                      : 0x01010101u;
    [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1)
          mipmapLevel:0
                slice:0
            withBytes:&white
          bytesPerRow:4
        bytesPerImage:4];
    dummy_textures[key] = t;
    return t;
  }

  // How a shader samples `f`: float, uint or sint.
  static TexSample SampleKind(MTLPixelFormat f) {
    switch (f) {
    case MTLPixelFormatR8Uint:
    case MTLPixelFormatRG8Uint:
    case MTLPixelFormatRGBA8Uint:
    case MTLPixelFormatR16Uint:
    case MTLPixelFormatRG16Uint:
    case MTLPixelFormatRGBA16Uint:
    case MTLPixelFormatR32Uint:
    case MTLPixelFormatRG32Uint:
    case MTLPixelFormatRGBA32Uint:
    case MTLPixelFormatRGB10A2Uint:
      return TexSample::Uint;
    case MTLPixelFormatR8Sint:
    case MTLPixelFormatRG8Sint:
    case MTLPixelFormatRGBA8Sint:
    case MTLPixelFormatR16Sint:
    case MTLPixelFormatRG16Sint:
    case MTLPixelFormatRGBA16Sint:
    case MTLPixelFormatR32Sint:
    case MTLPixelFormatRG32Sint:
    case MTLPixelFormatRGBA32Sint:
      return TexSample::Sint;
    default:
      return TexSample::Float;
    }
  }

  // Cheap change detection: a few dwords spread over the texture.
  u64 Hash64(const u8 *p, u64 size) {
    stat_hashed += size;
    u64 h[4] = {0x9e3779b97f4a7c15ull, 0xc2b2ae3d27d4eb4full,
                0x165667b19e3779f9ull, 0x27d4eb2f165667c5ull};
    u64 off = 0;
    for (; off + 32 <= size; off += 32) {
      for (u32 k = 0; k < 4; ++k) {
        u64 v;
        std::memcpy(&v, p + off + k * 8, 8);
        h[k] = (h[k] ^ v) * 0x100000001b3ull;
        h[k] ^= h[k] >> 29;
      }
    }
    for (; off < size; ++off) {
      h[0] = (h[0] ^ p[off]) * 0x100000001b3ull;
    }
    return h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7);
  }

  u64 ChunkHash(u64 addr, u64 size, u32 chunk) {
    const u64 off = u64(chunk) * kChunk;
    return Hash64(reinterpret_cast<const u8 *>(addr) + off,
                  std::min(kChunk, size - off));
  }

  // True if the texture memory differs from what was uploaded.
  bool TextureChanged(TextureEntry &e, u64 addr, u64 size) {
    const u32 n = static_cast<u32>(e.chunks.size());
    if (e.hot) {
      for (u32 c = 0; c < n; ++c) {
        if (ChunkHash(addr, size, c) != e.chunks[c]) {
          return true;
        }
      }
      return false;
    }
    const u32 c = e.next_chunk++ % n;
    return ChunkHash(addr, size, c) != e.chunks[c];
  }

  // Decodes a T# and returns a Metal texture for it.
  id<MTLTexture> GetTexture(const u32 t[8], TextureType *type_out,
                            bool storage = false) {
    const u64 addr =
        ((u64(t[0]) | (u64(t[1] & 0xFF) << 32)) << 8) & kGpuAddrMask;
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
    const u32 base_array = std::min<u32>(t[5] & 0x1FFF, last_array);
    const u32 base_level = (t[3] >> 12) & 0xF;
    const u32 last_level = std::max(base_level, (t[3] >> 16) & 0xF);

    const TextureFormatInfo fi = TextureFormat(dfmt, nfmt);
    TextureType tt;
    tt.sample = fi.sample;
    if (ttype == 10) {
      tt.dim = TexDim::Tex3D;
    } else if (ttype == 11 || ttype == 12 || ttype == 13 || ttype == 15) {
      tt.dim = TexDim::Tex2DArray;
      tt.cube = ttype == 11 && !storage;
    }
    *type_out = tt;
    if (!addr || ttype < 8 || dfmt == 0) {
      return Dummy(tt, true);
    }

    // Slices a descriptor of an array covers (a cube is 6 faces from its base).
    const u32 slice_count = tt.dim != TexDim::Tex2DArray ? 1
                            : ttype == 11
                                ? std::max<u32>(6, last_array - base_array + 1)
                                : last_array - base_array + 1;

    // A level of a larger image (see MipAlias): that level of it.
    if (auto it = mip_alias.find(addr);
        it != mip_alias.end() && it->second.width == width &&
        it->second.height == height && fi.format != MTLPixelFormatInvalid &&
        PixelBytes(fi.format) == PixelBytes(it->second.fmt)) {
      if (RenderTarget *p = MipParent(addr, it->second.fmt, width, height)) {
        const u32 lv = it->second.level;
        if (storage) {
          p->initialized = true;
          p->clear_pending = false;
          p->written = ++write_seq;
          type_out->ufloat = p->tex.pixelFormat == MTLPixelFormatRG11B10Float;
          return TargetView(*p, lv + base_level, 1, MTLPixelFormatInvalid,
                            base_array, slice_count);
        }
        tt.sample = SampleKind(p->tex.pixelFormat);
        *type_out = tt;
        return TargetView(*p, lv + base_level, last_level - base_level + 1,
                          MTLPixelFormatInvalid, base_array, slice_count, swz);
      }
    }

    // Render-to-texture: sample the render target directly.
    if (!storage) {
      // As shadPS4's image views: the T# format (same texel size) and swizzle.
      if (RenderTarget *rt =
              Latest(color_targets, addr, tt.dim, width, height)) {
        // An array read past the slices rendered so far: grow it (shadPS4 sizes
        // images from the T#), the new slices read as zero until written.
        if (tt.dim == TexDim::Tex2DArray &&
            rt->depth_or_layers < base_array + slice_count &&
            rt->width == width && rt->height == height) {
          rt = &GetColorTarget(addr, rt->tex.pixelFormat, width, height,
                               rt->levels, TexDim::Tex2DArray,
                               base_array + slice_count);
        }
        if (last_level >= rt->levels && rt->width == width &&
            rt->height == height) { // the T# has the levels: so does the image
          rt = &GetColorTarget(addr, rt->tex.pixelFormat, width, height,
                               last_level + 1, rt->dim, rt->depth_or_layers);
        }
        if (!fi.compressed && fi.bytes_per_block) {
          NoteMipLevels(*rt, t, fi.bytes_per_block);
        }
        MTLPixelFormat view_fmt = MTLPixelFormatInvalid;
        if (fi.format != MTLPixelFormatInvalid && !fi.compressed &&
            fi.format != rt->tex.pixelFormat && PixelBytes(fi.format) &&
            PixelBytes(fi.format) == PixelBytes(rt->tex.pixelFormat) &&
            !(fi.sample == TexSample::Float) ==
                !(SampleKind(rt->tex.pixelFormat) == TexSample::Float)) {
          view_fmt = fi.format;
        }
        if (view_fmt == MTLPixelFormatInvalid) {
          tt.sample = SampleKind(
              rt->tex.pixelFormat); // the shader reads what the view holds
          *type_out = tt;
        }
        return TargetView(*rt, base_level, last_level - base_level + 1,
                          view_fmt, base_array, slice_count, swz);
      }
      if (tt.dim == TexDim::Tex2D) {
        if (RenderTarget *rt =
                Latest(depth_targets, addr, tt.dim, width, height)) {
          return swz == kIdentitySwizzle
                     ? rt->tex
                     : TargetView(*rt, 0, rt->levels, MTLPixelFormatInvalid, 0,
                                  1, swz);
        }
      }
    }

    const u32 layers = tt.dim == TexDim::Tex2DArray ? base_array + slice_count
                       : tt.dim == TexDim::Tex3D    ? depth
                                                    : 1;

    if (storage) {
      // Written by shaders: keep it GPU-side like a render target, so
      // later draws sample what the compute pass produced.
      if (fi.format == MTLPixelFormatInvalid || fi.compressed) {
        ReportOnce("storage image type/format not supported (dim=" +
                   std::to_string(ttype) + " dfmt=" + std::to_string(dfmt) +
                   " nfmt=" + std::to_string(nfmt) +
                   " addr=" + std::to_string(addr) + " " +
                   std::to_string(width) + "x" + std::to_string(height) + ")");
        return Dummy(tt);
      }
      const MTLPixelFormat f = fi.format == MTLPixelFormatRGBA8Unorm_sRGB
                                   ? MTLPixelFormatRGBA8Unorm
                                   : fi.format;
      type_out->ufloat = f == MTLPixelFormatRG11B10Float;
      RenderTarget &target = GetColorTarget(addr, f, width, height,
                                            last_level + 1, tt.dim, layers);
      target.initialized = true;
      target.clear_pending = false;
      target.written = ++write_seq;
      return TargetView(target, base_level, 1, MTLPixelFormatInvalid,
                        base_array, slice_count);
    }

    if (fi.format == MTLPixelFormatInvalid || fi.gpu_only) {
      ReportOnce("texture format dfmt=" + std::to_string(dfmt) +
                 " nfmt=" + std::to_string(nfmt) + " not supported");
      return Dummy(tt);
    }
    if (!Tiling::Supported(tiling)) {
      ReportOnce("texture tile mode " + std::to_string(tiling) +
                 " not supported yet");
      return Dummy(tt);
    }
    const u32 width_el = fi.compressed ? (width + 3) / 4 : width;
    const u32 height_el = fi.compressed ? (height + 3) / 4 : height;
    const u32 thick = Tiling::Thickness(tiling);
    // The whole mip chain (3D textures: level 0 only), laid out like shadPS4's
    // ComputeImageSize: levels one after another, each with all its slices.
    const u32 max_levels =
        1 + static_cast<u32>(std::log2(std::max(width, height)));
    const u32 levels =
        tt.dim == TexDim::Tex3D ? 1 : std::min(last_level + 1, max_levels);
    const bool pow2 = (t[3] >> 25) & 1;
    Tiling::MipLevel mips[16];
    const u64 size =
        Tiling::MipLayout(tiling, pitch, height, layers, levels,
                          fi.bytes_per_block, fi.compressed, pow2, mips);
    if (!Readable(addr, size)) {
      ReportOnce("texture memory not mapped");
      return Dummy(tt);
    }

    u64 key = Hash(addr);
    for (u32 k = 1; k < 6; ++k) {
      key = Hash(t[k], key);
    }
    TextureEntry &e = textures[key];
    // T# BASE_LEVEL..LAST_LEVEL: a view of those levels.
    auto view = [&]() -> id<MTLTexture> {
      if (base_level == 0 || base_level >= levels) {
        return e.tex;
      }
      if (!e.base_view) {
        e.base_view = [e.tex
            newTextureViewWithPixelFormat:e.tex.pixelFormat
                              textureType:e.tex.textureType
                                   levels:NSMakeRange(base_level,
                                                      levels - base_level)
                                   slices:NSMakeRange(
                                              0, e.tex.textureType ==
                                                         MTLTextureType2DArray
                                                     ? e.tex.arrayLength
                                                     : 1)];
      }
      return e.base_view;
    };
    if (e.tex && e.frame == frame_count) {
      return view();
    }
    if (e.tex) {
      if (!TextureChanged(e, addr, size)) {
        e.frame = frame_count;
        return view();
      }
      e.hot = true;
    }

    if (!e.tex) {
      MTLTextureDescriptor *d = [MTLTextureDescriptor new];
      d.textureType = tt.dim == TexDim::Tex2D        ? MTLTextureType2D
                      : tt.dim == TexDim::Tex2DArray ? MTLTextureType2DArray
                                                     : MTLTextureType3D;
      d.pixelFormat = fi.format;
      d.width = width;
      d.height = height;
      d.mipmapLevelCount = levels;
      if (tt.dim == TexDim::Tex3D) {
        d.depth = layers;
      } else if (tt.dim == TexDim::Tex2DArray) {
        d.arrayLength = layers;
      }
      d.usage = MTLTextureUsageShaderRead;
      d.storageMode = MTLStorageModeManaged;
      d.swizzle =
          MTLTextureSwizzleChannelsMake(Swizzle(swz), Swizzle(swz >> 3),
                                        Swizzle(swz >> 6), Swizzle(swz >> 9));
      e.tex = [device newTextureWithDescriptor:d];
      if (!e.tex) {
        return Dummy(tt);
      }
    }
    const auto *src = reinterpret_cast<const u8 *>(addr);
    std::vector<u8> linear;
    for (u32 lv = 0; lv < levels; ++lv) {
      const Tiling::MipLevel &m = mips[lv];
      const u32 w = std::max(width >> lv, 1u), h = std::max(height >> lv, 1u);
      const u32 w_el = fi.compressed ? (w + 3) / 4 : w,
                h_el = fi.compressed ? (h + 3) / 4 : h;
      const u64 row = u64(w_el) * fi.bytes_per_block;
      const u64 image = row * h_el;
      linear.resize(image);
      const u32 level_thick = Tiling::Thickness(m.tile_index);
      for (u32 l = 0; l < layers; ++l) {
        const u64 group_offset = m.offset + m.slice_size * (l / level_thick);
        Tiling::DetileSlice(m.tile_index, src + group_offset, linear.data(),
                            w_el, h_el, m.pitch_el, fi.bytes_per_block,
                            l % level_thick);
        if (dump_texture == addr && dump_frames && l == 0 &&
            lv == 0) { // DC_DUMP_TEXTURE=<addr>: raw linear bytes
          char path[512];
          std::snprintf(path, sizeof(path), "%s/tex_%llx_%ux%u_bpe%u_f%llu.raw",
                        dump_frames, addr, width_el, height_el,
                        fi.bytes_per_block, frame_count);
          std::ofstream(path, std::ios::binary)
              .write(reinterpret_cast<const char *>(linear.data()), image);
        }
        const MTLRegion region = tt.dim == TexDim::Tex3D
                                     ? MTLRegionMake3D(0, 0, l, w, h, 1)
                                     : MTLRegionMake2D(0, 0, w, h);
        [e.tex replaceRegion:region
                 mipmapLevel:lv
                       slice:tt.dim == TexDim::Tex2DArray ? l : 0
                   withBytes:linear.data()
                 bytesPerRow:row
               bytesPerImage:image];
      }
    }
    (void)thick;
    e.chunks.resize((size + kChunk - 1) / kChunk);
    for (u32 c = 0; c < e.chunks.size(); ++c) {
      e.chunks[c] = ChunkHash(addr, size, c);
    }
    e.frame = frame_count;
    e.addr = addr;
    e.size = size;
    return view();
  }

  id<MTLSamplerState> GetSampler(const u32 s[4]) {
    const u64 key = Fnv(s, 16);
    auto it = samplers.find(key);
    if (it != samplers.end()) {
      return it->second;
    }
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.sAddressMode = AddressMode(s[0]);
    d.tAddressMode = AddressMode(s[0] >> 3);
    d.rAddressMode = AddressMode(s[0] >> 6);
    const u32 aniso = (s[0] >> 9) & 7;
    const u32 mag = (s[2] >> 20) & 3, min = (s[2] >> 22) & 3,
              mip = (s[2] >> 26) & 3;
    d.magFilter = (mag & 1) ? MTLSamplerMinMagFilterLinear
                            : MTLSamplerMinMagFilterNearest;
    d.minFilter = (min & 1) ? MTLSamplerMinMagFilterLinear
                            : MTLSamplerMinMagFilterNearest;
    d.mipFilter = mip == 0   ? MTLSamplerMipFilterNotMipmapped
                  : mip == 1 ? MTLSamplerMipFilterNearest
                             : MTLSamplerMipFilterLinear;
    if (min >= 2 || mag >= 2) {
      d.maxAnisotropy = std::min<u32>(16, 1u << aniso);
    }
    d.lodMinClamp = static_cast<float>(s[1] & 0xFFF) / 256.0f;
    d.lodMaxClamp = static_cast<float>((s[1] >> 12) & 0xFFF) / 256.0f;
    const u32 border = (s[3] >> 30) & 3;
    d.borderColor = border == 0   ? MTLSamplerBorderColorTransparentBlack
                    : border == 1 ? MTLSamplerBorderColorOpaqueBlack
                                  : MTLSamplerBorderColorOpaqueWhite;
    id<MTLSamplerState> st = [device newSamplerStateWithDescriptor:d];
    samplers[key] = st ? st : default_sampler;
    return samplers[key];
  }

  // ================= render targets =================

  // The variant at `addr` written last; with a size, the last one of that size
  // if there is one (aliased memory holds surfaces of several sizes, and a T#
  // means the one matching its own extent, like shadPS4's image lookup).
  static RenderTarget *Latest(TargetMap &map, u64 addr,
                              std::optional<TexDim> dim = std::nullopt,
                              u32 width = 0, u32 height = 0) {
    auto it = map.find(addr);
    if (it == map.end()) {
      return nullptr;
    }
    RenderTarget *best = nullptr;
    RenderTarget *sized = nullptr;
    for (auto &t : it->second) {
      if (dim && t->dim != *dim) {
        continue;
      }
      if (!best || t->written > best->written) {
        best = t.get();
      }
      if (width && t->width == width && t->height == height &&
          (!sized || t->written > sized->written)) {
        sized = t.get();
      }
    }
    return sized ? sized : best;
  }

  id<MTLTexture> NewTargetTexture(MTLPixelFormat fmt, u32 width, u32 height,
                                  u32 levels, bool depth,
                                  TexDim dim = TexDim::Tex2D,
                                  u32 depth_or_layers = 1) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = fmt;
    d.width = width;
    d.height = height;
    d.mipmapLevelCount = levels;
    d.storageMode = MTLStorageModePrivate;
    if (dim == TexDim::Tex3D) {
      d.textureType = MTLTextureType3D;
      d.depth = std::max(depth_or_layers, 1u);
      d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
                MTLTextureUsagePixelFormatView;
    } else if (dim == TexDim::Tex2DArray) {
      d.textureType = MTLTextureType2DArray;
      d.arrayLength = std::max(depth_or_layers, 1u);
      d.usage = depth
                    ? MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead
                    : MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
                          MTLTextureUsageShaderWrite |
                          MTLTextureUsagePixelFormatView;
    } else {
      d.textureType = MTLTextureType2D;
      d.usage = depth
                    ? MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
                          MTLTextureUsagePixelFormatView
                    : MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead |
                          MTLTextureUsageShaderWrite |
                          MTLTextureUsagePixelFormatView;
    }
    return [device newTextureWithDescriptor:d];
  }

  RenderTarget &GetTarget(TargetMap &map, u64 addr, MTLPixelFormat fmt,
                          u32 width, u32 height, bool depth, u32 levels = 1,
                          TexDim dim = TexDim::Tex2D, u32 depth_or_layers = 1) {
    auto &list = map[addr];
    const u32 max_levels =
        1 + static_cast<u32>(std::log2(std::max(width, height)));
    levels = std::clamp<u32>(levels, 1, max_levels);
    const bool array = dim == TexDim::Tex2DArray;
    for (auto &t : list) {
      // Arrays are one surface whatever slice range a descriptor covers: they
      // grow.
      if (t->width != width || t->height != height ||
          t->tex.pixelFormat != fmt || t->dim != dim ||
          (!array && t->depth_or_layers != depth_or_layers)) {
        continue;
      }
      const u32 want_layers =
          array ? std::max(t->depth_or_layers, depth_or_layers)
                : t->depth_or_layers;
      if (t->levels < levels ||
          t->depth_or_layers < want_layers) { // grow, keeping what is there
        const u32 new_levels = std::max(t->levels, levels);
        id<MTLTexture> tex = NewTargetTexture(fmt, width, height, new_levels,
                                              depth, dim, want_layers);
        EndPass();
        EnsureCommandBuffer();
        id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
        const u32 slices = array ? t->depth_or_layers : 1;
        for (u32 l = 0; l < t->levels; ++l) {
          [b copyFromTexture:t->tex
                   sourceSlice:0
                   sourceLevel:l
                     toTexture:tex
              destinationSlice:0
              destinationLevel:l
                    sliceCount:slices
                    levelCount:1];
        }
        [b endEncoding];
        t->tex = tex;
        t->levels = new_levels;
        t->depth_or_layers = want_layers;
        t->views.clear();
      }
      return *t;
    }
    auto rt = std::make_unique<RenderTarget>();
    rt->tex = NewTargetTexture(fmt, width, height, levels, depth, dim,
                               depth_or_layers);
    rt->addr = addr;
    rt->width = width;
    rt->height = height;
    rt->levels = levels;
    rt->dim = dim;
    rt->depth_or_layers = depth_or_layers;
    DC_INFO("gpu: %s target %#llx %ux%u (dim=%d d/l=%u) fmt %lu",
            depth ? "depth" : "render", addr, width, height, (int)dim,
            depth_or_layers, (unsigned long)fmt);
    list.push_back(std::move(rt));
    return *list.back();
  }

  // Levels [base, base + count) and, for arrays, slices [sbase, sbase + scount)
  // of `rt` (the texture itself when that is all of it).
  static constexpr u32 kIdentitySwizzle = 0xFAC; // T# DST_SEL x=R y=G z=B w=A

  id<MTLTexture> TargetView(RenderTarget &rt, u32 base, u32 count,
                            MTLPixelFormat fmt = MTLPixelFormatInvalid,
                            u32 sbase = 0, u32 scount = ~0u,
                            u32 swz = kIdentitySwizzle) {
    if (fmt == MTLPixelFormatInvalid) {
      fmt = rt.tex.pixelFormat;
    }
    base = std::min(base, rt.levels - 1);
    count = std::clamp<u32>(count, 1, rt.levels - base);
    const u32 total = rt.tex.textureType == MTLTextureType2DArray
                          ? static_cast<u32>(rt.tex.arrayLength)
                          : 1;
    sbase = std::min(sbase, total - 1);
    scount = std::clamp<u32>(scount, 1, total - sbase);
    swz &= 0xFFF;
    if (base == 0 && count == rt.levels && sbase == 0 && scount == total &&
        fmt == rt.tex.pixelFormat && swz == kIdentitySwizzle) {
      return rt.tex;
    }
    u64 key = Hash(base | (u64(count) << 8) | (u64(fmt) << 16) |
                   (u64(sbase) << 32) | (u64(scount) << 46));
    key = Hash(swz, key);
    auto it = rt.views.find(key);
    if (it != rt.views.end()) {
      return it->second;
    }
    id<MTLTexture> v = [rt.tex
        newTextureViewWithPixelFormat:fmt
                          textureType:rt.tex.textureType
                               levels:NSMakeRange(base, count)
                               slices:NSMakeRange(sbase, scount)
                              swizzle:MTLTextureSwizzleChannelsMake(
                                          Swizzle(swz), Swizzle(swz >> 3),
                                          Swizzle(swz >> 6),
                                          Swizzle(swz >> 9))];
    rt.views[key] = v;
    return v;
  }

  RenderTarget &GetColorTarget(u64 addr, MTLPixelFormat fmt, u32 width,
                               u32 height, u32 levels = 1,
                               TexDim dim = TexDim::Tex2D,
                               u32 depth_or_layers = 1) {
    return GetTarget(color_targets, addr, fmt, width, height, false, levels,
                     dim, depth_or_layers);
  }

  RenderTarget &GetDepthTarget(u64 addr, MTLPixelFormat fmt, u32 width,
                               u32 height) {
    return GetTarget(depth_targets, addr, fmt, width, height, true);
  }

  void NoteMeta(u64 meta, u64 target, bool depth) {
    if (meta) {
      metas[meta] = {target, depth};
    }
  }

  // A fill or a clear shader rewrote metadata in [addr, addr + bytes):
  // the surfaces it describes now read as their clear value.
  bool expanding_htile = false;

  bool MarkMetaCleared(u64 addr, u64 bytes) {
    bool any = false;
    for (const auto &[meta, m] : metas) {
      if (meta < addr || meta >= addr + bytes) {
        continue;
      }
      auto it = (m.depth ? depth_targets : color_targets).find(m.target);
      if (it != (m.depth ? depth_targets : color_targets).end()) {
        for (auto &t : it->second) {
          t->clear_pending = true;
        }
      }
      any = true;
      if (trace) {
        DC_INFO("gpu:   fast clear of %s %#llx (meta %#llx)",
                m.depth ? "depth" : "color", m.target, meta);
      }
    }
    return any;
  }

  void OnContextRegs(const u32 *body, u32 n) {
    if (n < 2) {
      return;
    }
    const u32 reg = kContextRegBase + body[0];
    const u32 count = n - 1;
    const u32 base0 = Reg::CB_COLOR0_BASE, stride = Reg::kColorTargetStride;
    if (reg >= base0 && reg < base0 + 8 * stride) {
      const u32 cb = (reg - base0) / stride, field = (reg - base0) % stride;
      if (field == Reg::CB_BASE) {
        cb_extent[cb] = (count == 0x0E || count == 0x0D || count == 0x0B) &&
                                body[n] == kNopSizeHint
                            ? body[n + 1]
                            : 0;
      } else if (field == Reg::CB_CMASK && count == 4 &&
                 body[n] == kNopSizeHint) {
        cb_extent[cb] = body[n + 1];
      }
    } else if (reg == Reg::DB_Z_INFO) {
      db_extent = count == 8 && body[21] == kNopSizeHint ? body[22] : 0;
    }
  }

  static bool HasStencil(id<MTLTexture> t) {
    return t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
  }

  void BeginPass(RenderTarget *colors[8], RenderTarget *depth, bool clear_depth,
                 float depth_value, bool clear_stencil = false,
                 u32 stencil_value = 0) {
    EndPass();
    if (work_since_commit >= kCommitEvery) {
      Commit(false); // keep the host GPU busy while the rest is recorded
    }
    EnsureCommandBuffer();
    MTLRenderPassDescriptor *rp =
        [MTLRenderPassDescriptor renderPassDescriptor];
    PassState ps;
    u32 w = ~0u, h = ~0u;
    const u64 seq = ++write_seq;
    for (u32 k = 0; k < 8; ++k) {
      if (!colors[k]) {
        continue;
      }
      RenderTarget &rt = *colors[k];
      rp.colorAttachments[k].texture = rt.tex;
      rp.colorAttachments[k].slice = rt.bind_slice;
      rp.colorAttachments[k].level = rt.bind_level;
      rp.colorAttachments[k].loadAction = rt.initialized && !rt.clear_pending
                                              ? MTLLoadActionLoad
                                              : MTLLoadActionClear;
      rp.colorAttachments[k].clearColor =
          rt.clear_pending ? rt.clear_color
          : debug_clear    ? MTLClearColorMake(0.1, 0.1, 0.3, 1)
                           : MTLClearColorMake(0, 0, 0, 0);
      rp.colorAttachments[k].storeAction = MTLStoreActionStore;
      rt.initialized = true;
      rt.clear_pending = false;
      rt.written = seq;
      ps.color[k] = reinterpret_cast<u64>(&rt);
      ps.slice[k] = rt.bind_slice | (rt.bind_level << 24);
      w = std::min(w, std::max(rt.width >> rt.bind_level, 1u));
      h = std::min(h, std::max(rt.height >> rt.bind_level, 1u));
    }
    if (depth) {
      const bool clear =
          clear_depth || depth->clear_pending || !depth->initialized;
      rp.depthAttachment.texture = depth->tex;
      rp.depthAttachment.loadAction =
          clear ? MTLLoadActionClear : MTLLoadActionLoad;
      rp.depthAttachment.clearDepth = clear_depth ? depth_value
                                      : depth->clear_pending
                                          ? depth->clear_depth
                                          : 1.0;
      rp.depthAttachment.storeAction = MTLStoreActionStore;
      if (HasStencil(depth->tex)) {
        const bool sclear =
            clear_stencil || depth->clear_pending || !depth->initialized;
        rp.stencilAttachment.texture = depth->tex;
        rp.stencilAttachment.loadAction =
            sclear ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.stencilAttachment.clearStencil = clear_stencil ? stencil_value
                                            : depth->clear_pending
                                                ? depth->clear_stencil
                                                : 0;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
      }
      depth->initialized = true;
      depth->clear_pending = false;
      depth->written = seq;
      ps.depth = reinterpret_cast<u64>(depth);
      w = std::min(w, depth->width);
      h = std::min(h, depth->height);
    }
    pass_width = w;
    pass_height = h;
    rp.renderTargetWidth = w;
    rp.renderTargetHeight = h;
    enc = [cmd renderCommandEncoderWithDescriptor:rp];
    pass = ps;
    if (dump_passes && trace) {
      for (u32 k = 0; k < 8; ++k) {
        pass_tex[k] = colors[k] ? colors[k]->tex : nil;
        pass_addr[k] = colors[k] ? colors[k]->addr : 0;
      }
      pass_tex[8] = depth ? depth->tex : nil;
      pass_addr[8] = depth ? depth->addr : 0;
    }
  }

  // ================= draws =================

  // DB_DEPTH_CONTROL + DB_STENCIL_CONTROL + DB_STENCILREFMASK(_BF), the way
  // shadPS4's UpdateDepthStencilState reads them.
  id<MTLDepthStencilState> GetDepthState(u32 db_control, bool depth_clear,
                                         bool stencil, u32 st_control,
                                         u32 ref_front, u32 ref_back,
                                         bool stencil_clear) {
    const bool backface = (db_control >> 7) & 1;
    if (!backface) {
      ref_back = ref_front;
    }
    u64 key = Hash(db_control & 0xFFF00FFFu);
    key = Hash(depth_clear, key);
    if (stencil) {
      key = Hash(st_control, key);
      key = Hash(ref_front & 0x00FFFF00u,
                 key); // masks; the reference is set per draw
      key = Hash(ref_back & 0x00FFFF00u, key);
      key = Hash(stencil_clear, key);
    }
    auto it = depth_states.find(key);
    if (it != depth_states.end()) {
      return it->second;
    }
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    const bool z_enable = (db_control >> 1) & 1;
    d.depthCompareFunction =
        z_enable ? CompareFunc(db_control >> 4) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = z_enable && ((db_control >> 2) & 1) && !depth_clear;
    if (stencil && (db_control & 1)) {
      auto face = [&](u32 fail, u32 zpass, u32 zfail, u32 func, u32 ref) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new];
        f.stencilFailureOperation = StencilOp(fail);
        f.depthStencilPassOperation = StencilOp(zpass);
        f.depthFailureOperation = StencilOp(zfail);
        f.stencilCompareFunction = CompareFunc(func);
        f.readMask = (ref >> 8) & 0xFF;
        f.writeMask = stencil_clear ? 0 : (ref >> 16) & 0xFF;
        return f;
      };
      d.frontFaceStencil = face(st_control, st_control >> 4, st_control >> 8,
                                db_control >> 8, ref_front);
      d.backFaceStencil =
          backface ? face(st_control >> 12, st_control >> 16, st_control >> 20,
                          db_control >> 20, ref_back)
                   : face(st_control, st_control >> 4, st_control >> 8,
                          db_control >> 8, ref_front);
    }
    id<MTLDepthStencilState> st = [device newDepthStencilStateWithDescriptor:d];
    depth_states[key] = st;
    return st;
  }

  // GCN REPLACE_OP writes STENCILOPVAL, REPLACE_TEST the test value (shadPS4: a
  // face with a ReplaceOp takes its reference from op_val).
  static u32 StencilRef(u32 st_ops, u32 refmask) {
    const bool op = (st_ops & 0xF) == 4 || ((st_ops >> 4) & 0xF) == 4 ||
                    ((st_ops >> 8) & 0xF) == 4;
    return op ? refmask >> 24 : refmask & 0xFF;
  }

  struct Resources {
    std::vector<id<MTLTexture>> tex;
    std::vector<id<MTLSamplerState>> smp;
  };
  // Evaluates each image instruction's descriptors against the user data.
  void Resolve(const Program &p, const u32 *ud, std::vector<TextureType> &types,
               Resources &out) {
    std::vector<std::array<u32, 4>> ssharps;
    for (const auto &ss : p.samplers) {
      std::array<u32, 4> s{};
      for (u32 k = 0; k < 4; ++k) {
        s[k] = Shader::ResolveSgpr(ss[k], ud);
      }
      ssharps.push_back(s);
    }
    for (const auto &img : p.images) {
      u32 t[8];
      for (u32 k = 0; k < 8; ++k) {
        t[k] = Shader::ResolveSgpr(img.tsharp[k], ud);
      }
      TextureType tt;
      id<MTLTexture> tex = GetTexture(t, &tt, img.storage);
      if (trace && (t[0] == 0 || std::getenv("DC_TRACE_TSHARP"))) {
        DC_INFO("gpu:   T# %08x %08x %08x %08x %08x %08x %08x %08x src kind %u "
                "ud %u off %u",
                t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7],
                img.tsharp[0].kind, img.tsharp[0].ud, img.tsharp[0].offset);
      }
      if (trace) {
        DC_INFO("gpu:   tex%zu addr=%#llx dfmt=%u nfmt=%u %ux%u tile=%u "
                "type=%u swz=%#x%s",
                out.tex.size(), (u64(t[0]) | (u64(t[1] & 0xFF) << 32)) << 8,
                (t[1] >> 20) & 0x3F, (t[1] >> 26) & 0xF, (t[2] & 0x3FFF) + 1,
                ((t[2] >> 14) & 0x3FFF) + 1, (t[3] >> 20) & 0x1F, t[3] >> 28,
                t[3] & 0xFFF, img.storage ? " storage" : "");
        DC_INFO("gpu:     bound: type %lu %lux%lu layers %lu levels %lu fmt "
                "%lu base_array %u last_array %u",
                (unsigned long)tex.textureType, (unsigned long)tex.width,
                (unsigned long)tex.height, (unsigned long)tex.arrayLength,
                (unsigned long)tex.mipmapLevelCount,
                (unsigned long)tex.pixelFormat, t[5] & 0x1FFF,
                (t[5] >> 13) & 0x1FFF);
      }
      if (img.compare && img.has_sampler &&
          img.sampler_index < ssharps.size()) {
        tt.compare = static_cast<u8>((ssharps[img.sampler_index][0] >> 12) & 7);
        static const bool always =
            std::getenv("DC_CMP_ALWAYS") != nullptr; // debug experiment
        if (always) {
          tt.compare = 7;
        }
        static const bool flip =
            std::getenv("DC_CMP_FLIP") != nullptr; // debug experiment
        if (flip && tt.compare >= 1 && tt.compare <= 6 && tt.compare != 2 &&
            tt.compare != 5) {
          tt.compare = tt.compare == 1   ? 6
                       : tt.compare == 3 ? 4
                       : tt.compare == 4 ? 3
                                         : 1;
        }
        if (trace) {
          const auto &ss = ssharps[img.sampler_index];
          DC_INFO("gpu:   compare sampler S# %08x %08x %08x %08x func %u",
                  ss[0], ss[1], ss[2], ss[3], tt.compare);
        }
      }
      types.push_back(tt);
      out.tex.push_back(tex);
    }
    for (const auto &s : ssharps) {
      out.smp.push_back(GetSampler(s.data()));
    }
  }

  // The target whose mip level lives at `addr` with this size and format.
  RenderTarget *MipParent(u64 addr, MTLPixelFormat fmt, u32 width, u32 height) {
    auto it = mip_alias.find(addr);
    if (it == mip_alias.end() || it->second.fmt != fmt ||
        it->second.width != width || it->second.height != height) {
      return nullptr;
    }
    RenderTarget *p = Latest(color_targets, it->second.parent, std::nullopt,
                             width << it->second.level,
                             height << it->second.level);
    return p && p->levels > it->second.level && p->tex.pixelFormat == fmt
               ? p
               : nullptr;
  }

  // shadPS4 MipOf for a sampled render target: each level past the first of
  // the T#'s memory becomes an alias; a target already rendered there is
  // copied into that level and retired.
  void NoteMipLevels(RenderTarget &rt, const u32 t[8], u32 bytes_per_el) {
    const u32 tiling = (t[3] >> 20) & 0x1F;
    const u32 last_level = (t[3] >> 16) & 0xF;
    if (!last_level || rt.levels <= 1 || !Tiling::Supported(tiling)) {
      return;
    }
    // ImageInfo NumLayers: depth + 1 slices (cubes: full cubes), pow2 padded.
    const u32 ttype = t[3] >> 28;
    u32 layers = rt.dim == TexDim::Tex2DArray ? (t[4] & 0x1FFF) + 1 : 1;
    if (ttype == 11) {
      layers *= 6;
    }
    const bool pow2 = (t[3] >> 25) & 1;
    if (pow2) {
      layers = std::bit_ceil(layers);
    }
    const u32 pitch = ((t[4] >> 13) & 0x3FFF) + 1;
    const u32 levels = std::min(last_level + 1, rt.levels);
    Tiling::MipLevel mips[16];
    Tiling::MipLayout(tiling, pitch, rt.height, layers, levels, bytes_per_el,
                      false, pow2, mips);
    for (u32 m = 1; m < levels; ++m) {
      const u64 a = rt.addr + mips[m].offset;
      MipAlias alias{rt.addr, m, std::max(rt.width >> m, 1u),
                     std::max(rt.height >> m, 1u), rt.tex.pixelFormat};
      auto [it, added] = mip_alias.try_emplace(a, alias);
      if (!added) {
        continue;
      }
      // Already rendered as a target of its own: move its slices in.
      auto list = color_targets.find(a);
      if (list == color_targets.end()) {
        continue;
      }
      auto &v = list->second;
      for (size_t i = 0; i < v.size();) {
        RenderTarget &c = *v[i];
        if (c.width != alias.width || c.height != alias.height ||
            c.tex.pixelFormat != alias.fmt || c.levels < 1) {
          ++i;
          continue;
        }
        if (c.initialized) {
          const u32 n = std::min<u32>(
              c.dim == TexDim::Tex2DArray ? c.depth_or_layers : 1,
              rt.dim == TexDim::Tex2DArray ? rt.depth_or_layers : 1);
          EndPass();
          EnsureCommandBuffer();
          id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
          [b copyFromTexture:c.tex
                   sourceSlice:0
                   sourceLevel:0
                     toTexture:rt.tex
              destinationSlice:0
              destinationLevel:m
                    sliceCount:n
                    levelCount:1];
          [b endEncoding];
          rt.written = std::max(rt.written, c.written);
        }
        DC_INFO("gpu: target %#llx %ux%u is level %u of %#llx", a, c.width,
                c.height, m, rt.addr);
        retired.push_back(std::move(v[i]));
        v.erase(v.begin() + i);
      }
      if (v.empty()) {
        color_targets.erase(list);
      }
    }
  }

  // Color target `k` as the CB registers describe it; notes its CMASK and
  // clear color for fast clears.
  RenderTarget &ColorTargetFor(const RegFile &r, u32 k) {
    const u32 info = r.ColorTarget(k, Reg::CB_INFO);
    const u64 base = u64(r.ColorTarget(k, Reg::CB_BASE)) << 8;
    u32 width = ((r.ColorTarget(k, Reg::CB_PITCH) & 0x7FF) + 1) * 8;
    const u32 slice = ((r.ColorTarget(k, Reg::CB_SLICE) & 0x3FFFFF) + 1) * 64;
    u32 height = slice / width;
    if (cb_extent[k]) { // the real size; pitch and slice are padded
      width = cb_extent[k] & 0xFFFF;
      height = cb_extent[k] >> 16;
    }
    const ColorFormatInfo cf =
        ColorTargetFormat(ColorInfo::Format(info), ColorInfo::NumberType(info));
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    // Rendering into one slice of an array (cube faces):
    // CB_COLOR_VIEW.SLICE_START.
    const u32 view = r.ColorTarget(k, Reg::CB_VIEW);
    const u32 slice_start = view & 0x7FF, slice_max = (view >> 13) & 0x7FF;
    if (RenderTarget *p = MipParent(base, cf.format, width, height)) {
      p->bind_slice = slice_start;
      p->bind_level = mip_alias[base].level;
      p->clear_color = ClearColor(info, r.ColorTarget(k, Reg::CB_CLEAR_WORD0),
                                  r.ColorTarget(k, Reg::CB_CLEAR_WORD1));
      return *p;
    }
    bool array = slice_start || slice_max;
    if (!array) {
      if (RenderTarget *a = Latest(color_targets, base, TexDim::Tex2DArray)) {
        array = a->width == width && a->height == height &&
                a->tex.pixelFormat == cf.format;
      }
    }
    RenderTarget &rt = array
                           ? GetColorTarget(base, cf.format, width, height, 1,
                                            TexDim::Tex2DArray, slice_start + 1)
                           : GetColorTarget(base, cf.format, width, height);
    rt.bind_slice = array ? slice_start : 0;
    rt.bind_level = 0;
    rt.clear_color = ClearColor(info, r.ColorTarget(k, Reg::CB_CLEAR_WORD0),
                                r.ColorTarget(k, Reg::CB_CLEAR_WORD1));
    if (ColorInfo::FastClear(info)) {
      NoteMeta(u64(r.ColorTarget(k, Reg::CB_CMASK)) << 8, base, false);
    }
    return rt;
  }

  // CB_COLOR_CONTROL.MODE = eliminate: tiles still fast-cleared get the
  // clear color. Here: a target with a pending clear is cleared now.
  void EliminateFastClear(const RegFile &r) {
    const u32 info = r.ColorTarget(0, Reg::CB_INFO);
    if (!ColorInfo::Format(info) || !r.ColorTarget(0, Reg::CB_BASE)) {
      return;
    }
    RenderTarget &rt = ColorTargetFor(r, 0);
    if (!rt.clear_pending) {
      return;
    }
    RenderTarget *colors[8] = {&rt};
    BeginPass(colors, nullptr, false, 1.0f);
    EndPass();
  }

  // True if `p` stores through the V# in user SGPRs ud..ud+3 and never loads
  // from it.
  static bool OnlyStoresTo(const Program &p, u32 ud) {
    bool store = false;
    for (const auto &i : p.code) {
      if ((i.enc != Shader::Gcn::Enc::MUBUF &&
           i.enc != Shader::Gcn::Enc::MTBUF) ||
          i.srsrc != ud) {
        continue;
      }
      const bool is_store =
          (i.op >= 4 && i.op <= 7) ||
          (i.enc == Shader::Gcn::Enc::MUBUF && i.op >= 24 && i.op <= 31);
      if (!is_store) {
        return false;
      }
      store = true;
    }
    return store;
  }

  // Probe slot for this draw/dispatch of the probed shader, or -1.
  int ProbeSlot(u64 shader, const std::string &what) {
    if (!probe_buf || !trace ||
        (shader != probe.shader && probe.shader != ~0ull) ||
        probe_slots.size() >= kProbeSlots) {
      return -1;
    }
    probe_slots.push_back(what);
    return static_cast<int>(probe_slots.size() - 1);
  }

  void PrintProbes() {
    const u32 *d = static_cast<const u32 *>(probe_buf.contents);
    for (size_t k = 0; k < probe_slots.size(); ++k) {
      const u32 *w = d + k * Shader::kProbeWords;
      if (!w[0]) {
        if (probe.shader != ~0ull) {
          DC_INFO("probe %zu (%s): not reached", k, probe_slots[k].c_str());
        }
        continue;
      }
      DC_INFO("probe %zu (%s) at %u,%u: exec=%#x vcc=%#x m0=%#x", k,
              probe_slots[k].c_str(), w[4], w[5], w[1], w[2], w[3]);
      std::string line;
      for (u32 n = 0; n < 96; ++n) {
        float f;
        std::memcpy(&f, &w[8 + n], 4);
        char b[64];
        std::snprintf(b, sizeof(b), " v%u=%08x(%g)", n, w[8 + n], f);
        line += b;
        if (n % 6 == 5) {
          DC_INFO("probe  %s", line.c_str());
          line.clear();
        }
      }
      for (u32 n = 0; n < 104; ++n) {
        char b[32];
        std::snprintf(b, sizeof(b), " s%u=%08x", n, w[104 + n]);
        line += b;
        if (n % 8 == 7) {
          DC_INFO("probe  %s", line.c_str());
          line.clear();
        }
      }
    }
    probe_slots.clear();
    std::memset(probe_buf.contents, 0, probe_buf.length);
  }

  void Draw(const RegFile &r, const DrawCall &dc);
  void Dispatch(const RegFile &r, u32 x, u32 y, u32 z);
  void Flip(const FlipInfo &f);
  std::unordered_map<u64, id<MTLComputePipelineState>> compute_pipelines;

  // DC_DEBUG_MEMTEST: reads `addr` through the shader memory path and
  // compares with what the CPU sees.
  void MemTest(u64 addr) {
    static bool done = false;
    if (done || !Readable(addr, 16)) {
      return;
    }
    done = true;
    std::string src = Shader::kMslPrelude;
    src += "kernel void memtest(device uint* gm0 [[buffer(1)]], device uint* "
           "gm1 [[buffer(2)]], "
           "device const DcMap& gmap [[buffer(3)]], constant ulong& addr "
           "[[buffer(4)]], device uint* out [[buffer(5)]]) {"
           " DcMem mem = {gm0, gm1, &gmap}; for (uint i = 0; i < 4; ++i) "
           "out[i] = dc_ld(mem, addr + i * 4);"
           " out[4] = uint(gmap.pages); out[5] = uint(gmap.split >> 20); }";
    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:@(src.c_str())
                                              options:nil
                                                error:&err];
    id<MTLComputePipelineState> cp = [device
        newComputePipelineStateWithFunction:[lib newFunctionWithName:@"memtest"]
                                      error:&err];
    if (!cp) {
      DC_ERROR("memtest: %s", err.localizedDescription.UTF8String);
      return;
    }
    UpdateMemMap();
    id<MTLBuffer> out =
        [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> c = [queue commandBuffer];
    id<MTLComputeCommandEncoder> e = [c computeCommandEncoder];
    [e setComputePipelineState:cp];
    [e setBuffer:mem[0] offset:0 atIndex:1];
    [e setBuffer:mem[1] offset:0 atIndex:2];
    [e setBuffer:mem_map offset:0 atIndex:3];
    [e setBytes:&addr length:8 atIndex:4];
    [e setBuffer:out offset:0 atIndex:5];
    [e dispatchThreads:MTLSizeMake(1, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [e endEncoding];
    [c commit];
    [c waitUntilCompleted];
    const u32 *g = static_cast<const u32 *>(out.contents);
    const u32 *cpu = reinterpret_cast<const u32 *>(addr);
    DC_INFO("memtest %#llx: gpu %08x %08x %08x %08x | cpu %08x %08x %08x %08x "
            "| ranges %u split %u MB (%s)",
            addr, g[0], g[1], g[2], g[3], cpu[0], cpu[1], cpu[2], cpu[3], g[4],
            g[5], c.error ? c.error.localizedDescription.UTF8String : "ok");
  }

  // Reads a presented frame back and writes it as a PPM (debug only).
  void DumpFrame(id<MTLTexture> tex) {
    const u32 w = static_cast<u32>(tex.width), h = static_cast<u32>(tex.height);
    id<MTLBuffer> buf =
        [device newBufferWithLength:u64(w) * h * 4
                            options:MTLResourceStorageModeShared];
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
    std::snprintf(path, sizeof(path), "%s/frame_%04llu.ppm", dump_frames,
                  frame_count);
    std::ofstream out(path, std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    const auto *px = static_cast<const u8 *>(buf.contents);
    std::vector<u8> rgb(u64(w) * h * 3);
    for (u64 i = 0; i < u64(w) * h; ++i) { // BGRA -> RGB
      rgb[i * 3] = px[i * 4 + 2];
      rgb[i * 3 + 1] = px[i * 4 + 1];
      rgb[i * 3 + 2] = px[i * 4];
    }
    out.write(reinterpret_cast<const char *>(rgb.data()),
              static_cast<std::streamsize>(rgb.size()));
  }
};

void MetalRenderer::Impl::Draw(const RegFile &r, const DrawCall &dc) {
  ++draw_count;
  ++stat_draws;
  ++work_since_commit;
  const u64 vs_addr = r.ProgramAddr(Reg::SPI_SHADER_PGM_LO_VS);
  const u64 ps_addr = r.ProgramAddr(Reg::SPI_SHADER_PGM_LO_PS);
  const u32 cb_mode = Bits(r[Reg::CB_COLOR_CONTROL], 4, 3);
  const u32 db_render = r[Reg::DB_RENDER_CONTROL];
  const u32 db_control = r[Reg::DB_DEPTH_CONTROL];
  const PrimType prim = r.Primitive();

  if (trace) {
    DC_INFO("gpu: draw #%llu prim=%#x count=%u idx=%d vs=%#llx ps=%#llx "
            "cb_mode=%u db_render=%#x "
            "db_ctl=%#x cb0=%#llx info=%#x mask=%#x z=%#llx/%#llx zinfo=%#x "
            "blend0=%#x colfmt=%#x",
            draw_count, static_cast<u32>(prim), dc.count, dc.indexed, vs_addr,
            ps_addr, cb_mode, db_render, db_control,
            u64(r.ColorTarget(0, Reg::CB_BASE)) << 8,
            r.ColorTarget(0, Reg::CB_INFO), r[Reg::CB_TARGET_MASK],
            u64(r[Reg::DB_Z_READ_BASE]) << 8, u64(r[Reg::DB_Z_WRITE_BASE]) << 8,
            r[Reg::DB_Z_INFO], r[Reg::CB_BLEND0_CONTROL],
            r[Reg::SPI_SHADER_COL_FORMAT]);
    if (r.ColorTarget(0, Reg::CB_VIEW)) {
      DC_INFO("gpu:   cb0 view=%#x attrib=%#x pitch=%#x slice=%#x",
              r.ColorTarget(0, Reg::CB_VIEW), r.ColorTarget(0, Reg::CB_ATTRIB),
              r.ColorTarget(0, Reg::CB_PITCH), r.ColorTarget(0, Reg::CB_SLICE));
    }
  }
  if (!vs_addr) {
    return;
  }
  if ((r[Reg::PA_CL_CLIP_CNTL] >> 22) &
      1) { // DX_RASTERIZATION_KILL: nothing reaches the rasterizer
    return;
  }
  static const u64 skip_ps =
      std::getenv("DC_SKIP_PS")
          ? std::strtoull(std::getenv("DC_SKIP_PS"), nullptr, 16)
          : 0;
  if (skip_ps && ps_addr == skip_ps) { // debug: drop one pixel shader's draws
    return;
  }

  // ---- attachments ----
  if (cb_mode == 2 || cb_mode == 5) { // fast-clear eliminate / FMASK decompress
    EliminateFastClear(r);
    return;
  }
  RenderTarget *colors[8] = {};
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
      if (!fmt || !base || !((target_mask >> (4 * k)) & 0xF) ||
          !((col_format >> (4 * k)) & 0xF)) {
        col_format &= ~(0xFu << (4 * k));
        continue;
      }
      const ColorFormatInfo cf =
          ColorTargetFormat(fmt, ColorInfo::NumberType(info));
      colors[k] = &ColorTargetFor(r, k);
      color_fmt[k] = cf.format;
      out_type[k] = cf.out;
      any_color = true;
    }
  } else {
    col_format = 0;
  }

  RenderTarget *depth = nullptr;
  const u32 z_info = r[Reg::DB_Z_INFO];
  const u32 zfmt = z_info & 3;
  const bool depth_clear = db_render & 1;
  const bool stencil_clear = (db_render >> 1) & 1;
  // shadPS4 StencilValid(): a stencil format and a stencil address.
  const bool stencil_valid =
      (r[Reg::DB_STENCIL_INFO] & 1) && r[Reg::DB_STENCIL_WRITE_BASE];
  const bool z_used = ((db_control >> 1) & 1) || depth_clear ||
                      (stencil_valid && ((db_control & 1) || stencil_clear));
  MTLPixelFormat depth_fmt = MTLPixelFormatInvalid;
  if (zfmt && z_used) {
    const u64 zbase = u64(r[Reg::DB_Z_WRITE_BASE]) << 8;
    const u32 size = r[Reg::DB_DEPTH_SIZE];
    u32 w = ((size & 0x7FF) + 1) * 8, h = (((size >> 11) & 0x7FF) + 1) * 8;
    if (db_extent) {
      w = db_extent & 0xFFFF;
      h = db_extent >> 16;
    }
    depth_fmt = stencil_valid ? MTLPixelFormatDepth32Float_Stencil8
                : zfmt == 1   ? MTLPixelFormatDepth16Unorm
                              : MTLPixelFormatDepth32Float;
    if (zbase && w && h) {
      depth = &GetDepthTarget(zbase, depth_fmt, w, h);
      depth->clear_depth = r.F(Reg::DB_DEPTH_CLEAR);
      depth->clear_stencil = r[Reg::DB_STENCIL_CLEAR] & 0xFF;
      if (ZInfo::TileSurface(z_info)) {
        NoteMeta(u64(r[Reg::DB_HTILE_DATA_BASE]) << 8, zbase, true);
      }
    } else {
      depth_fmt = MTLPixelFormatInvalid;
    }
  }
  if (!any_color && !depth) {
    if (trace)
      DC_INFO("gpu:   skipped: no attachments");
    return;
  }

  PassState want;
  for (u32 k = 0; k < 8; ++k) {
    want.color[k] = reinterpret_cast<u64>(colors[k]);
    want.slice[k] =
        colors[k] ? colors[k]->bind_slice | (colors[k]->bind_level << 24) : 0;
  }
  want.depth = reinterpret_cast<u64>(depth);
  if ((depth_clear || (stencil_clear && stencil_valid)) && depth) {
    // The draw only exists to clear depth and/or stencil.
    depth->bounds_seq = 0;
    if (!depth_clear) {
      // Stencil only: keep depth, so end any pass and reload it.
      BeginPass(colors, depth, false, 1.0f, true,
                r[Reg::DB_STENCIL_CLEAR] & 0xFF);
    } else {
      BeginPass(colors, depth, true, r.F(Reg::DB_DEPTH_CLEAR), stencil_clear,
                r[Reg::DB_STENCIL_CLEAR] & 0xFF);
    }
    return;
  }
  // ---- shaders ----
  const u32 *vs_ud = r.UserData(Reg::SPI_SHADER_USER_DATA_VS_0);
  const u32 *ps_ud = r.UserData(Reg::SPI_SHADER_USER_DATA_PS_0);
  Program *vs = GetProgram(Stage::Vertex, vs_addr, vs_ud,
                           Bits(r[Reg::SPI_SHADER_PGM_RSRC2_VS], 1, 5),
                           Bits(r[Reg::SPI_SHADER_PGM_RSRC1_VS], 24, 2));
  Program *ps = ps_addr && any_color
                    ? GetProgram(Stage::Fragment, ps_addr, ps_ud,
                                 Bits(r[Reg::SPI_SHADER_PGM_RSRC2_PS], 1, 5), 0)
                    : nullptr;
  if (!vs || (ps_addr && any_color && !ps)) {
    if (trace)
      DC_INFO("gpu:   skipped: shader decode failed");
    return;
  }

  if (trace) { // DC_PEEK_PS=<ps>: its first two V# buffers, printed after the
               // traced frame
    static const u64 peek_ps =
        std::getenv("DC_PEEK_PS")
            ? std::strtoull(std::getenv("DC_PEEK_PS"), nullptr, 16)
            : 0;
    if (ps_addr == peek_ps) {
      for (u32 k = 0; k < 8; k += 4) {
        peek_addrs.push_back(
            (u64(ps_ud[k]) | (u64(ps_ud[k + 1] & 0xFFF) << 32)) & kGpuAddrMask);
        peek_addrs.push_back(peek_addrs.back() + 64);
      }
      // DC_PEEK_PS_AT=<ud>:<byte offset>,...: 8 floats at V#(ud)+off, read when
      // the draw is recorded
      if (const char *at = std::getenv("DC_PEEK_PS_AT")) {
        for (const char *q = at; *q;) {
          char *e = nullptr;
          const u32 n = std::strtoul(q, &e, 10);
          const u32 off = *e == ':' ? std::strtoul(e + 1, &e, 16) : 0;
          const u64 a =
              ((u64(ps_ud[n & 15]) | (u64(ps_ud[(n + 1) & 15] & 0xFFF) << 32)) &
               kGpuAddrMask) +
              off;
          if (Readable(a, 32)) {
            const float *v = reinterpret_cast<const float *>(a);
            DC_INFO("gpu:   peek ud%u+%#x: %g %g %g %g %g %g %g %g", n, off,
                    v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
          }
          q = *e ? e + 1 : e;
        }
      }
    }
  }
  // ---- resources ----
  std::vector<TextureType> vs_types, ps_types;
  Resources vs_res, ps_res;
  Resolve(*vs, vs_ud, vs_types, vs_res);
  if (ps) {
    Resolve(*ps, ps_ud, ps_types, ps_res);
  }

  bool pending_clear = depth && depth->clear_pending;
  for (u32 k = 0; k < 8; ++k) {
    pending_clear = pending_clear || (colors[k] && colors[k]->clear_pending);
  }
  // Depth bounds test (Metal on AMD has none): the pixel shader compares a copy
  // of the depth buffer, taken when the depth may have changed since the last
  // copy.
  const bool depth_bounds = depth && ps && ((db_control >> 3) & 1);
  if (depth && ((db_control >> 2) & 1)) {
    depth->bounds_seq = 0; // this draw writes depth: an older copy goes stale
  }
  if (depth_bounds &&
      (!depth->bounds_copy || depth->bounds_seq != depth->written ||
       depth->clear_pending)) {
    if (depth->clear_pending ||
        !depth->initialized) { // apply the pending clear before copying
      BeginPass(colors, depth, false, 1.0f);
    }
    EndPass();
    EnsureCommandBuffer();
    if (!depth->bounds_copy) {
      MTLTextureDescriptor *d = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:depth->tex.pixelFormat
                                       width:depth->width
                                      height:depth->height
                                   mipmapped:NO];
      d.usage = MTLTextureUsageShaderRead;
      d.storageMode = MTLStorageModePrivate;
      depth->bounds_copy = [device newTextureWithDescriptor:d];
    }
    id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
    [b copyFromTexture:depth->tex
             sourceSlice:0
             sourceLevel:0
               toTexture:depth->bounds_copy
        destinationSlice:0
        destinationLevel:0
              sliceCount:1
              levelCount:1];
    [b endEncoding];
    BeginPass(colors, depth, false, 1.0f);
    depth->bounds_seq = depth->written;
  } else if (!enc || !(want == pass) || pending_clear) {
    BeginPass(colors, depth, false, 1.0f);
    if (depth && depth->bounds_seq && depth_bounds) {
      depth->bounds_seq = depth->written; // a new pass over unchanged depth
    }
  }

  // ---- functions ----
  // DC_PROBE_CB=<addr>: with DC_PROBE=*:nan, only draws into that color target.
  static const u64 probe_cb =
      std::getenv("DC_PROBE_CB")
          ? std::strtoull(std::getenv("DC_PROBE_CB"), nullptr, 16)
          : 0;
  const bool probe_target =
      !probe_cb || (u64(r.ColorTarget(0, Reg::CB_BASE)) << 8) == probe_cb;
  const int probe_slot =
      ps && probe_target
          ? ProbeSlot(ps_addr,
                      "draw " + std::to_string(draw_count) + " ps " +
                          Hex(ps_addr) + " cb0 " +
                          Hex(u64(r.ColorTarget(0, Reg::CB_BASE)) << 8))
          : -1;
  const bool rect = prim == PrimType::RectList;
  Shader::MslOptions vo;
  vo.rect_list = rect;
  vo.points = prim == PrimType::PointList;
  vo.vtx_point_size = vo.points && (r[Reg::PA_CL_VS_OUT_CNTL] & 1);
  const bool capture = debug_vsout && vs_addr == debug_vsout && !debug_vs_buf;
  vo.debug_capture = capture;
  vo.textures = vs_types;
  u64 vs_key = Hash(vs->hash);
  vs_key = Hash(rect, vs_key);
  vs_key = Hash(u32(vo.points) | (u32(vo.vtx_point_size) << 1), vs_key);
  vs_key = Hash(capture, vs_key);
  vs_key = Fnv(vs_types.data(), vs_types.size() * sizeof(TextureType), vs_key);
  u64 ps_key = 0;
  Shader::MslOptions fo;
  if (ps) {
    for (u32 k = 0; k < 32; ++k) {
      fo.ps_input_cntl[k] = r[Reg::SPI_PS_INPUT_CNTL_0 + k];
    }
    fo.vs_param_mask = vs->param_mask;
    fo.ps_input_addr = r[Reg::SPI_PS_INPUT_ADDR];
    fo.col_format = col_format;
    for (u32 k = 0; k < 8; ++k) {
      fo.mrt_type[k] = out_type[k];
      if (color_fmt[k] == MTLPixelFormatRG11B10Float) {
        fo.mrt_ufloat |= 1u << k;
      }
    }
    fo.textures = ps_types;
    fo.depth_target = depth != nullptr;
    fo.depth_bounds = depth_bounds;
    if (probe_slot >= 0) {
      fo.probe_pc = probe.pc;
      fo.probe_x = probe.x;
      fo.probe_y = probe.y;
    }
    ps_key = Hash(ps->hash);
    ps_key = Hash(fo.probe_pc, ps_key);
    ps_key = Hash(fo.depth_target, ps_key);
    ps_key = Hash(fo.depth_bounds, ps_key);
    ps_key = Fnv(fo.ps_input_cntl.data(), sizeof(fo.ps_input_cntl), ps_key);
    ps_key = Hash(fo.vs_param_mask, ps_key);
    ps_key = Hash(fo.ps_input_addr, ps_key);
    ps_key = Hash(fo.col_format, ps_key);
    ps_key = Fnv(fo.mrt_type.data(), sizeof(fo.mrt_type), ps_key);
    ps_key = Hash(fo.mrt_ufloat, ps_key);
    ps_key =
        Fnv(ps_types.data(), ps_types.size() * sizeof(TextureType), ps_key);
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
  pk.shader_mask = r[Reg::CB_SHADER_MASK];
  pk.depth_fmt = static_cast<u32>(depth_fmt);
  const u64 pkey = Hash(pk);
  TakeCompiled();
  auto pit = pipelines.find(pkey);
  if (pit == pipelines.end()) {
    if (!pending.count(pkey)) {
      MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
      for (u32 k = 0; k < 8; ++k) {
        if (!colors[k]) {
          continue;
        }
        MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[k];
        ca.pixelFormat = color_fmt[k];
        const u32 b = pk.blend[k];
        const u32 wm = (target_mask >> (4 * k)) & 0xF;
        ca.writeMask = ((wm & 1) ? MTLColorWriteMaskRed : 0) |
                       ((wm & 2) ? MTLColorWriteMaskGreen : 0) |
                       ((wm & 4) ? MTLColorWriteMaskBlue : 0) |
                       ((wm & 8) ? MTLColorWriteMaskAlpha : 0);
        const bool blendable = out_type[k] == OutType::Float &&
                               color_fmt[k] != MTLPixelFormatRGBA32Float;
        // CB_SHADER_MASK without alpha: the hardware blends with source alpha 1
        // (shadPS4 swaps SrcAlpha factors for One/Zero).
        const bool alpha_masked =
            !((r[Reg::CB_SHADER_MASK] >> (4 * k + 3)) & 1);
        if ((b >> 30) & 1 && blendable && !debug_fs) {
          ca.blendingEnabled = YES;
          auto fixa = [&](u32 f) {
            return !alpha_masked ? BlendFactor(f)
                   : f == 4      ? MTLBlendFactorOne
                   : f == 5      ? MTLBlendFactorZero
                                 : BlendFactor(f);
          };
          ca.sourceRGBBlendFactor = fixa(b & 0x1F);
          ca.rgbBlendOperation = BlendOp((b >> 5) & 7);
          ca.destinationRGBBlendFactor = fixa((b >> 8) & 0x1F);
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
      if (depth_fmt == MTLPixelFormatDepth32Float_Stencil8) {
        d.stencilAttachmentPixelFormat = depth_fmt;
      }
      pending.insert(pkey);
      std::string vsrc = MslSource(*vs, vo, vs_key);
      std::string fsrc = ps ? MslSource(*ps, fo, ps_key) : std::string();
      const bool flat = debug_fs && ps && out_type[0] == OutType::Float;
      compiler->Submit([this, d, vsrc = std::move(vsrc), fsrc = std::move(fsrc),
                        vs_key, ps_key, pkey, flat, vs_addr, ps_addr] {
        id<MTLRenderPipelineState> pso = nil;
        id<MTLFunction> vfn = compiler->Function(vs_key, vsrc, vs_addr);
        id<MTLFunction> ffn =
            fsrc.empty() ? nil : compiler->Function(ps_key, fsrc, ps_addr);
        if (vfn && (fsrc.empty() || ffn)) {
          d.vertexFunction = vfn;
          d.fragmentFunction = flat ? debug_fs : ffn;
          NSError *err = nil;
          const auto t0 = std::chrono::steady_clock::now();
          pso = [device newRenderPipelineStateWithDescriptor:d error:&err];
          ReportSlowCompile("pipeline", vs_addr, ps_addr, t0);
          if (!pso) {
            DC_ERROR("gpu: pipeline (vs %#llx ps %#llx): %s", vs_addr, ps_addr,
                     err ? err.localizedDescription.UTF8String : "?");
          }
        }
        {
          std::scoped_lock lk(done_lock);
          done_render.emplace_back(pkey, pso);
        }
        done_cv.notify_all();
      });
    }
    if (!sync_shaders && probe_slot < 0) { // probed draws must run: wait
      if (trace)
        DC_INFO("gpu:   skipped: pipeline compiling");
      return;
    }
    WaitCompiled(pkey);
    pit = pipelines.find(pkey);
  }
  id<MTLRenderPipelineState> pso = pit->second;
  if (!pso) {
    if (trace)
      DC_INFO("gpu:   skipped: no pipeline");
    return;
  }

  // ---- fixed function ----
  Rect sc{0, 0, static_cast<s32>(pass_width), static_cast<s32>(pass_height)};
  sc = Intersect(sc, ScissorReg(r[Reg::PA_SC_SCREEN_SCISSOR_TL],
                                r[Reg::PA_SC_SCREEN_SCISSOR_BR]));
  sc = Intersect(sc, ScissorReg(r[Reg::PA_SC_WINDOW_SCISSOR_TL],
                                r[Reg::PA_SC_WINDOW_SCISSOR_BR]));
  sc = Intersect(sc, ScissorReg(r[Reg::PA_SC_GENERIC_SCISSOR_TL],
                                r[Reg::PA_SC_GENERIC_SCISSOR_BR]));
  sc = Intersect(sc, ScissorReg(r[Reg::PA_SC_VPORT_SCISSOR_0_TL],
                                r[Reg::PA_SC_VPORT_SCISSOR_0_TL + 1]));
  if (trace) {
    DC_INFO("gpu:   vte=%#x vp=[%g %g | %g %g | %g %g] scissor=(%d,%d)-(%d,%d) "
            "[scr %#x %#x win %#x %#x gen %#x %#x vp %#x %#x mode0 %#x] "
            "pass=%ux%u clip=%#x su=%#x",
            r[Reg::PA_CL_VTE_CNTL], r.F(Reg::PA_CL_VPORT_XSCALE),
            r.F(Reg::PA_CL_VPORT_XSCALE + 1), r.F(Reg::PA_CL_VPORT_XSCALE + 2),
            r.F(Reg::PA_CL_VPORT_XSCALE + 3), r.F(Reg::PA_CL_VPORT_XSCALE + 4),
            r.F(Reg::PA_CL_VPORT_XSCALE + 5), sc.x0, sc.y0, sc.x1, sc.y1,
            r[Reg::PA_SC_SCREEN_SCISSOR_TL], r[Reg::PA_SC_SCREEN_SCISSOR_BR],
            r[Reg::PA_SC_WINDOW_SCISSOR_TL], r[Reg::PA_SC_WINDOW_SCISSOR_BR],
            r[Reg::PA_SC_GENERIC_SCISSOR_TL], r[Reg::PA_SC_GENERIC_SCISSOR_BR],
            r[Reg::PA_SC_VPORT_SCISSOR_0_TL],
            r[Reg::PA_SC_VPORT_SCISSOR_0_TL + 1], r[0xA292], pass_width,
            pass_height, r[Reg::PA_CL_CLIP_CNTL], r[Reg::PA_SU_SC_MODE_CNTL]);
  }
  if (sc.x1 <= sc.x0 || sc.y1 <= sc.y0) {
    return;
  }

  [enc setRenderPipelineState:pso];
  [enc setViewport:MTLViewport{0, 0, static_cast<double>(pass_width),
                               static_cast<double>(pass_height), 0, 1}];
  [enc setScissorRect:MTLScissorRect{static_cast<NSUInteger>(sc.x0),
                                     static_cast<NSUInteger>(sc.y0),
                                     static_cast<NSUInteger>(sc.x1 - sc.x0),
                                     static_cast<NSUInteger>(sc.y1 - sc.y0)}];
  if (depth) {
    const bool st = HasStencil(depth->tex);
    const u32 st_ops = r[Reg::DB_STENCIL_CONTROL];
    const u32 ref_f = r[Reg::DB_STENCILREFMASK],
              ref_b = r[Reg::DB_STENCILREFMASK_BF];
    [enc setDepthStencilState:GetDepthState(db_control, depth_clear, st, st_ops,
                                            ref_f, ref_b, stencil_clear)];
    if (st && (db_control & 1)) {
      const bool backface = (db_control >> 7) & 1;
      [enc setStencilFrontReferenceValue:StencilRef(st_ops, ref_f)
                      backReferenceValue:backface
                                             ? StencilRef(st_ops >> 12, ref_b)
                                             : StencilRef(st_ops, ref_f)];
    }
  }
  // PA_CL_CLIP_CNTL.ZCLIP_NEAR/FAR_DISABLE: depth is clamped instead of clipped
  // (shadPS4 ZclipEnable(): clipping only when both are enabled).
  const u32 clip_cntl = r[Reg::PA_CL_CLIP_CNTL];
  const bool zclip = !((clip_cntl >> 26) & 1) && !((clip_cntl >> 27) & 1);
  [enc setDepthClipMode:zclip ? MTLDepthClipModeClip : MTLDepthClipModeClamp];
  const u32 su = r[Reg::PA_SU_SC_MODE_CNTL];
  static const bool no_cull = std::getenv("DC_NO_CULL") != nullptr;
  const bool cull_front = (su & 1) && !no_cull,
             cull_back = ((su >> 1) & 1) && !no_cull;
  [enc setCullMode:(cull_front && cull_back) ? MTLCullModeNone
                   : cull_front              ? MTLCullModeFront
                   : cull_back               ? MTLCullModeBack
                                             : MTLCullModeNone];
  [enc setFrontFacingWinding:((su >> 2) & 1) ? MTLWindingClockwise
                                             : MTLWindingCounterClockwise];
  if (depth) {
    // Polygon offset (shadow maps rely on it): POLY_OFFSET_FRONT/BACK_ENABLE.
    const bool front = (su >> 11) & 1, back = (su >> 12) & 1;
    if (front || back) {
      const u32 base = Reg::PA_SU_POLY_OFFSET_FRONT_SCALE + (front ? 0 : 2);
      [enc setDepthBias:r.F(base + 1)
             slopeScale:r.F(base) / 16.0f
                  clamp:r.F(Reg::PA_SU_POLY_OFFSET_CLAMP)];
    } else {
      [enc setDepthBias:0 slopeScale:0 clamp:0];
    }
  }
  [enc setBlendColorRed:r.F(Reg::CB_BLEND_RED)
                  green:r.F(Reg::CB_BLEND_RED + 1)
                   blue:r.F(Reg::CB_BLEND_RED + 2)
                  alpha:r.F(Reg::CB_BLEND_RED + 3)];

  Shader::GpuDrawConsts consts{};
  const u32 vte = r[Reg::PA_CL_VTE_CNTL];
  const float xs = r.F(Reg::PA_CL_VPORT_XSCALE),
              xo = r.F(Reg::PA_CL_VPORT_XSCALE + 1);
  const float ys = r.F(Reg::PA_CL_VPORT_XSCALE + 2),
              yo = r.F(Reg::PA_CL_VPORT_XSCALE + 3);
  const float zs = r.F(Reg::PA_CL_VPORT_XSCALE + 4),
              zo = r.F(Reg::PA_CL_VPORT_XSCALE + 5);
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
    MemTest(vs_addr);
  }
  u32 vud[16], pud[16];
  std::memcpy(vud, vs_ud, sizeof(vud));
  std::memcpy(pud, ps_ud, sizeof(pud));
  PatchUserData(vud);
  if (ps) {
    PatchUserData(pud);
  }
  UpdateMemMap();
  [enc setVertexBytes:vud length:64 atIndex:Shader::Binding::UserData];
  [enc setVertexBuffer:mem[0] offset:0 atIndex:Shader::Binding::Mem0];
  [enc setVertexBuffer:mem[1] offset:0 atIndex:Shader::Binding::Mem1];
  [enc setVertexBuffer:mem_map offset:0 atIndex:Shader::Binding::MemMap];
  [enc setVertexBytes:&consts
               length:sizeof(consts)
              atIndex:Shader::Binding::Draw];
  for (u32 k = 0; k < vs_res.tex.size(); ++k) {
    [enc setVertexTexture:vs_res.tex[k] atIndex:k];
  }
  for (u32 u = 0; u < vs_res.smp.size() && u < 16; ++u) {
    [enc setVertexSamplerState:vs_res.smp[u] atIndex:u];
  }
  if (ps) {
    [enc setFragmentBytes:pud length:64 atIndex:Shader::Binding::UserData];
    [enc setFragmentBuffer:mem[0] offset:0 atIndex:Shader::Binding::Mem0];
    [enc setFragmentBuffer:mem[1] offset:0 atIndex:Shader::Binding::Mem1];
    [enc setFragmentBuffer:mem_map offset:0 atIndex:Shader::Binding::MemMap];
    for (u32 k = 0; k < ps_res.tex.size(); ++k) {
      [enc setFragmentTexture:ps_res.tex[k] atIndex:k];
    }
    for (u32 u = 0; u < ps_res.smp.size() && u < 16; ++u) {
      [enc setFragmentSamplerState:ps_res.smp[u] atIndex:u];
    }
    if (depth_bounds) {
      const float zb[2] = {r.F(Reg::DB_DEPTH_BOUNDS_MIN),
                           r.F(Reg::DB_DEPTH_BOUNDS_MAX)};
      if (trace) {
        DC_INFO("gpu:   depth bounds [%g, %g] depth fmt %lu", zb[0], zb[1],
                (unsigned long)depth->tex.pixelFormat);
      }
      [enc setFragmentBytes:zb
                     length:sizeof(zb)
                    atIndex:Shader::kDepthBoundsBuffer];
      [enc setFragmentTexture:depth->bounds_copy
                      atIndex:Shader::kDepthBoundsTexture];
    }
    if (probe_slot >= 0) {
      [enc setFragmentBuffer:probe_buf
                      offset:u64(probe_slot) * Shader::kProbeWords * 4
                     atIndex:Shader::kProbeBinding];
    }
  }

  if (capture) {
    debug_vs_buf = [device newBufferWithLength:64 * 16
                                       options:MTLResourceStorageModeShared];
    [enc setVertexBuffer:debug_vs_buf offset:0 atIndex:6];
    DC_INFO("gpu: capturing VS %#llx ud: %08x %08x %08x %08x %08x %08x %08x "
            "%08x %08x %08x %08x %08x %08x %08x %08x %08x",
            vs_addr, vs_ud[0], vs_ud[1], vs_ud[2], vs_ud[3], vs_ud[4], vs_ud[5],
            vs_ud[6], vs_ud[7], vs_ud[8], vs_ud[9], vs_ud[10], vs_ud[11],
            vs_ud[12], vs_ud[13], vs_ud[14], vs_ud[15]);
  }
  // ---- primitive assembly ----
  const u32 base_vertex = r[Reg::VGT_INDX_OFFSET];
  const NSUInteger instances = std::max<u32>(dc.instances, 1);
  MTLPrimitiveType mprim = MTLPrimitiveTypeTriangle;
  bool to_list = false; // fans and quads are rebuilt as triangle lists
  switch (prim) {
  case PrimType::PointList:
    mprim = MTLPrimitiveTypePoint;
    break;
  case PrimType::LineList:
    mprim = MTLPrimitiveTypeLine;
    break;
  case PrimType::LineStrip:
    mprim = MTLPrimitiveTypeLineStrip;
    break;
  case PrimType::TriList:
    mprim = MTLPrimitiveTypeTriangle;
    break;
  case PrimType::TriStrip:
    mprim = MTLPrimitiveTypeTriangleStrip;
    break;
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
    [enc drawPrimitives:MTLPrimitiveTypeTriangle
            vertexStart:0
            vertexCount:(dc.count / 3) * 6
          instanceCount:instances];
    return;
  default:
    ReportOnce("primitive type " + std::to_string(static_cast<u32>(prim)) +
               " not supported");
    return;
  }

  if (!dc.indexed && !to_list) {
    [enc drawPrimitives:mprim
            vertexStart:base_vertex
            vertexCount:dc.count
          instanceCount:instances];
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

  // Indexed lists and strips read the guest index buffer in place (Metal
  // restarts strips at 0xFFFF / 0xFFFFFFFF like VGT_MULTI_PRIM_IB_RESET_INDX =
  // -1).
  if (dc.indexed && !to_list && (dc.index_size == 2 || dc.index_size == 4)) {
    const u64 src = dc.index_base + u64(dc.index_offset) * dc.index_size;
    id<MTLBuffer> ibuf = nil;
    u64 ioff = 0;
    if ((src % dc.index_size) == 0 &&
        Locate(src, u64(dc.count) * dc.index_size, &ibuf, &ioff)) {
      [enc drawIndexedPrimitives:mprim
                      indexCount:dc.count
                       indexType:dc.index_size == 2 ? MTLIndexTypeUInt16
                                                    : MTLIndexTypeUInt32
                     indexBuffer:ibuf
               indexBufferOffset:ioff
                   instanceCount:instances
                      baseVertex:base_vertex
                    baseInstance:0];
      return;
    }
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
      std::memcpy(idx.data(), reinterpret_cast<const void *>(src),
                  dc.count * 4ull);
    } else {
      const auto *s16 = reinterpret_cast<const u16 *>(src);
      for (u32 k = 0; k < dc.count; ++k) {
        idx[k] =
            s16[k] == 0xFFFF ? 0xFFFFFFFFu : s16[k]; // keep the restart index
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
        list.insert(list.end(), {idx[q], idx[q + 1], idx[q + 2], idx[q],
                                 idx[q + 2], idx[q + 3]});
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

void MetalRenderer::Impl::Dispatch(const RegFile &r, u32 gx, u32 gy, u32 gz) {
  const u64 addr = r.ProgramAddr(Reg::COMPUTE_PGM_LO);
  const u32 rsrc2 = r[Reg::COMPUTE_PGM_RSRC2];
  const u32 *ud = r.UserData(Reg::COMPUTE_USER_DATA_0);
  const u32 tx = std::max<u32>(1, r[Reg::COMPUTE_NUM_THREAD_X] & 0xFFFF);
  const u32 ty = std::max<u32>(1, r[Reg::COMPUTE_NUM_THREAD_Y] & 0xFFFF);
  const u32 tz = std::max<u32>(1, r[Reg::COMPUTE_NUM_THREAD_Z] & 0xFFFF);
  if (trace) {
    DC_INFO("gpu: dispatch cs=%#llx groups %ux%ux%u threads %ux%ux%u rsrc2=%#x "
            "ud=%08x %08x %08x %08x %08x %08x %08x "
            "%08x %08x %08x %08x %08x %08x %08x %08x %08x",
            addr, gx, gy, gz, tx, ty, tz, rsrc2, ud[0], ud[1], ud[2], ud[3],
            ud[4], ud[5], ud[6], ud[7], ud[8], ud[9], ud[10], ud[11], ud[12],
            ud[13], ud[14], ud[15]);
  }
  if (!addr || !gx || !gy || !gz) {
    return;
  }
  ++stat_dispatches;
  if (trace) {
    static const u64 peek_cs =
        std::getenv("DC_PEEK_CS")
            ? std::strtoull(std::getenv("DC_PEEK_CS"), nullptr, 16)
            : 0;
    if (addr == peek_cs) {
      for (u32 k = 0; k + 1 < 12; k += 4) {
        peek_addrs.push_back((u64(ud[k]) | (u64(ud[k + 1] & 0xFFF) << 32)) &
                             kGpuAddrMask);
      }
      // DC_PEEK_TABLE=<ud>,<dword>: also the V# found at pointer ud[n..n+1] +
      // dword*4
      if (const char *pt = std::getenv("DC_PEEK_TABLE")) {
        char *e = nullptr;
        const u32 n = std::strtoul(pt, &e, 10);
        const u32 off = *e ? std::strtoul(e + 1, nullptr, 10) : 0;
        const u64 table = u64(ud[n]) | (u64(ud[n + 1] & 0xFFFF) << 32);
        if (n + 1 < 16 && Readable(table + off * 4, 16)) {
          const u32 *v = reinterpret_cast<const u32 *>(table + off * 4);
          DC_INFO("gpu: peek table %#llx+%u: V# %08x %08x %08x %08x", table,
                  off, v[0], v[1], v[2], v[3]);
          peek_addrs.push_back((u64(v[0]) | (u64(v[1] & 0xFFF) << 32)) &
                               kGpuAddrMask);
        }
      }
    }
  }
  // The Gnm driver copies surfaces with a buffer-copy shader (V# source in
  // user data 0-3, destination in 4-7). Surfaces we render live only in
  // Metal textures, so copy the texture and give the destination one too.
  if (tx == 64 && ty == 1 && tz == 1) {
    const u64 src = (u64(ud[0]) | (u64(ud[1] & 0xFFF) << 32)) & kGpuAddrMask;
    const u64 dst = (u64(ud[4]) | (u64(ud[5] & 0xFFF) << 32)) & kGpuAddrMask;
    const bool from_depth = Latest(depth_targets, src) != nullptr;
    RenderTarget *from =
        from_depth ? Latest(depth_targets, src) : Latest(color_targets, src);
    if (from && dst && dst != src) {
      RenderTarget &to =
          GetTarget(from_depth ? depth_targets : color_targets, dst,
                    from->tex.pixelFormat, from->width, from->height,
                    from_depth, from->levels, from->dim, from->depth_or_layers);
      EndPass();
      EnsureCommandBuffer();
      id<MTLBlitCommandEncoder> b = [cmd blitCommandEncoder];
      [b copyFromTexture:from->tex toTexture:to.tex];
      [b endEncoding];
      to.initialized = true;
      to.clear_pending = from->clear_pending;
      to.clear_color = from->clear_color;
      to.clear_depth = from->clear_depth;
      to.written = ++write_seq;
      if (trace) {
        DC_INFO("gpu:   surface copy %#llx -> %#llx (%ux%u)", src, dst,
                from->width, from->height);
      }
      return;
    }
  }
  Program *cs = GetProgram(Stage::Compute, addr, ud, Bits(rsrc2, 1, 5),
                           Bits(rsrc2, 11, 2), rsrc2);
  if (!cs) {
    return;
  }
  // A shader that only stores to a CMASK/HTILE surface it gets straight
  // from user data is a fast clear (others read metadata, e.g. HTILE for
  // tile depth bounds, and run normally).
  for (u32 k = 0; k + 3 < 16 && !metas.empty(); ++k) {
    const u64 base =
        (u64(ud[k]) | (u64(ud[k + 1] & 0xFFF) << 32)) & kGpuAddrMask;
    if (metas.count(base) && OnlyStoresTo(*cs, k) && MarkMetaCleared(base, 1)) {
      EndPass();
      return;
    }
  }
  // A shader reading HTILE (e.g. per-tile depth bounds for light culling)
  // would see stale memory: depth lives in Metal textures. Give it expanded
  // tiles with the full Z range, so nothing is culled by mistake.
  for (u32 k = 0; k + 3 < 16 && !metas.empty(); ++k) {
    const u64 base =
        (u64(ud[k]) | (u64(ud[k + 1] & 0xFFF) << 32)) & kGpuAddrMask;
    auto m = metas.find(base);
    if (m == metas.end() || !m->second.depth) {
      continue;
    }
    const u32 stride = (ud[k + 1] >> 16) & 0x3FFF;
    const u64 bytes = u64(ud[k + 2]) * (stride ? stride : 1);
    if (bytes && bytes < (64ull << 20)) {
      expanding_htile = true; // our own fill, not the game clearing depth
      DmaData(0, base, 2, 0xFFFC000Fu, static_cast<u32>(bytes & ~3ull));
      expanding_htile = false;
    }
    k += 3;
  }
  std::vector<TextureType> types;
  Resources res;
  Resolve(*cs, ud, types, res);

  Shader::MslOptions o;
  o.textures = types;
  const int probe_slot = ProbeSlot(
      addr, "dispatch " + std::to_string(stat_dispatches) + " cs " + Hex(addr));
  if (probe_slot >= 0) {
    o.probe_pc = probe.pc;
    o.probe_x = probe.x;
    o.probe_y = probe.y;
  }
  u64 key = Hash(cs->hash);
  key = Hash(o.probe_pc, key);
  key = Hash(rsrc2, key);
  key = Fnv(types.data(), types.size() * sizeof(TextureType), key);
  const u32 group = tx * ty * tz;
  const u64 pkey = Hash(group, key);
  TakeCompiled();
  auto it = compute_pipelines.find(pkey);
  if (it == compute_pipelines.end()) {
    if (!pending.count(pkey)) {
      pending.insert(pkey);
      compiler->Submit([this, src = MslSource(*cs, o, key), key, pkey, group,
                        addr] {
        id<MTLComputePipelineState> pso = nil;
        if (id<MTLFunction> fn = compiler->Function(key, src, addr)) {
          MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
          d.computeFunction = fn;
          d.maxTotalThreadsPerThreadgroup =
              group; // the guest's group size must fit as is
          NSError *err = nil;
          const auto t0 = std::chrono::steady_clock::now();
          pso = [device
              newComputePipelineStateWithDescriptor:d
                                            options:MTLPipelineOptionNone
                                         reflection:nil
                                              error:&err];
          ReportSlowCompile("compute pipeline", addr, 0, t0);
          if (!pso) {
            DC_ERROR("gpu: compute pipeline %#llx: %s", addr,
                     err ? err.localizedDescription.UTF8String : "?");
          } else if (pso.maxTotalThreadsPerThreadgroup < group) {
            DC_ERROR("gpu: compute shader %#llx needs %u threads per group, "
                     "Metal allows %lu",
                     addr, group,
                     (unsigned long)pso.maxTotalThreadsPerThreadgroup);
            pso = nil;
          }
        }
        {
          std::scoped_lock lk(done_lock);
          done_compute.emplace_back(pkey, pso);
        }
        done_cv.notify_all();
      });
    }
    // Dispatches often produce data later work depends on: wait.
    WaitCompiled(pkey);
    it = compute_pipelines.find(pkey);
  }
  id<MTLComputePipelineState> pso = it->second;
  if (!pso) {
    return;
  }

  EndPass();
  if (++work_since_commit >= kCommitEvery) {
    Commit(false);
  }
  EnsureCommandBuffer();
  u32 cud[16];
  std::memcpy(cud, ud, sizeof(cud));
  PatchUserData(cud);
  UpdateMemMap();
  id<MTLComputeCommandEncoder> e = [cmd computeCommandEncoder];
  [e setComputePipelineState:pso];
  [e setBytes:cud length:64 atIndex:Shader::Binding::UserData];
  [e setBuffer:mem[0] offset:0 atIndex:Shader::Binding::Mem0];
  [e setBuffer:mem[1] offset:0 atIndex:Shader::Binding::Mem1];
  [e setBuffer:mem_map offset:0 atIndex:Shader::Binding::MemMap];
  [e setBuffer:gds offset:0 atIndex:Shader::Binding::Gds];
  for (u32 k = 0; k < res.tex.size(); ++k) {
    [e setTexture:res.tex[k] atIndex:k];
  }
  for (u32 u = 0; u < res.smp.size() && u < 16; ++u) {
    [e setSamplerState:res.smp[u] atIndex:u];
  }
  if (probe_slot >= 0) {
    [e setBuffer:probe_buf
          offset:u64(probe_slot) * Shader::kProbeWords * 4
         atIndex:Shader::kProbeBinding];
  }
  // LDS_SIZE is in 128-dword units on CI; Metal wants a multiple of 16 bytes.
  const u32 lds = std::max<u32>(16, Bits(rsrc2, 15, 9) * 512);
  [e setThreadgroupMemoryLength:std::min<u32>(lds, 32768) atIndex:0];
  [e dispatchThreadgroups:MTLSizeMake(gx, gy, gz)
      threadsPerThreadgroup:MTLSizeMake(tx, ty, tz)];
  [e endEncoding];
  if (dump_passes && trace && dump_frames) {
    u32 n = 0;
    for (u32 k = 0; k < res.tex.size() && n < 8; ++k) {
      if (cs->images[k].storage &&
          (res.tex[k].textureType == MTLTextureType2D ||
           res.tex[k].textureType == MTLTextureType2DArray)) {
        pass_tex[n] = res.tex[k];
        pass_addr[n++] = addr;
      }
    }
    DumpPass();
  }
  // DC_DUMP_CS=<cs>: with DC_TRACE_FRAME, the textures that shader samples.
  static const u64 dump_cs = std::getenv("DC_DUMP_CS")
                                 ? std::strtoull(std::getenv("DC_DUMP_CS"),
                                                 nullptr, 16)
                                 : 0;
  if (dump_cs == addr && trace && dump_frames) {
    for (u32 k = 0; k < res.tex.size(); ++k) {
      id<MTLTexture> t = res.tex[k];
      if (cs->images[k].storage || !copy_pipeline) {
        continue;
      }
      const NSUInteger n =
          t.textureType == MTLTextureType2DArray ? t.arrayLength : 1;
      for (NSUInteger l = 0; l < std::min<NSUInteger>(t.mipmapLevelCount, 3);
           ++l) {
        for (NSUInteger sl = 0; sl < std::min<NSUInteger>(n, 12); ++sl) {
          if (t.textureType != MTLTextureType2D &&
              t.textureType != MTLTextureType2DArray) {
            continue;
          }
          DumpTexture([t newTextureViewWithPixelFormat:t.pixelFormat
                                           textureType:MTLTextureType2D
                                                levels:NSMakeRange(l, 1)
                                                slices:NSMakeRange(sl, 1)],
                      k, (u64(k) << 8) | (l << 4) | sl);
        }
      }
    }
  }
}

void MetalRenderer::Impl::Flip(const FlipInfo &f) {
  ++frame_count;
  if (trace_frame) {
    trace = frame_count == trace_frame;
  }
  if (stats) {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - stat_t0).count();
    static u64 frames = 0;
    ++frames;
    if (dt >= 1.0) {
      DC_INFO(
          "gpu stats: %.1f fps, gpu busy %.0f%%, wait %.0f%%, per frame: %.0f "
          "draws %.0f dispatches %.1f commits %.1f waits %.1f MB hashed",
          frames / dt, gpu_ns.exchange(0) / 1e9 / dt * 100,
          stat_wait_s / dt * 100, stat_draws / double(frames),
          stat_dispatches / double(frames), stat_commits / double(frames),
          stat_waits / double(frames),
          stat_hashed / double(frames) / (1 << 20));
      stat_hashed = 0;
      frames = stat_commits = stat_waits = stat_draws = stat_dispatches = 0;
      if (profile) {
        ProfileReport();
      }
      stat_wait_s = 0;
      stat_t0 = now;
    }
  }
  EndPass();
  EnsureCommandBuffer();

  __strong id<MTLTexture> &dst = frames[frame_index];
  if (!dst || dst.width != f.width || dst.height != f.height) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:f.width
                                    height:f.height
                                 mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    dst = [device newTextureWithDescriptor:d];
  }

  id<MTLTexture> src = nil;
  float scale[2] = {1.0f, 1.0f};
  RenderTarget *rt = Latest(color_targets, f.address);
  if (rt && rt->tex) {
    id<MTLTexture> t = rt->tex;
    // Scan-out shows the stored bytes: sample sRGB targets without decoding.
    if (t.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB) {
      t = [t newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm];
    }
    src = t;
    scale[0] = static_cast<float>(f.width) / static_cast<float>(rt->width);
    scale[1] = static_cast<float>(f.height) / static_cast<float>(rt->height);
  } else if (Readable(f.address, u64(f.pitch) * f.height * 4)) {
    // Nothing rendered there on the GPU: the CPU wrote the image.
    MTLTextureDescriptor *d = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                     width:f.width
                                    height:f.height
                                 mipmapped:NO];
    d.storageMode = MTLStorageModeManaged;
    d.usage = MTLTextureUsageShaderRead;
    src = [device newTextureWithDescriptor:d];
    [src replaceRegion:MTLRegionMake2D(0, 0, f.width, f.height)
           mipmapLevel:0
             withBytes:reinterpret_cast<const void *>(f.address)
           bytesPerRow:f.pitch * 4];
  }
  if (src && copy_pipeline) {
    MTLRenderPassDescriptor *rp =
        [MTLRenderPassDescriptor renderPassDescriptor];
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
  if (debug_vs_buf) {
    Commit(true);
    EnsureCommandBuffer();
    const float *d = static_cast<const float *>(debug_vs_buf.contents);
    for (u32 v = 0; v < 8; ++v) {
      DC_INFO("gpu: vs out %u: raw (%g %g %g %g) -> clip (%g %g %g %g)", v,
              d[v * 8], d[v * 8 + 1], d[v * 8 + 2], d[v * 8 + 3], d[v * 8 + 4],
              d[v * 8 + 5], d[v * 8 + 6], d[v * 8 + 7]);
    }
    debug_vsout = 0;
  }
  if (src && dump_frames && frame_count % dump_every == 0) {
    DumpFrame(dst);
  }
  if (!pass_dumps.empty()) {
    Commit(true);
    EnsureCommandBuffer();
    WritePassDumps();
  }
  if (probe_buf && !probe_slots.empty()) {
    Commit(true);
    EnsureCommandBuffer();
    PrintProbes();
  }
  if (!peek_addrs.empty()) {
    Commit(true);
    EnsureCommandBuffer();
    for (u64 a : peek_addrs) {
      if (Readable(a, 32)) {
        const float *v = reinterpret_cast<const float *>(a);
        DC_INFO("gpu: peek %#llx: %g %g %g %g %g %g %g %g", a, v[0], v[1], v[2],
                v[3], v[4], v[5], v[6], v[7]);
      }
    }
    peek_addrs.clear();
  }
  if (const char *peek = std::getenv("DC_PEEK");
      peek && trace) { // debug: DC_PEEK=addr,addr: guest floats
    Commit(true);
    EnsureCommandBuffer();
    for (const char *q = peek; *q;) {
      char *end = nullptr;
      const u64 a = std::strtoull(q, &end, 16);
      if (Readable(a, 32)) {
        const float *v = reinterpret_cast<const float *>(a);
        DC_INFO("gpu: peek %#llx: %g %g %g %g %g %g %g %g", a, v[0], v[1], v[2],
                v[3], v[4], v[5], v[6], v[7]);
      }
      q = *end ? end + 1 : end;
    }
  }
  Commit(false);
  if (src) {
    latest_frame.store((__bridge void *)dst, std::memory_order_release);
    frame_index = (frame_index + 1) % 3;
  }
  if (frame_count <= 3 || frame_count % 300 == 0) {
    DC_INFO("gpu: flip #%llu %#llx %ux%u (%s), %llu draws so far", frame_count,
            f.address, f.width, f.height, rt ? "gpu target" : "cpu memory",
            draw_count);
  }
}

MetalRenderer::MetalRenderer(Device &device) : impl(std::make_unique<Impl>()) {
  impl->device = (__bridge id<MTLDevice>)device.Handle();
  impl->queue = (__bridge id<MTLCommandQueue>)device.Queue();
  impl->Init();
}

MetalRenderer::~MetalRenderer() {
  std::scoped_lock lk(impl->lock);
  impl->Commit(true);
}

void MetalRenderer::Draw(const RegFile &regs, const DrawCall &draw) {
  std::scoped_lock lk(impl->lock);
  @autoreleasepool {
    impl->Draw(regs, draw);
    if (impl->profile) {
      char what[64];
      std::snprintf(what, sizeof(what), "vs %#llx ps %#llx",
                    regs.ProgramAddr(Liverpool::Reg::SPI_SHADER_PGM_LO_VS),
                    regs.ProgramAddr(Liverpool::Reg::SPI_SHADER_PGM_LO_PS));
      impl->ProfileCut(what);
    }
  }
}

void MetalRenderer::Dispatch(const RegFile &regs, std::uint32_t x,
                             std::uint32_t y, std::uint32_t z) {
  std::scoped_lock lk(impl->lock);
  @autoreleasepool {
    impl->Dispatch(regs, x, y, z);
    if (impl->profile) {
      char what[64];
      std::snprintf(what, sizeof(what), "cs %#llx %ux%ux%u",
                    regs.ProgramAddr(Liverpool::Reg::COMPUTE_PGM_LO), x, y, z);
      impl->ProfileCut(what);
    }
  }
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

void MetalRenderer::Signal(std::function<void()> done) {
  std::scoped_lock lk(impl->lock);
  @autoreleasepool {
    impl->Signal(std::move(done));
  }
}

void MetalRenderer::Flip(const FlipInfo &flip) {
  std::scoped_lock lk(impl->lock);
  @autoreleasepool {
    impl->Flip(flip);
  }
}

bool MetalRenderer::DmaData(std::uint32_t dst_sel, std::uint64_t dst,
                            std::uint32_t src_sel, std::uint64_t src,
                            std::uint32_t bytes) {
  std::scoped_lock lk(impl->lock);
  @autoreleasepool {
    return impl->DmaData(dst_sel, dst, src_sel, src, bytes);
  }
}

void MetalRenderer::OnContextRegs(const std::uint32_t *body,
                                  std::uint32_t body_dwords) {
  std::scoped_lock lk(impl->lock);
  impl->OnContextRegs(body, body_dwords);
}

void MetalRenderer::NoteConstDump(std::uint64_t addr, std::uint32_t bytes) {
  std::scoped_lock lk(impl->lock);
  impl->NoteConstDump(addr, bytes);
}

std::uint8_t *MetalRenderer::Gds() {
  return static_cast<std::uint8_t *>(impl->gds.contents);
}

void *MetalRenderer::LatestFrame() const {
  return impl->latest_frame.load(std::memory_order_acquire);
}

} // namespace Gpu::Metal
