// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>

#include "common/types.h"
#include "gpu/liverpool/regs_color.h"
#include "gpu/liverpool/regs_depth.h"
#include "gpu/liverpool/regs_primitive.h"
#include "gpu/liverpool/regs_shader.h"
#include "gpu/liverpool/regs_texture.h"
#include "gpu/liverpool/regs_vertex.h"

namespace Gpu::Liverpool {

#define DO_CONCAT(x, y) x##y
#define CONCAT(x, y) DO_CONCAT(x, y)
#define PAD_DWORDS(n) [[maybe_unused]] std::array<u32, n> CONCAT(pad_, __LINE__)

union Regs {
    static constexpr u32 NumRegs = 0x10000;
    static constexpr u32 ContextRegBase = 0xA000;
    static constexpr u32 ShRegBase = 0x2C00;
    static constexpr u32 ConfigRegBase = 0x2000;
    static constexpr u32 UconfigRegBase = 0xC000;

    struct {
        PAD_DWORDS(11272);
        ShaderProgram ps_program;
        PAD_DWORDS(44);
        ShaderProgram vs_program;
        PAD_DWORDS(44);
        ShaderProgram gs_program;
        PAD_DWORDS(44);
        ShaderProgram es_program;
        PAD_DWORDS(44);
        ShaderProgram hs_program;
        PAD_DWORDS(44);
        ShaderProgram ls_program;
        PAD_DWORDS(164);
        ComputeProgram cs_program;
        PAD_DWORDS(29104);
        DepthRenderControl depth_render_control;
        PAD_DWORDS(1);
        DepthView depth_view;
        DepthRenderOverride depth_render_override;
        PAD_DWORDS(1);
        Address depth_htile_data_base;
        PAD_DWORDS(2);
        float depth_bounds_min;
        float depth_bounds_max;
        u32 stencil_clear;
        float depth_clear;
        Scissor screen_scissor;
        PAD_DWORDS(2);
        DepthBuffer depth_buffer;
        PAD_DWORDS(8);
        BorderColorBuffer ta_bc_base;
        PAD_DWORDS(94);
        WindowOffset window_offset;
        ViewportScissor window_scissor;
        PAD_DWORDS(11);
        ColorBufferMask color_target_mask;
        ColorBufferMask color_shader_mask;
        ViewportScissor generic_scissor;
        PAD_DWORDS(2);
        std::array<ViewportScissor, NUM_VIEWPORTS> viewport_scissors;
        std::array<ViewportDepth, NUM_VIEWPORTS> viewport_depths;
        PAD_DWORDS(46);
        u32 index_offset;
        u32 primitive_restart_index;
        PAD_DWORDS(1);
        BlendConstants blend_constants;
        PAD_DWORDS(2);
        StencilControl stencil_control;
        StencilRefMask stencil_ref_front;
        StencilRefMask stencil_ref_back;
        PAD_DWORDS(1);
        std::array<ViewportBounds, NUM_VIEWPORTS> viewports;
        std::array<ClipUserData, NUM_CLIP_PLANES> clip_user_data;
        PAD_DWORDS(10);
        std::array<PsInputControl, 32> ps_inputs;
        VsOutputConfig vs_output_config;
        PAD_DWORDS(1);
        PsInput ps_input_ena;
        PsInput ps_input_addr;
        PAD_DWORDS(1);
        u32 num_interp : 6;
        PAD_DWORDS(1);
        BarycentricControl barycentric_control;
        PAD_DWORDS(10);
        ShaderPosFormat shader_pos_format;
        ShaderExportFormat z_export_format;
        ColorExportFormat color_export_format;
        PAD_DWORDS(26);
        std::array<BlendControl, NUM_COLOR_BUFFERS> blend_control;
        PAD_DWORDS(17);
        IndexBufferBase index_base_address;
        PAD_DWORDS(1);
        u32 draw_initiator;
        PAD_DWORDS(3);
        DepthControl depth_control;
        PAD_DWORDS(1);
        ColorControl color_control;
        DepthShaderControl depth_shader_control;
        ClipperControl clipper_control;
        PolygonControl polygon_control;
        ViewportControl viewport_control;
        VsOutputControl vs_output_control;
        PAD_DWORDS(122);
        LineControl line_control;
        PAD_DWORDS(4);
        TessFactorClamp hs_clamp;
        PAD_DWORDS(7);
        u32 vgt_gs_mode;
        u32 vgt_gs_onchip_control;
        ModeControl mode_control;
        PAD_DWORDS(5);
        u32 vgt_gsvs_ring_offset_1;
        u32 vgt_gsvs_ring_offset_2;
        u32 vgt_gsvs_ring_offset_3;
        u32 vgt_gs_out_prim_type;
        PAD_DWORDS(1);
        u32 index_size;
        u32 max_index_size;
        u32 index_buffer_type;
        PAD_DWORDS(1);
        u32 enable_primitive_id;
        PAD_DWORDS(3);
        u32 enable_primitive_restart;
        PAD_DWORDS(2);
        u32 vgt_instance_step_rate_0;
        u32 vgt_instance_step_rate_1;
        PAD_DWORDS(1);
        u32 vgt_esgs_ring_itemsize;
        u32 vgt_gsvs_ring_itemsize;
        PAD_DWORDS(33);
        u32 vgt_gs_max_vert_out;
        PAD_DWORDS(6);
        u32 stage_enable;
        u32 ls_hs_config;
        u32 vgt_gs_vert_itemsize[4];
        u32 tess_config;
        PAD_DWORDS(3);
        PolygonOffset poly_offset;
        u32 vgt_gs_instance_cnt;
        u32 vgt_strmout_config;
        u32 vgt_strmout_buffer_config;
        PAD_DWORDS(17);
        AaConfig aa_config;
        PAD_DWORDS(31);
        ColorBuffer color_buffers[NUM_COLOR_BUFFERS];
        PAD_DWORDS(7343);
        u32 cp_strmout_cntl;
        PAD_DWORDS(512);
        u32 vgt_esgs_ring_size;
        u32 vgt_gsvs_ring_size;
        PrimitiveType primitive_type;
        PAD_DWORDS(9);
        u32 num_indices;
        VgtNumInstances num_instances;
    };

    std::array<u32, NumRegs> reg_array;

    void SetDefaults() {
        std::memset(reg_array.data(), 0, sizeof(reg_array));
    }
};

#undef DO_CONCAT
#undef CONCAT
#undef PAD_DWORDS

#define REG_INDEX(field) (offsetof(Regs, field) / sizeof(u32))

static_assert(REG_INDEX(ps_program) == 0x2C08);
static_assert(REG_INDEX(vs_program) == 0x2C48);
static_assert(REG_INDEX(screen_scissor) == 0xA00C);
static_assert(REG_INDEX(depth_buffer.z_info) == 0xA010);
static_assert(REG_INDEX(viewports) == 0xA10F);
static_assert(REG_INDEX(blend_control) == 0xA1E0);
static_assert(REG_INDEX(index_base_address) == 0xA1F9);
static_assert(REG_INDEX(draw_initiator) == 0xA1FC);
static_assert(REG_INDEX(depth_control) == 0xA200);
static_assert(REG_INDEX(color_control) == 0xA202);
static_assert(REG_INDEX(clipper_control) == 0xA204);
static_assert(REG_INDEX(color_buffers[0].base_address) == 0xA318);
static_assert(REG_INDEX(color_buffers[0].pitch) == 0xA319);
static_assert(REG_INDEX(color_buffers[7].base_address) == 0xA381);
static_assert(REG_INDEX(primitive_type) == 0xC242);
static_assert(REG_INDEX(num_indices) == 0xC24C);
static_assert(REG_INDEX(num_instances) == 0xC24D);

#undef REG_INDEX

} // namespace Gpu::Liverpool
