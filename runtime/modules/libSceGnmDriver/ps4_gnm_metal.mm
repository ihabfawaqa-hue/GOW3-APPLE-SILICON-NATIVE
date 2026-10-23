// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PS4 GNM Metal Graphics Backend for PS4Recomp.
 * Translates AMD GCN Liverpool PM4 command streams directly to Apple Metal API.
 */

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "ps4_gnm_metal.h"
#include "ps4_gnmdriver.h"
#include "ps4_metal_screen.h"
#include "spirv_msl.hpp"
#include "ps4_gcn_recompiler.h"
#include "ps4_gcn_translate.h"
#include "ps4_tiling.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>
#include <fstream>

// PM4 Liverpool Opcode constants
#define PM4_OP_NOP                         0x10
#define PM4_OP_SET_BASE                    0x11
#define PM4_OP_CLEAR_STATE                 0x12
#define PM4_OP_INDEX_BUFFER_SIZE           0x13
#define PM4_OP_DISPATCH_DIRECT             0x15
#define PM4_OP_DISPATCH_INDIRECT           0x16
#define PM4_OP_DRAW_INDIRECT               0x24
#define PM4_OP_DRAW_INDEX_INDIRECT         0x25
#define PM4_OP_INDEX_BASE                  0x26
#define PM4_OP_DRAW_INDEX_2                0x27
#define PM4_OP_CONTEXT_CONTROL             0x28
#define PM4_OP_INDEX_TYPE                  0x2A
#define PM4_OP_DRAW_INDIRECT_MULTI         0x2C
#define PM4_OP_DRAW_INDEX_AUTO             0x2D
#define PM4_OP_NUM_INSTANCES               0x2F
#define PM4_OP_DRAW_INDEX_OFFSET_2         0x35
#define PM4_OP_WRITE_DATA                  0x37
#define PM4_OP_DRAW_INDEX_INDIRECT_MULTI   0x38
#define PM4_OP_WAIT_REG_MEM                0x3C
#define PM4_OP_EVENT_WRITE                 0x46
#define PM4_OP_EVENT_WRITE_EOP             0x47
#define PM4_OP_SET_CONFIG_REG              0x68
#define PM4_OP_SET_CONTEXT_REG             0x69
#define PM4_OP_SET_SH_REG                  0x76
#define PM4_OP_SET_UCONFIG_REG             0x79

// Sea Islands GCN Register Base Offsets
#define CONTEXT_REG_BASE                   0xA000
#define SH_REG_BASE                        0x2C00
#define CONFIG_REG_BASE                    0x2000
#define UCONFIG_REG_BASE                   0xC000

// Context Register Offsets (relative to 0xA000), checked against shadPS4's
// GFX6_3D_REG_INDEX assertions for the same Liverpool register layout.
#define CTX_REG_PA_SC_SCREEN_SCISSOR_TL    0x000C
#define CTX_REG_PA_SC_SCREEN_SCISSOR_BR    0x000D
#define CTX_REG_PA_SC_WINDOW_OFFSET        0x0080
#define CTX_REG_PA_SC_WINDOW_SCISSOR_TL    0x0081
#define CTX_REG_PA_SC_WINDOW_SCISSOR_BR    0x0082
#define CTX_REG_PA_SC_GENERIC_SCISSOR_TL   0x0090
#define CTX_REG_PA_SC_GENERIC_SCISSOR_BR   0x0091
#define CTX_REG_PA_SC_VPORT_SCISSOR0_TL    0x0094
#define CTX_REG_PA_SC_VPORT_SCISSOR0_BR    0x0095
#define CTX_REG_PA_CL_VPORT_XSCALE         0x010F
#define CTX_REG_PA_CL_VPORT_XOFFSET        0x0110
#define CTX_REG_PA_CL_VPORT_YSCALE         0x0111
#define CTX_REG_PA_CL_VPORT_YOFFSET        0x0112
#define CTX_REG_PA_CL_VPORT_ZSCALE         0x0113
#define CTX_REG_PA_CL_VPORT_ZOFFSET        0x0114
#define CTX_REG_CB_BLEND0_CONTROL          0x01E0
#define CTX_REG_DB_DEPTH_CONTROL           0x0200
#define CTX_REG_CB_COLOR_CONTROL           0x0202
#define CTX_REG_CB_TARGET_MASK             0x008E
#define CTX_REG_SPI_PS_INPUT_CNTL_0        0x0191
#define CTX_REG_SPI_PS_IN_CONTROL          0x01B6
#define CTX_REG_SPI_PS_INPUT_ENA           0x01B3
#define CTX_REG_SPI_PS_INPUT_ADDR          0x01B4
#define CTX_REG_SPI_SHADER_COL_FORMAT      0x01C5
#define CTX_REG_SPI_SHADER_Z_FORMAT        0x01C3
#define CTX_REG_VGT_INDX_OFFSET            0x0102
#define CTX_REG_PA_CL_VPORT_CONTROL        0x0206
#define CTX_REG_PA_SC_MODE_CONTROL         0x0292
#define CTX_REG_CB_COLOR0_BASE             0x0318
#define CTX_REG_CB_COLOR_STRIDE            0x000F

// VGT_PRIMITIVE_TYPE is UCONFIG register C242, not context register A2A1.
#define UCONFIG_REG_VGT_PRIMITIVE_TYPE    0x0242

// SH Register Offsets (relative to 0x2C00)
#define SH_REG_SPI_SHADER_PGM_LO_PS        0x0008
#define SH_REG_SPI_SHADER_PGM_HI_PS        0x0009
#define SH_REG_SPI_SHADER_USER_DATA_PS_0   0x000C
#define SH_REG_SPI_SHADER_PGM_LO_VS        0x0048
#define SH_REG_SPI_SHADER_PGM_HI_VS        0x0049
#define SH_REG_SPI_SHADER_USER_DATA_VS_0   0x004C
#define SH_REG_COMPUTE_PGM_LO              0x0200
#define SH_REG_COMPUTE_PGM_HI              0x0201
#define SH_REG_COMPUTE_USER_DATA_0         0x0204

// Embedded Metal Shading Language source for built-in baseline and detiling pipelines
static const char *s_default_msl = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexInput {
    float4 position [[attribute(0)]];
    float4 color    [[attribute(1)]];
    float2 texcoord [[attribute(2)]];
};

struct RasterizerData {
    float4 position [[position]];
    float4 color;
    float2 texcoord;
};

vertex RasterizerData default_vertex(uint vid [[vertex_id]],
                                    constant uint8_t *raw_vb [[buffer(0)]],
                                    constant uint32_t *layout_ptr [[buffer(1)]],
                                    constant float4x4 *cbuffer [[buffer(2)]]) {
    RasterizerData out;
    if (raw_vb && layout_ptr && layout_ptr[0] >= 12) {
        uint32_t stride = layout_ptr[0];
        uint32_t position_components = layout_ptr[1];
        constant float *p = (constant float *)(raw_vb + vid * stride);
        float x = p[0];
        float y = p[1];
        float z = p[2];
        float w = (position_components == 3) ? 1.0f : ((stride >= 16) ? p[3] : 1.0f);
        if (w == 0.0f) w = 1.0f;

        // Position transformation (faithful projection via MVP matrix when available)
        if (cbuffer != nullptr && (abs((*cbuffer)[0][0]) > 1e-4f || abs((*cbuffer)[1][1]) > 1e-4f || abs((*cbuffer)[2][2]) > 1e-4f)) {
            // Apply Model-View-Projection matrix from uniform constant buffer
            float4 pos = (*cbuffer) * float4(x, y, z, 1.0f);
            if (pos.w == 0.0f) pos.w = 1.0f;
            out.position = pos;
        } else if (abs(x) <= 1.05f && abs(y) <= 1.05f) {
            // Normalized Device Coordinates (fullscreen/intro quad)
            out.position = float4(x, y, z, 1.0f);
        } else if (x >= 0.0f && x <= 1920.0f && y >= 0.0f && y <= 1080.0f) {
            // Screen-space pixel coordinates
            out.position = float4((x / 960.0f) - 1.0f, 1.0f - (y / 540.0f), z, 1.0f);
        } else {
            // Direct pass-through homogeneous coordinates (no arbitrary normalization)
            out.position = float4(x, y, z, w);
        }

        // Texture coordinates
        if (stride >= 32) {
            out.texcoord = float2(p[6], p[7]);
        } else if (stride >= 24) {
            out.texcoord = float2(p[4], p[5]);
        } else if (stride >= 20) {
            out.texcoord = float2(p[3], p[4]);
        } else {
            out.texcoord = float2((out.position.x + 1.0f) * 0.5f, (1.0f - out.position.y) * 0.5f);
        }

        // Lighting / Color: Lambertian directional lighting if normal is present
        if (stride >= 24) {
            float3 norm = normalize(float3(p[3], p[4], p[5]));
            if (length(norm) > 0.1f) {
                float3 lightDir = normalize(float3(0.577f, 0.577f, 0.577f));
                float diff = max(dot(norm, lightDir), 0.0f) * 0.6f + 0.4f;
                out.color = float4(diff, diff, diff, 1.0f);
            } else {
                out.color = float4(1.0f, 1.0f, 1.0f, 1.0f);
            }
        } else {
            out.color = float4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    } else {
        // Discard geometry if no valid vertex buffer: degenerate point with zero alpha
        out.position = float4(0.0f, 0.0f, 0.0f, 0.0f);
        out.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
        out.texcoord = float2(0.0f, 0.0f);
    }
    return out;
}

fragment float4 default_fragment(RasterizerData in [[stage_in]],
                                 texture2d<float> colorTexture [[texture(0)]],
                                 sampler textureSampler [[sampler(0)]]) {
    if (!is_null_texture(colorTexture)) {
        float4 c = colorTexture.sample(textureSampler, in.texcoord);
        return c * in.color;
    }
    return in.color;
}

// Detiling kernel for AMD GCN 2D micro-tile layout to linear BGRA8
kernel void gcn_detile_kernel(device const uint32_t *src_tiled [[buffer(0)]],
                              texture2d<float, access::write> dst_texture [[texture(0)]],
                              constant uint2 &dimensions [[buffer(1)]],
                              uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dimensions.x || gid.y >= dimensions.y) return;
    uint x = gid.x;
    uint y = gid.y;
    uint tile_x = x / 8;
    uint tile_y = y / 8;
    uint in_x = x % 8;
    uint in_y = y % 8;
    uint tiles_per_row = (dimensions.x + 7) / 8;
    uint tile_idx = tile_y * tiles_per_row + tile_x;
    uint morton = ((in_x & 1) << 0) | ((in_y & 1) << 1) |
                  ((in_x & 2) << 1) | ((in_y & 2) << 2) |
                  ((in_x & 4) << 2) | ((in_y & 4) << 3);
    uint pixel_idx = tile_idx * 64 + morton;
    uint32_t raw_pixel = src_tiled[pixel_idx];
    float b = float((raw_pixel >> 0) & 0xFF) / 255.0;
    float g = float((raw_pixel >> 8) & 0xFF) / 255.0;
    float r = float((raw_pixel >> 16) & 0xFF) / 255.0;
    float a = float((raw_pixel >> 24) & 0xFF) / 255.0;
    dst_texture.write(float4(r, g, b, a), gid);
}
)";

namespace {

struct GcnScissorRegisters {
    // Liverpool reset values produce a full 16K scissor. The render target
    // intersection is applied when this state is translated to Metal.
    uint32_t screen_tl = 0;
    uint32_t screen_br = 0x40004000;
    uint32_t window_offset = 0;
    uint32_t window_tl = 0x80000000; // PA_SC_WINDOW_SCISSOR_TL.offset_disable
    uint32_t window_br = 0x40004000;
    uint32_t generic_tl = 0;
    uint32_t generic_br = 0x40004000;
    uint32_t viewport0_tl = 0x80000000;
    uint32_t viewport0_br = 0x40004000;
    uint32_t mode_control = 0;
};

struct GnmMetalState {
    std::recursive_mutex mutex;
    bool initialized = false;

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> cmdQueue = nil;
    id<MTLLibrary> defaultLibrary = nil;
    id<MTLRenderPipelineState> defaultRenderPipeline = nil;
    id<MTLComputePipelineState> detileComputePipeline = nil;
    id<MTLDepthStencilState> defaultDepthStencilState = nil;
    id<MTLSamplerState> defaultSamplerState = nil;
    bool needsClear = true;

    id<MTLCommandBuffer> currentCmdBuffer = nil;
    id<MTLRenderCommandEncoder> currentRenderEncoder = nil;
    id<MTLComputeCommandEncoder> currentComputeEncoder = nil;

    // Viewport & Scissor
    MTLViewport viewport{0.0, 0.0, 1920.0, 1080.0, 0.0, 1.0};
    MTLScissorRect scissor{0, 0, 1920, 1080};
    GcnScissorRegisters scissorRegs;
    float viewportBounds[6] = {};
    uint32_t viewportControl = 0;
    uint8_t viewportBoundsSeenMask = 0;

    // State Tracking
    MTLPrimitiveType primitiveType = MTLPrimitiveTypeTriangle;
    uint32_t guestPrimitiveType = 4;
    MTLIndexType indexType = MTLIndexTypeUInt16;
    uint64_t indexBufferGpuAddr = 0;
    uint32_t indexBufferSize = 0;

    // Shader addresses
    uint64_t vsGpuAddr = 0;
    uint64_t psGpuAddr = 0;
    uint64_t csGpuAddr = 0;

    // User Data SGPRs (16 registers x 32-bit DWORDs per shader stage)
    uint32_t vsUserData[16] = {0};
    uint32_t psUserData[16] = {0};
    uint32_t csUserData[16] = {0};

    // Render Target & Depth
    uint64_t colorBufferBase[8] = {0};
    uint32_t colorBufferInfo[8] = {0};
    uint64_t depthBufferBase = 0;

    // Blend & Depth Controls
    BOOL blendEnable = NO;
    MTLBlendFactor srcRgbFactor = MTLBlendFactorOne;
    MTLBlendFactor dstRgbFactor = MTLBlendFactorZero;
    MTLBlendOperation rgbOp = MTLBlendOperationAdd;
    MTLBlendFactor srcAlphaFactor = MTLBlendFactorOne;
    MTLBlendFactor dstAlphaFactor = MTLBlendFactorZero;
    MTLBlendOperation alphaOp = MTLBlendOperationAdd;
    MTLColorWriteMask writeMask = MTLColorWriteMaskAll;
    // CB_TARGET_MASK: 4 bits per color target (R, G, B, A); 0xF writes everything.
    uint32_t cbTargetMask = 0xF;
    // VGT_INDX_OFFSET: added to every index of an indexed draw (Metal baseVertex).
    int32_t vgtIndexOffset = 0;
    // Pixel-shader input and color-format registers (see GcnTranslate::FragmentState).
    GcnTranslate::FragmentState fragmentState;

    BOOL depthWriteEnable = NO;
    MTLCompareFunction depthCompareFunc = MTLCompareFunctionAlways;

    // Pipeline & Texture cache
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> renderPipelineCache;
    std::unordered_map<uint64_t, id<MTLComputePipelineState>> computePipelineCache;
    std::unordered_map<uint64_t, id<MTLTexture>> textureCache;
};

static GnmMetalState g_state;
// GOW3_GCN=1 selects the translated shader path (see TryBindTranslatedDraw).
static bool g_gcn_enabled = false;
// GOW3_GCN_DEBUG=1 also prints per-draw inputs, buffers, and flattened descriptors.
static bool g_gcn_debug = false;
// GOW3_GCN_PROBE=1 synchronously samples screen pixels after the selected draws.
static uint32_t g_gcn_probe_limit = 0;
static uint32_t g_gcn_probe_count = 0;
static uint64_t g_gcn_probe_start_draw = 0;
static std::vector<uint64_t> g_gcn_probe_capture_draws;
static std::string g_gcn_probe_capture_dir;
// Optional exact draw whose translated shaders and binding state are written to the debug log.
static uint64_t g_gcn_probe_dump_draw = 0;
// Diagnostic-only: disable blending for the exact shader-address filter in a probe run.
static bool g_gcn_probe_disable_blend = false;
// Shader-address filters keep probes stable when guest worker timing shifts draw ordinals.
static uint64_t g_gcn_probe_vs_addr = 0;
static uint64_t g_gcn_probe_ps_addr = 0;
static uint64_t g_commands_decoded_count = 0;
static uint64_t g_draws_intercepted_count = 0;
static uint64_t g_draws_encoded_count = 0;
static uint64_t g_draws_executed_count = 0;
static uint32_t g_last_gcn_input_count = 0;
static uint64_t g_dispatches_encoded_count = 0;
static uint64_t g_shaders_compiled_count = 0;
static uint64_t g_pipelines_created_count = 0;
static std::atomic<uint64_t> g_last_eop_dst{0};
static std::atomic<uint64_t> g_last_eop_val{0};
static uint64_t g_vertex_buffers_bound_count = 0;
static uint64_t g_index_buffers_bound_count = 0;
static uint64_t g_textures_resolved_count = 0;
static uint64_t g_command_buffers_processed_count = 0;
static uint64_t g_resolved_vb_gpu_addr = 0;
static size_t g_resolved_vb_bytes = 0;
static uint32_t g_resolved_vb_stride = 0;
static uint32_t g_resolved_vb_records = 0;
static uint32_t g_resolved_vb_data_format = 0;
static uint32_t g_resolved_vb_number_format = 0;
static uint32_t g_resolved_vb_components = 0;
static uint64_t g_resolved_cb_gpu_addr = 0;
static size_t g_resolved_cb_bytes = 0;
static uint32_t g_largest_draw_capture_index_count = 0;
static uint32_t g_draw_capture_updates = 0;

static int32_t SignExtend16(uint32_t value) {
    return static_cast<int16_t>(value & 0xFFFFu);
}

static int32_t DecodeScissor15(uint32_t value, unsigned shift) {
    return static_cast<int32_t>((value >> shift) & 0x7FFFu);
}

static int32_t WrapScissor16(int32_t value) {
    return static_cast<int16_t>(static_cast<uint16_t>(value));
}

static void UpdateMetalScissor() {
    const GcnScissorRegisters &regs = g_state.scissorRegs;
    const int32_t screen_left = SignExtend16(regs.screen_tl);
    const int32_t screen_top = SignExtend16(regs.screen_tl >> 16);
    const int32_t screen_right = SignExtend16(regs.screen_br);
    const int32_t screen_bottom = SignExtend16(regs.screen_br >> 16);

    const bool apply_window_offset = (regs.window_tl & 0x8000u) == 0;
    const int32_t offset_x = apply_window_offset ? SignExtend16(regs.window_offset) : 0;
    const int32_t offset_y = apply_window_offset ? SignExtend16(regs.window_offset >> 16) : 0;

    int32_t left = std::max({
        screen_left,
        WrapScissor16(DecodeScissor15(regs.window_tl, 0) + offset_x),
        WrapScissor16(DecodeScissor15(regs.generic_tl, 0) + offset_x),
    });
    int32_t top = std::max({
        screen_top,
        WrapScissor16(DecodeScissor15(regs.window_tl, 16) + offset_y),
        WrapScissor16(DecodeScissor15(regs.generic_tl, 16) + offset_y),
    });
    int32_t right = std::min({
        screen_right,
        WrapScissor16(DecodeScissor15(regs.window_br, 0) + offset_x),
        WrapScissor16(DecodeScissor15(regs.generic_br, 0) + offset_x),
    });
    int32_t bottom = std::min({
        screen_bottom,
        WrapScissor16(DecodeScissor15(regs.window_br, 16) + offset_y),
        WrapScissor16(DecodeScissor15(regs.generic_br, 16) + offset_y),
    });

    if (regs.mode_control & 0x2u) {
        left = std::max(left, DecodeScissor15(regs.viewport0_tl, 0));
        top = std::max(top, DecodeScissor15(regs.viewport0_tl, 16));
        right = std::min(right, DecodeScissor15(regs.viewport0_br, 0));
        bottom = std::min(bottom, DecodeScissor15(regs.viewport0_br, 16));
    }

    // Metal's scissor rectangle uses unsigned target coordinates. Intersect
    // the guest rectangle with the actual 1920x1080 presentation target only
    // after combining the guest's screen/window/generic rectangles.
    const int32_t x0 = std::clamp(left, 0, 1920);
    const int32_t y0 = std::clamp(top, 0, 1080);
    const int32_t x1 = std::clamp(right, 0, 1920);
    const int32_t y1 = std::clamp(bottom, 0, 1080);
    g_state.scissor = {
        static_cast<NSUInteger>(x0),
        static_cast<NSUInteger>(y0),
        static_cast<NSUInteger>(std::max(x1 - x0, 0)),
        static_cast<NSUInteger>(std::max(y1 - y0, 0)),
    };
}

static void UpdateMetalViewport() {
    // Wait until GCN has supplied both XY scale values. Before then the custom
    // backend keeps its known 1920x1080 fallback viewport.
    if ((g_state.viewportBoundsSeenMask & 0x5u) != 0x5u) return;

    const uint32_t enables = g_state.viewportControl;
    const float xscale = (enables & 0x1u) ? g_state.viewportBounds[0] : 1.0f;
    const float xoffset = (enables & 0x2u) ? g_state.viewportBounds[1] : 0.0f;
    const float yscale = (enables & 0x4u) ? g_state.viewportBounds[2] : 1.0f;
    const float yoffset = (enables & 0x8u) ? g_state.viewportBounds[3] : 0.0f;
    if (!std::isfinite(xscale) || !std::isfinite(xoffset) ||
        !std::isfinite(yscale) || !std::isfinite(yoffset) || xscale == 0.0f || yscale == 0.0f) {
        return;
    }

    // GCN X maps as xoffset + xscale * NDC.x. Metal uses the same X direction.
    g_state.viewport.originX = static_cast<double>(xoffset - xscale);
    g_state.viewport.width = static_cast<double>(2.0f * xscale);

    // GCN captures for this title use a negative Y scale to map NDC +1 to the
    // top of the target. Metal's origin is also top-left, but its viewport
    // height is non-negative and maps NDC +1 to the top. Convert the sign into
    // a positive extent instead of passing Vulkan's signed-viewport formula
    // through to Metal (which produced y=1080, height=-1080).
    if (yscale < 0.0f) {
        g_state.viewport.originY = static_cast<double>(yoffset + yscale);
        g_state.viewport.height = static_cast<double>(-2.0f * yscale);
    } else {
        g_state.viewport.originY = static_cast<double>(yoffset - yscale);
        g_state.viewport.height = static_cast<double>(2.0f * yscale);
    }
}

static MTLBlendFactor TranslateBlendFactor(uint32_t factor) {
    switch (factor) {
    case 0: return MTLBlendFactorZero;
    case 1: return MTLBlendFactorOne;
    case 2: return MTLBlendFactorSourceColor;
    case 3: return MTLBlendFactorOneMinusSourceColor;
    case 4: return MTLBlendFactorSourceAlpha;
    case 5: return MTLBlendFactorOneMinusSourceAlpha;
    case 6: return MTLBlendFactorDestinationAlpha;
    case 7: return MTLBlendFactorOneMinusDestinationAlpha;
    case 8: return MTLBlendFactorDestinationColor;
    case 9: return MTLBlendFactorOneMinusDestinationColor;
    case 10: return MTLBlendFactorSourceAlphaSaturated;
    case 13: return MTLBlendFactorBlendColor;
    case 14: return MTLBlendFactorOneMinusBlendColor;
    case 19: return MTLBlendFactorBlendAlpha;
    case 20: return MTLBlendFactorOneMinusBlendAlpha;
    default: return MTLBlendFactorOne;
    }
}

static MTLBlendOperation TranslateBlendOp(uint32_t op) {
    switch (op) {
    case 0: return MTLBlendOperationAdd;
    case 1: return MTLBlendOperationSubtract;
    case 2: return MTLBlendOperationMin;
    case 3: return MTLBlendOperationMax;
    case 4: return MTLBlendOperationReverseSubtract;
    default: return MTLBlendOperationAdd;
    }
}

// PS4 CB_TARGET_MASK uses bit 0=R, 1=G, 2=B, 3=A. Metal's mask bits use
// different positions (R=0x8, G=0x4, B=0x2, A=0x1), so a direct cast drops red
// for the common guest RGB mask 0x7 and writes alpha instead.
static MTLColorWriteMask TranslateColorWriteMask(uint32_t guest_mask) {
    MTLColorWriteMask mask = MTLColorWriteMaskNone;
    if (guest_mask & 0x1u) mask = (MTLColorWriteMask)(mask | MTLColorWriteMaskRed);
    if (guest_mask & 0x2u) mask = (MTLColorWriteMask)(mask | MTLColorWriteMaskGreen);
    if (guest_mask & 0x4u) mask = (MTLColorWriteMask)(mask | MTLColorWriteMaskBlue);
    if (guest_mask & 0x8u) mask = (MTLColorWriteMask)(mask | MTLColorWriteMaskAlpha);
    return mask;
}

static MTLCompareFunction TranslateCompareFunc(uint32_t func) {
    switch (func) {
    case 0: return MTLCompareFunctionNever;
    case 1: return MTLCompareFunctionLess;
    case 2: return MTLCompareFunctionEqual;
    case 3: return MTLCompareFunctionLessEqual;
    case 4: return MTLCompareFunctionGreater;
    case 5: return MTLCompareFunctionNotEqual;
    case 6: return MTLCompareFunctionGreaterEqual;
    case 7: return MTLCompareFunctionAlways;
    default: return MTLCompareFunctionAlways;
    }
}

static MTLPrimitiveType TranslatePrimitiveType(uint32_t type) {
    switch (type) {
    case 1: return MTLPrimitiveTypePoint;
    case 2: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 4: return MTLPrimitiveTypeTriangle;
    case 6: return MTLPrimitiveTypeTriangleStrip;
    case 17: return MTLPrimitiveTypeTriangle;
    default: return MTLPrimitiveTypeTriangle;
    }
}

// Zero-copy Apple Silicon unified memory wrapper
static id<MTLBuffer> WrapGuestMemory(GuestContext *ctx, uint64_t guest_addr, size_t size, size_t *out_offset) {
    if (!ctx || !ctx->mem_base || guest_addr == 0 || size == 0) return nil;
    static long s_page_size = sysconf(_SC_PAGESIZE);
    if (s_page_size <= 0) s_page_size = 16384;

    uint64_t page_mask = (uint64_t)s_page_size - 1;
    uint64_t aligned_addr = guest_addr & ~page_mask;
    size_t offset = (size_t)(guest_addr - aligned_addr);
    size_t aligned_len = (size + offset + page_mask) & ~page_mask;

    if (out_offset) *out_offset = offset;
    void *host_ptr = ctx->mem_base + aligned_addr;

    return [g_state.device newBufferWithBytesNoCopy:host_ptr
                                             length:aligned_len
                                            options:MTLResourceStorageModeShared
                                        deallocator:nil];
}

} // namespace

int ps4_gnm_metal_init(GuestContext *ctx) {
    std::scoped_lock lock(g_state.mutex);
    if (const char *gcn = getenv("GOW3_GCN")) {
        g_gcn_enabled = strcmp(gcn, "1") == 0;
    }
    if (const char *debug = getenv("GOW3_GCN_DEBUG")) {
        g_gcn_debug = strcmp(debug, "1") == 0;
    }
    if (const char *probe = getenv("GOW3_GCN_PROBE")) {
        if (strcmp(probe, "1") == 0 && g_gcn_debug) {
            g_gcn_probe_limit = 200;
            if (const char *limit = getenv("GOW3_GCN_PROBE_DRAWS")) {
                char *end = nullptr;
                const unsigned long parsed = strtoul(limit, &end, 10);
                if (end != limit && *end == '\0') {
                    g_gcn_probe_limit = (uint32_t)std::min(parsed, 2000ul);
                }
            }
            if (const char *start = getenv("GOW3_GCN_PROBE_START_DRAW")) {
                char *end = nullptr;
                const unsigned long long parsed = strtoull(start, &end, 10);
                if (end != start && *end == '\0') g_gcn_probe_start_draw = parsed;
            }
            if (const char *capture_draws = getenv("GOW3_GCN_PROBE_CAPTURE_DRAWS")) {
                const char *cursor = capture_draws;
                while (*cursor && g_gcn_probe_capture_draws.size() < 8) {
                    while (*cursor == ',' || *cursor == ' ' || *cursor == '\t') ++cursor;
                    if (!*cursor) break;
                    char *end = nullptr;
                    const unsigned long long parsed = strtoull(cursor, &end, 0);
                    if (end != cursor && parsed != 0) {
                        g_gcn_probe_capture_draws.push_back((uint64_t)parsed);
                        cursor = end;
                    } else {
                        while (*cursor && *cursor != ',') ++cursor;
                    }
                }
            }
            if (const char *capture_dir = getenv("GOW3_GCN_PROBE_CAPTURE_DIR")) {
                g_gcn_probe_capture_dir = capture_dir;
            }
            if (const char *dump_draw = getenv("GOW3_GCN_PROBE_DUMP_DRAW")) {
                char *end = nullptr;
                const unsigned long long parsed = strtoull(dump_draw, &end, 10);
                if (end != dump_draw && *end == '\0') g_gcn_probe_dump_draw = parsed;
            }
            if (const char *disable_blend = getenv("GOW3_GCN_PROBE_DISABLE_BLEND")) {
                g_gcn_probe_disable_blend = strcmp(disable_blend, "1") == 0;
            }
            if (const char *vs_addr = getenv("GOW3_GCN_PROBE_VS")) {
                char *end = nullptr;
                const unsigned long long parsed = strtoull(vs_addr, &end, 0);
                if (end != vs_addr && *end == '\0') g_gcn_probe_vs_addr = parsed;
            }
            if (const char *ps_addr = getenv("GOW3_GCN_PROBE_PS")) {
                char *end = nullptr;
                const unsigned long long parsed = strtoull(ps_addr, &end, 0);
                if (end != ps_addr && *end == '\0') g_gcn_probe_ps_addr = parsed;
            }
        }
    }
    if (ctx && ctx->mem_base) {
        GcnTranslate::SetGuestMemory(ctx->mem_base, ctx->mem_size);
    }
    fprintf(stderr, "[ps4-gnm-metal] GCN translated shader path: %s\n", g_gcn_enabled ? "enabled (GOW3_GCN=1)" : "disabled");
    if (g_state.initialized) return 0;

    @autoreleasepool {
        g_state.device = (__bridge id<MTLDevice>)ps4_metal_screen_get_device();
        if (!g_state.device) {
            g_state.device = MTLCreateSystemDefaultDevice();
        }
        if (!g_state.device) {
            fprintf(stderr, "[ps4-gnm-metal] Failed to obtain Metal device\n");
            return -1;
        }

        g_state.cmdQueue = (__bridge id<MTLCommandQueue>)ps4_metal_screen_get_command_queue();
        if (!g_state.cmdQueue) {
            g_state.cmdQueue = [g_state.device newCommandQueue];
        }

        // Compile built-in default baseline library
        NSError *error = nil;
        NSString *mslSource = [NSString stringWithUTF8String:s_default_msl];
        g_state.defaultLibrary = [g_state.device newLibraryWithSource:mslSource options:nil error:&error];
        if (!g_state.defaultLibrary) {
            fprintf(stderr, "[ps4-gnm-metal] Failed to compile default MSL shaders: %s\n",
                    error.localizedDescription.UTF8String);
            return -1;
        }

        id<MTLFunction> vertFunc = [g_state.defaultLibrary newFunctionWithName:@"default_vertex"];
        id<MTLFunction> fragFunc = [g_state.defaultLibrary newFunctionWithName:@"default_fragment"];

        MTLRenderPipelineDescriptor *pDesc = [[MTLRenderPipelineDescriptor alloc] init];
        pDesc.vertexFunction = vertFunc;
        pDesc.fragmentFunction = fragFunc;
        pDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pDesc.colorAttachments[0].blendingEnabled = NO;

        g_state.defaultRenderPipeline = [g_state.device newRenderPipelineStateWithDescriptor:pDesc error:&error];
        if (!g_state.defaultRenderPipeline) {
            fprintf(stderr, "[ps4-gnm-metal] Failed to create default render pipeline: %s\n",
                    error.localizedDescription.UTF8String);
            return -1;
        }

        // Compile detile compute kernel
        id<MTLFunction> detileKernel = [g_state.defaultLibrary newFunctionWithName:@"gcn_detile_kernel"];
        if (detileKernel) {
            g_state.detileComputePipeline = [g_state.device newComputePipelineStateWithFunction:detileKernel error:&error];
        }

        // Default Depth-Stencil State
        MTLDepthStencilDescriptor *dsDesc = [[MTLDepthStencilDescriptor alloc] init];
        dsDesc.depthWriteEnabled = NO;
        dsDesc.depthCompareFunction = MTLCompareFunctionAlways;
        g_state.defaultDepthStencilState = [g_state.device newDepthStencilStateWithDescriptor:dsDesc];

        // Default Texture Sampler State (Linear Clamp)
        MTLSamplerDescriptor *sDesc = [[MTLSamplerDescriptor alloc] init];
        sDesc.minFilter = MTLSamplerMinMagFilterLinear;
        sDesc.magFilter = MTLSamplerMinMagFilterLinear;
        sDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        g_state.defaultSamplerState = [g_state.device newSamplerStateWithDescriptor:sDesc];

        fprintf(stderr, "[ps4-gnm-metal] Default MSL shaders compiled successfully (vertex=default_vertex, fragment=default_fragment)\n");
        fprintf(stderr, "[ps4-gnm-metal] Default render pipeline state created successfully\n");
        // Self-test texture descriptor decoding against captured GOW3 GNF data
        if (ps4_gnm_test_texture_descriptor_decoding() != 0) {
            fprintf(stderr, "[ps4-gnm-metal] FATAL: Texture descriptor decoding validation failed!\n");
            return -1;
        }
        fprintf(stderr, "[ps4-gnm-metal] Texture descriptor decoder validated against real GOW3 register data\n");

        if (g_state.detileComputePipeline) {
            fprintf(stderr, "[ps4-gnm-metal] GCN detile compute pipeline created successfully\n");
        }
        g_state.initialized = true;
        fprintf(stderr, "[ps4-gnm-metal] Native Apple Metal GNM Command Processor initialized (%s)\n",
                g_state.device.name.UTF8String);
    }
    return 0;
}

static id<MTLBuffer> ResolveVertexBuffer(GuestContext *ctx, size_t *out_offset,
                                         uint32_t *out_stride,
                                         uint32_t *out_position_components) {
    g_resolved_vb_gpu_addr = 0;
    g_resolved_vb_bytes = 0;
    g_resolved_vb_stride = 0;
    g_resolved_vb_records = 0;
    g_resolved_vb_data_format = 0;
    g_resolved_vb_number_format = 0;
    g_resolved_vb_components = 0;
    if (out_position_components) *out_position_components = 0;
    if (!ctx || !ctx->mem_base) return nil;

    // The guest fetch program's first S_LOAD reads V# descriptor dwords at offsets 0..3
    // from the table referenced by VS user-data SGPRs 2:3. Prefer this vertex attribute over
    // unrelated direct resources later in user data (for example the transform palette).
    const uint64_t fetch_table_addr = ((uint64_t)g_state.vsUserData[3] << 32) |
                                      (uint64_t)g_state.vsUserData[2];
    const uint64_t fetch_shader_addr = ((uint64_t)g_state.vsUserData[1] << 32) |
                                       (uint64_t)g_state.vsUserData[0];
    if (fetch_table_addr && fetch_table_addr <= ctx->mem_size &&
        ctx->mem_size - (size_t)fetch_table_addr >= 16) {
        uint32_t descriptor[4];
        memcpy(descriptor, ctx->mem_base + fetch_table_addr, sizeof(descriptor));
        const uint64_t base_hi_stride = descriptor[1];
        const uint64_t base_addr = ((base_hi_stride & 0xFFFFu) << 32) | descriptor[0];
        const uint32_t stride = (uint32_t)((base_hi_stride >> 16) & 0x3FFFu);
        const uint32_t num_records = descriptor[2];
        const uint32_t data_format = (descriptor[3] >> 15) & 0x3Fu;
        const uint32_t number_format = (descriptor[3] >> 12) & 0x7u;
        const uint32_t position_components = GcnRecompiler::PositionComponentCount(data_format, number_format);
        if (base_addr >= 0x10000 && base_addr < ctx->mem_size && num_records && stride >= 8) {
            size_t buf_bytes = (size_t)num_records * stride;
            const size_t available = ctx->mem_size - (size_t)base_addr;
            if (buf_bytes > available) buf_bytes = available;
            if (buf_bytes < stride) return nil;
            if (out_stride) *out_stride = stride;
            if (out_position_components) *out_position_components = position_components;
            g_resolved_vb_gpu_addr = base_addr;
            g_resolved_vb_bytes = buf_bytes;
            g_resolved_vb_stride = stride;
            g_resolved_vb_records = num_records;
            g_resolved_vb_data_format = data_format;
            g_resolved_vb_number_format = number_format;
            g_resolved_vb_components = position_components;
            static uint32_t s_fetch_attribute_logs = 0;
            if (s_fetch_attribute_logs++ < 8) {
                fprintf(stderr, "[ps4-gnm-metal] Fetch attribute 0 selected: descriptor=0x%llx buffer=0x%llx stride=%u records=%u data_format=%u number_format=%u position_components=%u\n",
                        (unsigned long long)fetch_table_addr, (unsigned long long)base_addr,
                        stride, num_records, data_format, number_format, position_components);
            }
            return WrapGuestMemory(ctx, base_addr, buf_bytes, out_offset);
        }
    }

    // 1. Check vsUserData for direct V# Buffer Descriptors (16 bytes / 4 DWORDs)
    for (int slot = 0; slot < 4; slot++) {
        uint64_t base_lo = g_state.vsUserData[slot * 4 + 0];
        uint64_t base_hi_stride = g_state.vsUserData[slot * 4 + 1];
        uint32_t num_records = (uint32_t)g_state.vsUserData[slot * 4 + 2];
        uint32_t descriptor_format = g_state.vsUserData[slot * 4 + 3];

        uint64_t base_addr = ((base_hi_stride & 0xFFFFu) << 32) | (base_lo & 0xFFFFFFFFu);
        uint32_t stride = (base_hi_stride >> 16) & 0x3FFFu;

        if (base_addr >= 0x10000 && base_addr < ctx->mem_size && num_records > 0 && stride >= 8) {
            size_t buf_bytes = (size_t)num_records * stride;
            if (buf_bytes > 0x10000000) buf_bytes = 0x10000000;
            if (out_stride) *out_stride = stride;
            const uint32_t data_format = (descriptor_format >> 15) & 0x3Fu;
            const uint32_t number_format = (descriptor_format >> 12) & 0x7u;
            const uint32_t position_components = GcnRecompiler::PositionComponentCount(data_format, number_format);
            if (out_position_components) *out_position_components = position_components;
            g_resolved_vb_gpu_addr = base_addr;
            g_resolved_vb_bytes = buf_bytes;
            g_resolved_vb_stride = stride;
            g_resolved_vb_records = num_records;
            g_resolved_vb_data_format = data_format;
            g_resolved_vb_number_format = number_format;
            g_resolved_vb_components = position_components;
            return WrapGuestMemory(ctx, base_addr, buf_bytes, out_offset);
        }
    }

    // 2. Check each 64-bit pair in vsUserData as a potential SRT pointer
    for (int pair = 0; pair < 8; pair++) {
        uint64_t srt_ptr = ((uint64_t)g_state.vsUserData[pair * 2 + 1] << 32) | (uint64_t)g_state.vsUserData[pair * 2];
        if (srt_ptr == fetch_shader_addr) continue;
        if (srt_ptr >= 0x10000 && srt_ptr + 16 <= ctx->mem_size) {
            const uint32_t *srt = (const uint32_t *)(ctx->mem_base + srt_ptr);
            for (int entry = 0; entry < 16; entry++) {
                if (srt_ptr + (entry + 1) * 16 > ctx->mem_size) break;
                uint64_t base_lo = srt[entry * 4 + 0];
                uint64_t base_hi_stride = srt[entry * 4 + 1];
                uint32_t num_records = srt[entry * 4 + 2];
                uint32_t descriptor_format = srt[entry * 4 + 3];
                uint64_t base_addr = ((base_hi_stride & 0xFFFFu) << 32) | (base_lo & 0xFFFFFFFFu);
                uint32_t stride = (base_hi_stride >> 16) & 0x3FFFu;
                if (base_addr >= 0x10000 && base_addr < ctx->mem_size && num_records > 0 && stride >= 8) {
                    size_t buf_bytes = (size_t)num_records * stride;
                    if (buf_bytes > 0x10000000) buf_bytes = 0x10000000;
                    if (out_stride) *out_stride = stride;
                    const uint32_t data_format = (descriptor_format >> 15) & 0x3Fu;
                    const uint32_t number_format = (descriptor_format >> 12) & 0x7u;
                    const uint32_t position_components = GcnRecompiler::PositionComponentCount(data_format, number_format);
                    if (out_position_components) *out_position_components = position_components;
                    g_resolved_vb_gpu_addr = base_addr;
                    g_resolved_vb_bytes = buf_bytes;
                    g_resolved_vb_stride = stride;
                    g_resolved_vb_records = num_records;
                    g_resolved_vb_data_format = data_format;
                    g_resolved_vb_number_format = number_format;
                    g_resolved_vb_components = position_components;
                    return WrapGuestMemory(ctx, base_addr, buf_bytes, out_offset);
                }
            }
        }
    }

    return nil;
}

static id<MTLBuffer> ResolveConstantBuffer(GuestContext *ctx, size_t *out_offset) {
    g_resolved_cb_gpu_addr = 0;
    g_resolved_cb_bytes = 0;
    if (!ctx || !ctx->mem_base) return nil;

    const uint64_t fetch_shader_addr = ((uint64_t)g_state.vsUserData[1] << 32) |
                                       (uint64_t)g_state.vsUserData[0];
    // 1. Check vsUserData for direct C# Buffer Descriptors (stride == 0, num_records >= 64)
    for (int slot = 0; slot < 4; slot++) {
        uint64_t base_lo = g_state.vsUserData[slot * 4 + 0];
        uint64_t base_hi_stride = g_state.vsUserData[slot * 4 + 1];
        uint32_t num_records = (uint32_t)g_state.vsUserData[slot * 4 + 2];
        uint64_t base_addr = ((base_hi_stride & 0xFFFFu) << 32) | (base_lo & 0xFFFFFFFFu);
        uint32_t stride = (base_hi_stride >> 16) & 0x3FFFu;
        if (base_addr == fetch_shader_addr) continue;
        if (stride == 0 && base_addr >= 0x10000 && base_addr < ctx->mem_size && num_records >= 64) {
            size_t cb_bytes = (num_records <= 65536) ? (size_t)num_records : 256;
            g_resolved_cb_gpu_addr = base_addr;
            g_resolved_cb_bytes = cb_bytes;
            return WrapGuestMemory(ctx, base_addr, cb_bytes, out_offset);
        }
    }

    // 2. Check each 64-bit pair in vsUserData as a potential SRT pointer
    for (int pair = 0; pair < 8; pair++) {
        uint64_t srt_ptr = ((uint64_t)g_state.vsUserData[pair * 2 + 1] << 32) | (uint64_t)g_state.vsUserData[pair * 2];
        if (srt_ptr == fetch_shader_addr) continue;
        if (srt_ptr >= 0x10000 && srt_ptr + 16 <= ctx->mem_size) {
            const uint32_t *srt = (const uint32_t *)(ctx->mem_base + srt_ptr);
            // Search first 8 entries in SRT for uniform constant buffer
            for (int entry = 0; entry < 8; entry++) {
                if (srt_ptr + (entry + 1) * 16 > ctx->mem_size) break;
                uint64_t base_lo = srt[entry * 4 + 0];
                uint64_t base_hi_stride = srt[entry * 4 + 1];
                uint32_t stride = (base_hi_stride >> 16) & 0x3FFFu;
                uint32_t num_records = srt[entry * 4 + 2];
                uint64_t base_addr = ((base_hi_stride & 0xFFFFu) << 32) | (base_lo & 0xFFFFFFFFu);
                if (stride == 0 && base_addr >= 0x10000 && base_addr < ctx->mem_size && num_records >= 64) {
                    size_t cb_bytes = (num_records <= 65536) ? (size_t)num_records : 256;
                    g_resolved_cb_gpu_addr = base_addr;
                    g_resolved_cb_bytes = cb_bytes;
                    return WrapGuestMemory(ctx, base_addr, cb_bytes, out_offset);
                }
            }
        }
    }

    return nil;
}

static size_t CaptureGuestRange(GuestContext *ctx, const char *directory, const char *name,
                                uint64_t guest_addr, size_t requested_bytes) {
    if (!ctx || !ctx->mem_base || !directory || !name || requested_bytes == 0 ||
        guest_addr >= (uint64_t)ctx->mem_size) return 0;
    const size_t available = ctx->mem_size - (size_t)guest_addr;
    const size_t capped_request = requested_bytes > (8u * 1024u * 1024u)
        ? (8u * 1024u * 1024u) : requested_bytes;
    const size_t bytes = capped_request < available ? capped_request : available;
    char path[2048];
    int path_len = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (path_len <= 0 || (size_t)path_len >= sizeof(path)) return 0;
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    size_t written = fwrite(ctx->mem_base + guest_addr, 1, bytes, file);
    fclose(file);
    return written;
}

static void CaptureLargestDrawState(GuestContext *ctx, uint64_t dcb_addr, size_t dcb_size,
                                    uint32_t packet_offset_dw, uint32_t opcode,
                                    const uint32_t *payload, uint32_t payload_dw,
                                    uint64_t index_addr, uint32_t index_count,
                                    size_t index_bytes) {
    const char *directory = getenv("PS4_GPU_CAPTURE_DIR");
    if (!directory || !directory[0] || !ctx || !ctx->mem_base || index_count < 256 ||
        index_count <= g_largest_draw_capture_index_count || g_draw_capture_updates >= 8) return;
    g_largest_draw_capture_index_count = index_count;
    ++g_draw_capture_updates;

    const size_t dcb_written = CaptureGuestRange(ctx, directory, "draw_largest_dcb.bin", dcb_addr, dcb_size);
    const size_t vb_written = CaptureGuestRange(ctx, directory, "draw_largest_vertex.bin",
                                                g_resolved_vb_gpu_addr, g_resolved_vb_bytes);
    const size_t ib_written = CaptureGuestRange(ctx, directory, "draw_largest_index.bin", index_addr, index_bytes);
    const size_t cb_written = CaptureGuestRange(ctx, directory, "draw_largest_constant_candidate.bin",
                                                g_resolved_cb_gpu_addr, g_resolved_cb_bytes);
    const uint64_t fetch_shader_addr = ((uint64_t)g_state.vsUserData[1] << 32) |
                                       (uint64_t)g_state.vsUserData[0];
    const uint64_t vertex_descriptor_table_addr = ((uint64_t)g_state.vsUserData[3] << 32) |
                                                 (uint64_t)g_state.vsUserData[2];
    const size_t fetch_shader_capture_bytes = 256;
    const size_t vertex_descriptor_table_capture_bytes = 256;
    const size_t fetch_shader_written = fetch_shader_addr
        ? CaptureGuestRange(ctx, directory, "draw_largest_fetch.bin", fetch_shader_addr,
                            fetch_shader_capture_bytes) : 0;
    const size_t vertex_descriptor_table_written = vertex_descriptor_table_addr
        ? CaptureGuestRange(ctx, directory, "draw_largest_vertex_descriptors.bin",
                            vertex_descriptor_table_addr, vertex_descriptor_table_capture_bytes) : 0;
    const uint32_t fetch_attribute_offsets_dw[3] = {0, 0x38, 0x3c};
    uint64_t fetch_attribute_addrs[3] = {};
    uint32_t fetch_attribute_strides[3] = {};
    uint32_t fetch_attribute_records[3] = {};
    uint32_t fetch_attribute_data_formats[3] = {};
    uint32_t fetch_attribute_number_formats[3] = {};
    uint32_t fetch_attribute_components[3] = {};
    size_t fetch_attribute_bytes[3] = {};
    size_t fetch_attribute_written[3] = {};
    for (size_t i = 0; i < 3; ++i) {
        const uint64_t descriptor_addr = vertex_descriptor_table_addr +
                                         (uint64_t)fetch_attribute_offsets_dw[i] * 4;
        if (!vertex_descriptor_table_addr || descriptor_addr > ctx->mem_size ||
            ctx->mem_size - (size_t)descriptor_addr < 16) continue;
        uint32_t descriptor[4];
        memcpy(descriptor, ctx->mem_base + descriptor_addr, sizeof(descriptor));
        const uint64_t base_hi_stride = descriptor[1];
        const uint64_t base_addr = ((base_hi_stride & 0xFFFFu) << 32) | descriptor[0];
        const uint32_t stride = (uint32_t)((base_hi_stride >> 16) & 0x3FFFu);
        const uint32_t records = descriptor[2];
        const uint32_t data_format = (descriptor[3] >> 15) & 0x3Fu;
        const uint32_t number_format = (descriptor[3] >> 12) & 0x7u;
        if (base_addr < 0x10000 || base_addr >= ctx->mem_size || !records || stride < 8) continue;
        fetch_attribute_addrs[i] = base_addr;
        fetch_attribute_strides[i] = stride;
        fetch_attribute_records[i] = records;
        fetch_attribute_data_formats[i] = data_format;
        fetch_attribute_number_formats[i] = number_format;
        fetch_attribute_components[i] = GcnRecompiler::PositionComponentCount(data_format, number_format);
        fetch_attribute_bytes[i] = (size_t)records * stride;
        char attribute_name[64];
        snprintf(attribute_name, sizeof(attribute_name), "draw_largest_attribute%zu.bin", i);
        fetch_attribute_written[i] = CaptureGuestRange(ctx, directory, attribute_name, base_addr,
                                                       fetch_attribute_bytes[i]);
    }

    GcnShaderMetadata vs_meta;
    GcnShaderMetadata ps_meta;
    size_t vs_bytes = 0;
    size_t ps_bytes = 0;
    const size_t shader_probe_limit = 64u * 1024u;
    if (g_state.vsGpuAddr < ctx->mem_size) {
        const size_t available = ctx->mem_size - (size_t)g_state.vsGpuAddr;
        const size_t probe = available < shader_probe_limit ? available : shader_probe_limit;
        if (GcnRecompiler::AnalyzeShader(ctx->mem_base + g_state.vsGpuAddr, probe, GcnStage::Vertex, vs_meta)) {
            vs_bytes = vs_meta.code_size_bytes < probe ? vs_meta.code_size_bytes : probe;
        }
    }
    if (g_state.psGpuAddr < ctx->mem_size) {
        const size_t available = ctx->mem_size - (size_t)g_state.psGpuAddr;
        const size_t probe = available < shader_probe_limit ? available : shader_probe_limit;
        if (GcnRecompiler::AnalyzeShader(ctx->mem_base + g_state.psGpuAddr, probe, GcnStage::Pixel, ps_meta)) {
            ps_bytes = ps_meta.code_size_bytes < probe ? ps_meta.code_size_bytes : probe;
        }
    }
    const size_t vs_written = CaptureGuestRange(ctx, directory, "draw_largest_vs.bin", g_state.vsGpuAddr, vs_bytes);
    const size_t ps_written = CaptureGuestRange(ctx, directory, "draw_largest_ps.bin", g_state.psGpuAddr, ps_bytes);

    char manifest_path[2048];
    int path_len = snprintf(manifest_path, sizeof(manifest_path), "%s/draw_largest.json", directory);
    FILE *manifest = (path_len > 0 && (size_t)path_len < sizeof(manifest_path))
        ? fopen(manifest_path, "w") : nullptr;
    if (manifest) {
        fprintf(manifest,
                "{\n  \"capture_scope\": \"largest observed indexed draw with at least 256 indices; state and guest ranges only\",\n"
                "  \"replay_status\": \"NOT_REPLAYED_CAPTURE_ONLY\",\n"
                "  \"capture_update\": %u,\n"
                "  \"dcb\": {\"address\": \"0x%llx\", \"declared_bytes\": %zu, \"saved_bytes\": %zu, \"packet_offset_dwords\": %u, \"opcode\": %u},\n"
                "  \"draw\": {\"index_address\": \"0x%llx\", \"index_count\": %u, \"index_bytes\": %zu, \"primitive_type\": %u, \"index_type\": %u},\n"
                "  \"viewport\": {\"x\": %.6f, \"y\": %.6f, \"width\": %.6f, \"height\": %.6f, \"znear\": %.6f, \"zfar\": %.6f},\n"
                "  \"scissor\": {\"x\": %lu, \"y\": %lu, \"width\": %lu, \"height\": %lu},\n"
                "  \"raster_registers\": {\"gcn_primitive_type\": %u, \"viewport_control\": \"0x%08x\", \"viewport_bounds\": [%.6f, %.6f, %.6f, %.6f, %.6f, %.6f], \"viewport_seen_mask\": %u, \"screen_scissor\": [\"0x%08x\", \"0x%08x\"], \"window_offset\": \"0x%08x\", \"window_scissor\": [\"0x%08x\", \"0x%08x\"], \"generic_scissor\": [\"0x%08x\", \"0x%08x\"], \"mode_control\": \"0x%08x\"},\n"
                "  \"vertex_buffer\": {\"address\": \"0x%llx\", \"declared_bytes\": %zu, \"saved_bytes\": %zu, \"stride\": %u, \"records\": %u, \"data_format\": %u, \"number_format\": %u, \"position_components\": %u},\n"
                "  \"constant_buffer_candidate\": {\"address\": \"0x%llx\", \"declared_bytes\": %zu, \"saved_bytes\": %zu},\n"
                "  \"fetch_shader\": {\"address\": \"0x%llx\", \"capture_window_bytes\": %zu, \"saved_bytes\": %zu},\n"
                "  \"vertex_descriptor_table\": {\"address\": \"0x%llx\", \"capture_window_bytes\": %zu, \"saved_bytes\": %zu},\n"
                "  \"shaders\": {\"vs_address\": \"0x%llx\", \"vs_saved_bytes\": %zu, \"vs_hash_fnv1a\": \"0x%llx\", \"vs_uses_cbuffer\": %s, \"vs_has_position_export\": %s, \"ps_address\": \"0x%llx\", \"ps_saved_bytes\": %zu, \"ps_hash_fnv1a\": \"0x%llx\"},\n",
                g_draw_capture_updates,
                (unsigned long long)dcb_addr, dcb_size, dcb_written, packet_offset_dw, opcode,
                (unsigned long long)index_addr, index_count, index_bytes,
                (unsigned)g_state.primitiveType, (unsigned)g_state.indexType,
                g_state.viewport.originX, g_state.viewport.originY,
                g_state.viewport.width, g_state.viewport.height,
                g_state.viewport.znear, g_state.viewport.zfar,
                (unsigned long)g_state.scissor.x, (unsigned long)g_state.scissor.y,
                (unsigned long)g_state.scissor.width, (unsigned long)g_state.scissor.height,
                g_state.guestPrimitiveType, g_state.viewportControl,
                g_state.viewportBounds[0], g_state.viewportBounds[1],
                g_state.viewportBounds[2], g_state.viewportBounds[3],
                g_state.viewportBounds[4], g_state.viewportBounds[5],
                g_state.viewportBoundsSeenMask,
                g_state.scissorRegs.screen_tl, g_state.scissorRegs.screen_br,
                g_state.scissorRegs.window_offset,
                g_state.scissorRegs.window_tl, g_state.scissorRegs.window_br,
                g_state.scissorRegs.generic_tl, g_state.scissorRegs.generic_br,
                g_state.scissorRegs.mode_control,
                (unsigned long long)g_resolved_vb_gpu_addr, g_resolved_vb_bytes, vb_written,
                g_resolved_vb_stride, g_resolved_vb_records,
                g_resolved_vb_data_format, g_resolved_vb_number_format, g_resolved_vb_components,
                (unsigned long long)g_resolved_cb_gpu_addr, g_resolved_cb_bytes, cb_written,
                (unsigned long long)fetch_shader_addr, fetch_shader_capture_bytes, fetch_shader_written,
                (unsigned long long)vertex_descriptor_table_addr,
                vertex_descriptor_table_capture_bytes, vertex_descriptor_table_written,
                (unsigned long long)g_state.vsGpuAddr, vs_written, (unsigned long long)vs_meta.hash,
                vs_meta.uses_cbuffer ? "true" : "false",
                vs_meta.has_position_export ? "true" : "false",
                (unsigned long long)g_state.psGpuAddr, ps_written, (unsigned long long)ps_meta.hash);
        fprintf(manifest, "  \"fetch_attributes\": [\n");
        for (size_t i = 0; i < 3; ++i) {
            fprintf(manifest,
                    "    %s{\"index\": %zu, \"descriptor_offset_dwords\": %u, \"address\": \"0x%llx\", \"stride\": %u, \"records\": %u, \"data_format\": %u, \"number_format\": %u, \"position_components\": %u, \"requested_bytes\": %zu, \"saved_bytes\": %zu}",
                    i ? ",\n" : "", i, fetch_attribute_offsets_dw[i],
                    (unsigned long long)fetch_attribute_addrs[i], fetch_attribute_strides[i],
                    fetch_attribute_records[i], fetch_attribute_data_formats[i],
                    fetch_attribute_number_formats[i], fetch_attribute_components[i],
                    fetch_attribute_bytes[i], fetch_attribute_written[i]);
        }
        fprintf(manifest, "\n  ],\n  \"vs_user_data\": [");
        for (size_t i = 0; i < 16; ++i) fprintf(manifest, "%s%u", i ? ", " : "", g_state.vsUserData[i]);
        fprintf(manifest, "],\n  \"ps_user_data\": [");
        for (size_t i = 0; i < 16; ++i) fprintf(manifest, "%s%u", i ? ", " : "", g_state.psUserData[i]);
        fprintf(manifest, "],\n  \"draw_packet_words\": [");
        for (uint32_t i = 0; payload && i < payload_dw; ++i) fprintf(manifest, "%s%u", i ? ", " : "", payload[i]);
        fprintf(manifest, "]\n}\n");
        fclose(manifest);
    }
    fprintf(stderr, "[ps4-gnm-metal] GPU_CAPTURE: draw_largest.json update=%u index_count=%u dcb=%zuB vb=%zuB ib=%zuB cb_candidate=%zuB fetch=%zuB descriptors=%zuB vs=%zuB ps=%zuB; replay not implemented\n",
            g_draw_capture_updates, index_count, dcb_written, vb_written, ib_written, cb_written,
            fetch_shader_written, vertex_descriptor_table_written, vs_written, ps_written);
}

struct DecodedTextureInfo {
    uint64_t addr = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t dfmt = 0;
    uint32_t nfmt = 0;
    uint32_t imageType = 0;
    MTLPixelFormat pixelFormat = MTLPixelFormatInvalid;
    size_t bytesPerRow = 0;
    size_t totalBytes = 0;
    uint32_t tileMode = 31;        // T# tiling_index; 31 is DisplayLinearGeneral
    uint32_t pitchPixels = 0;      // T# pitch, in pixels
    uint32_t bitsPerElement = 32;  // one pixel, or one block for compressed formats
    bool blockCompressed = false;
    bool valid = false;
};

static DecodedTextureInfo DecodeTextureDescriptorWords(const uint32_t *dw, size_t num_words, size_t mem_size) {
    DecodedTextureInfo info;
    // A 2D image descriptor needs words 0..4: base/format, dimensions/type,
    // and the pitch used to read its backing guest memory.
    if (!dw || num_words < 5) return info;

    const uint32_t dims = dw[2];
    const uint32_t tw = (dims & 0x3FFFu) + 1u;
    const uint32_t th = ((dims >> 14) & 0x3FFFu) + 1u;
    if (tw < 4 || th < 4 || tw > 8192 || th > 8192) return info;

    // GCN Image descriptor layout (Sea Islands): format fields are in dw[1],
    // ImageType is dw[3][31:28], and pitch is dw[4][26:13].
    const uint32_t dfmt = (dw[1] >> 20) & 0x3Fu;
    const uint32_t nfmt = (dw[1] >> 26) & 0xFu;
    const uint32_t imageType = (dw[3] >> 28) & 0xFu;
    if (imageType != 9u) return info; // Color2D only; arrays/cubes need other Metal types.

    MTLPixelFormat fmt = MTLPixelFormatInvalid;
    size_t bytesPerBlock = 0;
    const bool srgb = nfmt == 9u;
    switch (dfmt) {
        case 10: // Format8_8_8_8
            if (nfmt == 0u) fmt = MTLPixelFormatRGBA8Unorm;
            else if (srgb) fmt = MTLPixelFormatRGBA8Unorm_sRGB;
            break;
        case 35: // BC1
            bytesPerBlock = 8;
            if (nfmt == 0u) fmt = MTLPixelFormatBC1_RGBA;
            else if (srgb) fmt = MTLPixelFormatBC1_RGBA_sRGB;
            break;
        case 36: // BC2
            bytesPerBlock = 16;
            if (nfmt == 0u) fmt = MTLPixelFormatBC2_RGBA;
            else if (srgb) fmt = MTLPixelFormatBC2_RGBA_sRGB;
            break;
        case 37: // BC3
            bytesPerBlock = 16;
            if (nfmt == 0u) fmt = MTLPixelFormatBC3_RGBA;
            else if (srgb) fmt = MTLPixelFormatBC3_RGBA_sRGB;
            break;
        case 38: // BC4 is an 8-byte single-channel block, not BC1 RGBA.
            bytesPerBlock = 8;
            if (nfmt == 0u) fmt = MTLPixelFormatBC4_RUnorm;
            break;
        case 39: // BC5
            bytesPerBlock = 16;
            if (nfmt == 0u) fmt = MTLPixelFormatBC5_RGUnorm;
            break;
        case 41: // BC7
            bytesPerBlock = 16;
            if (nfmt == 0u) fmt = MTLPixelFormatBC7_RGBAUnorm;
            else if (srgb) fmt = MTLPixelFormatBC7_RGBAUnorm_sRGB;
            break;
        default:
            // Unknown, integer, depth, BC6, and unsupported formats must not
            // silently become BGRA8. Add mappings only with verified layout.
            return info;
    }
    if (fmt == MTLPixelFormatInvalid) return info;

    const uint32_t pitchPixels = ((dw[4] >> 13) & 0x3FFFu) + 1u;
    if (pitchPixels < tw || pitchPixels > 16384u) return info;

    // dw[0] plus dw[1][5:0] form the 38-bit base address in 256-byte units.
    const uint64_t cand_shifted = (uint64_t)dw[0] << 8;
    const uint64_t cand_shifted_full = cand_shifted | (((uint64_t)(dw[1] & 0x3Fu)) << 40);
    const uint64_t cand_direct = (uint64_t)dw[0];
    const uint64_t cand_64 = ((uint64_t)(dw[1] & 0x3Fu) << 32) | (uint64_t)dw[0];

    uint64_t tex_addr = 0;
    if (cand_shifted_full >= 0x10000 && cand_shifted_full < mem_size) {
        tex_addr = cand_shifted_full;
    } else if (cand_shifted >= 0x10000 && cand_shifted < mem_size) {
        // Compatibility with runtime-relocated descriptors where the guest
        // allocator has already folded the high GPU address bits.
        tex_addr = cand_shifted;
    } else if (cand_direct >= 0x10000 && cand_direct < mem_size) {
        tex_addr = cand_direct;
    } else if (cand_64 >= 0x10000 && cand_64 < mem_size) {
        tex_addr = cand_64;
    }
    if (tex_addr == 0) return info;

    size_t bytesPerRow;
    size_t totalBytes;
    if (bytesPerBlock) {
        const size_t blocksPerRow = (pitchPixels + 3u) / 4u;
        const size_t blockRows = (th + 3u) / 4u;
        bytesPerRow = blocksPerRow * bytesPerBlock;
        totalBytes = bytesPerRow * blockRows;
    } else {
        bytesPerRow = (size_t)pitchPixels * 4u;
        totalBytes = bytesPerRow * th;
    }

    info.addr = tex_addr;
    info.width = tw;
    info.height = th;
    info.dfmt = dfmt;
    info.nfmt = nfmt;
    info.imageType = imageType;
    info.pixelFormat = fmt;
    info.bytesPerRow = bytesPerRow;
    info.totalBytes = totalBytes;
    info.tileMode = (dw[3] >> 20) & 0x1Fu;
    info.pitchPixels = pitchPixels;
    info.bitsPerElement = bytesPerBlock ? (uint32_t)(bytesPerBlock * 8) : 32u;
    info.blockCompressed = bytesPerBlock != 0;
    info.valid = true;
    return info;
}

// Untiles the mip 0 of a tiled texture into linear rows. Returns false when the tile mode is not
// covered by ps4_tiling.h or the source is too small; the caller then uploads the bytes as stored.
static bool DetileForUpload(GuestContext *ctx, const DecodedTextureInfo &info, std::vector<uint8_t> &out) {
    if (!Ps4Tiling::IsSupported(info.tileMode, info.bitsPerElement)) return false;
    const uint32_t cols = info.blockCompressed ? (info.width + 3u) / 4u : info.width;
    const uint32_t rows = info.blockCompressed ? (info.height + 3u) / 4u : info.height;
    const uint32_t pitch = info.blockCompressed ? (info.pitchPixels + 3u) / 4u : info.pitchPixels;

    static bool s_self_test_logged = false;
    if (!s_self_test_logged) {
        s_self_test_logged = true;
        const bool ok = Ps4Tiling::SelfTest(info.tileMode, info.bitsPerElement, pitch, cols, rows);
        fprintf(stderr, "[ps4-gnm-metal] tiling self-test (mode %u, %u bits, %ux%u): %s\n", info.tileMode,
                info.bitsPerElement, cols, rows, ok ? "ok" : "FAILED");
    }

    out.assign(info.bytesPerRow * rows, 0);
    if (!Ps4Tiling::Detile(ctx->mem_base + info.addr, ctx->mem_size - info.addr, info.tileMode,
                           info.bitsPerElement, pitch, cols, rows, out.data(), info.bytesPerRow)) {
        out.clear();
        return false;
    }
    // GOW3_DUMP_TEXTURES=<dir> writes the first detiled RGBA8 and BC1 textures as raw rows for inspection.
    static int s_dumped = 0;
    const char *dump_dir = getenv("GOW3_DUMP_TEXTURES");
    const bool dumpable = info.dfmt == 10 || info.dfmt == 35;
    if (dump_dir && s_dumped < 24 && dumpable) {
        s_dumped++;
        char path[512];
        snprintf(path, sizeof(path), "%s/tex_%llx_%ux%u_p%u_t%u_f%u.rgba", dump_dir,
                 (unsigned long long)info.addr, info.width, info.height, info.pitchPixels, info.tileMode,
                 info.dfmt);
        if (FILE *f = fopen(path, "wb")) {
            fwrite(out.data(), 1, out.size(), f);
            fclose(f);
        }
    }
    return true;
}

static id<MTLTexture> ParseAndCacheTextureDescriptor(GuestContext *ctx, const uint32_t *dw, size_t num_words = 8) {
    if (!ctx || !ctx->mem_base) return nil;
    DecodedTextureInfo info = DecodeTextureDescriptorWords(dw, num_words, ctx->mem_size);
    if (!info.valid) return nil;

    auto it = g_state.textureCache.find(info.addr);
    if (it != g_state.textureCache.end()) {
        return it->second;
    }

    if (info.addr + info.totalBytes <= ctx->mem_size) {
        MTLTextureDescriptor *tDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:info.pixelFormat
                                                                                        width:info.width
                                                                                       height:info.height
                                                                                    mipmapped:NO];
        tDesc.usage = MTLTextureUsageShaderRead;
        tDesc.storageMode = MTLStorageModeShared;
        id<MTLTexture> mtlTex = [g_state.device newTextureWithDescriptor:tDesc];
        if (mtlTex) {
            MTLRegion r = MTLRegionMake2D(0, 0, info.width, info.height);
            std::vector<uint8_t> detiled;
            const bool wasDetiled = DetileForUpload(ctx, info, detiled);
            const uint8_t *bytes = wasDetiled ? detiled.data() : ctx->mem_base + info.addr;
            [mtlTex replaceRegion:r mipmapLevel:0 withBytes:bytes bytesPerRow:info.bytesPerRow];
            g_state.textureCache[info.addr] = mtlTex;
            static uint64_t s_tex_logged = 0;
            if (++s_tex_logged <= 15) {
                fprintf(stderr, "[ps4-gnm-metal] Texture cached: addr=0x%llx, %ux%u, fmt=0x%lx (dfmt=0x%x, bpr=%zu, total=%zu, tile=%u%s)\n",
                        (unsigned long long)info.addr, info.width, info.height,
                        (unsigned long)info.pixelFormat, info.dfmt, info.bytesPerRow, info.totalBytes,
                        info.tileMode, wasDetiled ? ", detiled" : "");
            }
            return mtlTex;
        }
    }
    return nil;
}

static id<MTLTexture> ResolveFragmentTexture(GuestContext *ctx) {
    if (!ctx || !ctx->mem_base) return nil;

    // 1. Direct T# descriptors in psUserData[0..15] (each 8 DWORDs = 32 bytes)
    // psUserData has 16 DWORDs. Any offset > 8 with an 8-DWORD descriptor would read past bounds.
    // We check offsets 0, 4, 8 and pass remaining words (16 - offset) to guarantee bounds safety.
    for (int offset = 0; offset <= 8; offset += 4) {
        id<MTLTexture> tex = ParseAndCacheTextureDescriptor(ctx, &g_state.psUserData[offset], 16 - offset);
        if (tex) return tex;
    }

    // 2. Check each 64-bit pair in psUserData and vsUserData as potential SRT pointers
    for (int stage = 0; stage < 2; stage++) {
        const uint32_t *userData = (stage == 0) ? g_state.psUserData : g_state.vsUserData;
        for (int pair = 0; pair < 8; pair++) {
            uint64_t srt_ptr = ((uint64_t)userData[pair * 2 + 1] << 32) | (uint64_t)userData[pair * 2];
            if (srt_ptr < 0x10000 || srt_ptr + 32 > ctx->mem_size) continue;

            const uint8_t *srt_bytes = ctx->mem_base + srt_ptr;
            // Scan SRT at 16-byte boundaries up to 512 bytes
            for (size_t off = 0; off <= 512; off += 16) {
                if (srt_ptr + off + 32 > ctx->mem_size) break;
                const uint32_t *dw = (const uint32_t *)(srt_bytes + off);
                id<MTLTexture> tex = ParseAndCacheTextureDescriptor(ctx, dw, 8);
                if (tex) return tex;
            }
        }
    }

    return nil;
}

extern "C" int ps4_gnm_test_texture_descriptor_decoding(void) {
    // BOOT0_00.GNF fields with the base address relocated to a guest address.
    // The original asset stores a zero base word and high base bits 0x08.
    const uint32_t gow3_intro_gnf[8] = {
        0x00bbe080, // relocated dw[0]: byte address 0xbbe08000
        0x02600000, // GCN DataFormat 38 (BC4), Unorm, relocated high base bits cleared
        0x010dc77f, // 1920x1080
        0x90d00324, // Color2D image type (9)
        0x00efe000, 0, 0, 0x000ff000
    };
    DecodedTextureInfo info = DecodeTextureDescriptorWords(gow3_intro_gnf, 8, 0x200000000ULL);
    if (!info.valid) return -1;
    if (info.width != 1920 || info.height != 1080) return -2;
    if (info.addr != 0xbbe08000ULL) return -3;
    if (info.dfmt != 38 || info.nfmt != 0 || info.imageType != 9) return -4;
    if (info.pixelFormat != MTLPixelFormatBC4_RUnorm) return -5;
    if (info.bytesPerRow != 3840 || info.totalBytes != 1036800) return -6;

    // Standard GCN DataFormat 37 (BC3), with the format in dw[1].
    const uint32_t bc3_tex[8] = {
        0x00bc6000, 0x02500000, 0x010dc77f, 0x90000000,
        0x00efe000, 0, 0, 0
    };
    DecodedTextureInfo info2 = DecodeTextureDescriptorWords(bc3_tex, 8, 0x200000000ULL);
    if (!info2.valid || info2.addr != 0xbc600000ULL || info2.dfmt != 37 ||
        info2.pixelFormat != MTLPixelFormatBC3_RGBA) return -7;

    // Invalid format 0 must not fall back to a four-channel uncompressed texture.
    uint32_t invalid_format[8];
    memcpy(invalid_format, gow3_intro_gnf, sizeof(invalid_format));
    invalid_format[1] &= ~(0x3Fu << 20);
    if (DecodeTextureDescriptorWords(invalid_format, 8, 0x200000000ULL).valid) return -8;

    // Pitch is word 4, and arrays need a different Metal texture type.
    if (DecodeTextureDescriptorWords(gow3_intro_gnf, 4, 0x200000000ULL).valid) return -9;
    uint32_t array_image[8];
    memcpy(array_image, gow3_intro_gnf, sizeof(array_image));
    array_image[3] = (array_image[3] & 0x0FFFFFFFu) | 0xD0000000u;
    if (DecodeTextureDescriptorWords(array_image, 8, 0x200000000ULL).valid) return -10;

    // Only dw[1][5:0] contributes high base-address bits.
    uint32_t high_address[8];
    memcpy(high_address, gow3_intro_gnf, sizeof(high_address));
    high_address[0] = 0;
    high_address[1] = (high_address[1] & ~0x3Fu) | 1u;
    DecodedTextureInfo info3 = DecodeTextureDescriptorWords(high_address, 8, 0x20000000000ULL);
    if (!info3.valid || info3.addr != 0x10000000000ULL) return -11;

    return 0;
}

// ---------------------------------------------------------------------------
// Translated shader path (GOW3_GCN=1): guest GCN stages compiled through the vendored
// shadPS4 recompiler, bound from their real V# descriptors. Any draw this path cannot
// handle falls back to the heuristic path below.
// ---------------------------------------------------------------------------

static bool g_draw_translated = false;
static id<MTLRenderPipelineState> g_draw_pso = nil;
static uint64_t g_gcn_translated_draws = 0;
static uint64_t g_gcn_fallback_draws = 0;
static std::unordered_map<uint64_t, id<MTLFunction>> g_gcn_functions;
static std::unordered_map<uint64_t, id<MTLRenderPipelineState>> g_gcn_pipelines;

// Vertex-input buffers use layouts kGcnVertexLayoutBase + location, clear of the MSL
// [[buffer(n)]] indices the translator assigns to storage and uniform buffers.
static constexpr NSUInteger kGcnVertexLayoutBase = 16;
static constexpr uint32_t kGcnMaxVertexInputs = 8;

// AmdGpu::DataFormat and AmdGpu::NumberFormat values (video_core/amdgpu/pixel_format.h).
enum : uint32_t {
    kGcnFormat8 = 1,
    kGcnFormat16 = 2,
    kGcnFormat8_8 = 3,
    kGcnFormat32 = 4,
    kGcnFormat16_16 = 5,
    kGcnFormat8_8_8_8 = 10,
    kGcnFormat32_32 = 11,
    kGcnFormat16_16_16_16 = 12,
    kGcnFormat32_32_32 = 13,
    kGcnFormat32_32_32_32 = 14,
};
enum : uint32_t {
    kGcnNumUnorm = 0,
    kGcnNumSnorm = 1,
    kGcnNumUscaled = 2,
    kGcnNumSscaled = 3,
    kGcnNumUint = 4,
    kGcnNumSint = 5,
    kGcnNumSnormNz = 6,
    kGcnNumFloat = 7,
};

// Metal vertex format for a V# data and number format. input_class is the type of the shader
// input (0 float, 1 uint, 2 int), which the translator declares from the SPIR-V reflection; the
// Metal format must match it.
static MTLVertexFormat MapGcnVertexFormat(uint32_t data_format, uint32_t num_format, uint32_t input_class) {
    const bool is_int = input_class != 0;
    const bool is_signed = input_class == 2 || num_format == kGcnNumSnorm ||
                           num_format == kGcnNumSnormNz || num_format == kGcnNumSscaled;
    const bool normalized = !is_int && (num_format == kGcnNumUnorm || num_format == kGcnNumSnorm ||
                                        num_format == kGcnNumSnormNz);
    const bool is_float_num = num_format == kGcnNumFloat;
    switch (data_format) {
    case kGcnFormat8:
    case kGcnFormat8_8:
    case kGcnFormat8_8_8_8: {
        if (is_float_num && !is_int) return MTLVertexFormatInvalid; // 8-bit floats do not exist
        const int n = data_format == kGcnFormat8 ? 1 : data_format == kGcnFormat8_8 ? 2 : 4;
        if (normalized) {
            if (is_signed) {
                return n == 1 ? MTLVertexFormatCharNormalized
                              : n == 2 ? MTLVertexFormatChar2Normalized
                                       : MTLVertexFormatChar4Normalized;
            }
            return n == 1 ? MTLVertexFormatUCharNormalized
                          : n == 2 ? MTLVertexFormatUChar2Normalized
                                   : MTLVertexFormatUChar4Normalized;
        }
        if (is_signed) {
            return n == 1 ? MTLVertexFormatChar : n == 2 ? MTLVertexFormatChar2 : MTLVertexFormatChar4;
        }
        return n == 1 ? MTLVertexFormatUChar : n == 2 ? MTLVertexFormatUChar2 : MTLVertexFormatUChar4;
    }
    case kGcnFormat16:
    case kGcnFormat16_16:
    case kGcnFormat16_16_16_16: {
        const int n = data_format == kGcnFormat16 ? 1 : data_format == kGcnFormat16_16 ? 2 : 4;
        if (is_float_num && !is_int) {
            return n == 1 ? MTLVertexFormatHalf : n == 2 ? MTLVertexFormatHalf2 : MTLVertexFormatHalf4;
        }
        if (normalized) {
            if (is_signed) {
                return n == 1 ? MTLVertexFormatShortNormalized
                              : n == 2 ? MTLVertexFormatShort2Normalized
                                       : MTLVertexFormatShort4Normalized;
            }
            return n == 1 ? MTLVertexFormatUShortNormalized
                          : n == 2 ? MTLVertexFormatUShort2Normalized
                                   : MTLVertexFormatUShort4Normalized;
        }
        if (is_signed) {
            return n == 1 ? MTLVertexFormatShort : n == 2 ? MTLVertexFormatShort2 : MTLVertexFormatShort4;
        }
        return n == 1 ? MTLVertexFormatUShort : n == 2 ? MTLVertexFormatUShort2 : MTLVertexFormatUShort4;
    }
    case kGcnFormat32:
    case kGcnFormat32_32:
    case kGcnFormat32_32_32:
    case kGcnFormat32_32_32_32: {
        const int n = data_format == kGcnFormat32 ? 1
                    : data_format == kGcnFormat32_32 ? 2
                    : data_format == kGcnFormat32_32_32 ? 3 : 4;
        if (input_class == 1) {
            return n == 1 ? MTLVertexFormatUInt : n == 2 ? MTLVertexFormatUInt2
                 : n == 3 ? MTLVertexFormatUInt3 : MTLVertexFormatUInt4;
        }
        if (input_class == 2) {
            return n == 1 ? MTLVertexFormatInt : n == 2 ? MTLVertexFormatInt2
                 : n == 3 ? MTLVertexFormatInt3 : MTLVertexFormatInt4;
        }
        return n == 1 ? MTLVertexFormatFloat : n == 2 ? MTLVertexFormatFloat2
             : n == 3 ? MTLVertexFormatFloat3 : MTLVertexFormatFloat4;
    }
    default:
        return MTLVertexFormatInvalid;
    }
}

// Counts each fallback reason and reports it at 1, 2, 4, 8, ... occurrences, so the reasons
// stay visible without flooding the log.
static bool GcnFallback(const char *stage, const std::string &why) {
    g_gcn_fallback_draws++;
    static std::unordered_map<std::string, uint64_t> s_reason_counts;
    const std::string key = std::string(stage) + ": " + why;
    const uint64_t count = ++s_reason_counts[key];
    if ((count & (count - 1)) == 0) {
        fprintf(stderr, "[ps4-gnm-metal] GCN fallback x%llu (%s)\n", (unsigned long long)count, key.c_str());
    }
    return false;
}

// Compiles a translated stage's MSL once and returns its main0 function (nil on failure).
static id<MTLFunction> GcnFunction(const GcnTranslate::Program &program) {
    const uint64_t key = GcnTranslate::Id(program);
    auto it = g_gcn_functions.find(key);
    if (it != g_gcn_functions.end()) return it->second;

    // GOW3_DUMP_MSL=<dir> writes each translated stage's MSL once, as msl_<id>.metal.
    if (const char *dir = getenv("GOW3_DUMP_MSL")) {
        char path[512];
        snprintf(path, sizeof(path), "%s/msl_%llu.metal", dir, (unsigned long long)key);
        if (FILE *f = fopen(path, "w")) {
            fputs(GcnTranslate::MslSource(program).c_str(), f);
            fclose(f);
        }
    }

    NSError *err = nil;
    id<MTLLibrary> lib = [g_state.device newLibraryWithSource:[NSString stringWithUTF8String:GcnTranslate::MslSource(program).c_str()]
                                                      options:nil
                                                        error:&err];
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"main0"] : nil;
    if (!fn) {
        fprintf(stderr, "[ps4-gnm-metal] GCN MSL did not compile: %s\n",
                err ? err.localizedDescription.UTF8String : "no main0");
    }
    g_gcn_functions[key] = fn;
    return fn;
}

// Fragment function for draws without a pixel shader: writes nothing.
static id<MTLFunction> GcnNullFragment() {
    static id<MTLFunction> s_function = nil;
    if (!s_function && g_state.device) {
        NSError *err = nil;
        id<MTLLibrary> lib = [g_state.device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nfragment void gcn_null_fragment() {}\n"
                                                          options:nil
                                                            error:&err];
        s_function = [lib newFunctionWithName:@"gcn_null_fragment"];
    }
    return s_function;
}

// T# destination select (dword 3, bits 0..11): one 3-bit select per channel, in RGBA order.
// Values 4..7 pick R, G, B, A; 0 and 1 are the constants zero and one.
static MTLTextureSwizzle GcnSwizzleFromSelect(uint32_t select) {
    switch (select & 7u) {
    case 0: return MTLTextureSwizzleZero;
    case 1: return MTLTextureSwizzleOne;
    case 4: return MTLTextureSwizzleRed;
    case 5: return MTLTextureSwizzleGreen;
    case 6: return MTLTextureSwizzleBlue;
    case 7: return MTLTextureSwizzleAlpha;
    default: return MTLTextureSwizzleZero;
    }
}

// The decoded texture viewed through the descriptor's destination select. Identity selects
// return the texture itself; other selects get one cached view per texture and select.
static id<MTLTexture> GcnSwizzledTexture(id<MTLTexture> tex, uint32_t dst_sel) {
    static std::unordered_map<uint64_t, id<MTLTexture>> s_views;
    const uint32_t select = dst_sel & 0xFFFu;
    const uint32_t identity = (4u) | (5u << 3) | (6u << 6) | (7u << 9);
    if (!tex || select == identity) return tex;
    const uint64_t key = ((uint64_t)(uintptr_t)(__bridge void *)tex << 12) ^ select;
    auto it = s_views.find(key);
    if (it != s_views.end()) return it->second;
    MTLTextureSwizzleChannels channels = MTLTextureSwizzleChannelsMake(
        GcnSwizzleFromSelect(select & 7u), GcnSwizzleFromSelect((select >> 3) & 7u),
        GcnSwizzleFromSelect((select >> 6) & 7u), GcnSwizzleFromSelect((select >> 9) & 7u));
    id<MTLTexture> view = [tex newTextureViewWithPixelFormat:tex.pixelFormat
                                                 textureType:tex.textureType
                                                      levels:NSMakeRange(0, tex.mipmapLevelCount)
                                                      slices:NSMakeRange(0, tex.arrayLength)
                                                     swizzle:channels];
    s_views[key] = view ? view : tex;
    return s_views[key];
}

static id<MTLTexture> GcnDefaultTexture() {
    static id<MTLTexture> s_texture = nil;
    if (!s_texture && g_state.device) {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                        width:1
                                                                                       height:1
                                                                                    mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        s_texture = [g_state.device newTextureWithDescriptor:desc];
        const uint8_t white[4] = {255, 255, 255, 255};
        [s_texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:white bytesPerRow:4];
    }
    return s_texture;
}

static id<MTLSamplerState> GcnDefaultSampler() {
    if (g_state.defaultSamplerState) return g_state.defaultSamplerState;
    static id<MTLSamplerState> s_sampler = nil;
    if (!s_sampler && g_state.device) {
        MTLSamplerDescriptor *desc = [[MTLSamplerDescriptor alloc] init];
        desc.minFilter = MTLSamplerMinMagFilterLinear;
        desc.magFilter = MTLSamplerMinMagFilterLinear;
        s_sampler = [g_state.device newSamplerStateWithDescriptor:desc];
    }
    return s_sampler;
}

// A buffer the translated stage reads, resolved from its descriptor for this draw.
struct GcnBufferBinding {
    uint32_t msl_index;
    id<MTLBuffer> buffer; // nil when the descriptor is empty
    size_t offset;
};

// Wraps a guest range into binding.buffer/offset. Ranges are clamped to guest memory. A zero
// length leaves the binding empty and succeeds.
static bool GcnWrapRange(GuestContext *ctx, uint64_t addr, uint64_t bytes, GcnBufferBinding &binding) {
    binding.buffer = nil;
    binding.offset = 0;
    if (bytes == 0) return true;
    if (addr < 0x10000 || addr >= ctx->mem_size) return false;
    const uint64_t avail = ctx->mem_size - addr;
    if (bytes > avail) bytes = avail;
    size_t offset = 0;
    id<MTLBuffer> buffer = WrapGuestMemory(ctx, addr, (size_t)bytes, &offset);
    binding.buffer = buffer;
    binding.offset = offset;
    return buffer != nil;
}

static bool TryBindTranslatedDraw(GuestContext *ctx) {
    g_draw_translated = false;
    g_draw_pso = nil;
    g_last_gcn_input_count = 0;
    if (!g_gcn_enabled || !ctx || !ctx->mem_base || !g_state.device) return false;
    GcnTranslate::SetGuestMemory(ctx->mem_base, ctx->mem_size);

    std::string why;
    auto vs = GcnTranslate::Translate(GcnTranslate::Stage::Vertex, g_state.vsGpuAddr,
                                      g_state.vsUserData, 0, GcnTranslate::FragmentState{}, &why);
    if (!vs) return GcnFallback("vertex", why);
    // Draws without a pixel shader (depth-only passes) bind the null fragment function.
    const bool has_fragment = g_state.psGpuAddr >= 0x10000 && g_state.psGpuAddr < ctx->mem_size;
    std::shared_ptr<GcnTranslate::Program> ps;
    if (has_fragment) {
        ps = GcnTranslate::Translate(GcnTranslate::Stage::Fragment, g_state.psGpuAddr,
                                     g_state.psUserData, GcnTranslate::ParamCount(*vs),
                                     g_state.fragmentState, &why);
        if (!ps) return GcnFallback("fragment", why);
    }

    // Vertex inputs: one layout per location, each bound to its V#'s guest range.
    const auto inputs = GcnTranslate::ResolveVertexInputs(*vs, g_state.vsUserData);
    g_last_gcn_input_count = (uint32_t)inputs.size();
    MTLVertexDescriptor *vertex_desc = [MTLVertexDescriptor vertexDescriptor];
    std::vector<std::pair<NSUInteger, GcnBufferBinding>> vertex_buffers;
    const bool probe_blend_override = g_gcn_probe_disable_blend &&
        g_gcn_probe_vs_addr != 0 && g_gcn_probe_ps_addr != 0 &&
        g_state.vsGpuAddr == g_gcn_probe_vs_addr && g_state.psGpuAddr == g_gcn_probe_ps_addr;
    const bool pipeline_blend_enable = g_state.blendEnable && !probe_blend_override;
    uint64_t pipeline_key = GcnTranslate::Id(*vs) * 0x9E3779B97F4A7C15ULL ^ (ps ? GcnTranslate::Id(*ps) : 0) ^
                            ((uint64_t)g_state.cbTargetMask << 56);
    for (const auto &input : inputs) {
        if (input.location >= kGcnMaxVertexInputs) return GcnFallback("vertex", "too many inputs");
        if (input.stride == 0 || input.base == 0 || input.num_records == 0) {
            return GcnFallback("vertex", "unbound vertex input");
        }
        const MTLVertexFormat format = MapGcnVertexFormat(input.data_format, input.num_format, input.input_class);
        if (format == MTLVertexFormatInvalid) {
            return GcnFallback("vertex", "unsupported vertex format");
        }
        const NSUInteger layout = kGcnVertexLayoutBase + input.location;
        vertex_desc.attributes[input.location].format = format;
        vertex_desc.attributes[input.location].offset = 0;
        vertex_desc.attributes[input.location].bufferIndex = layout;
        vertex_desc.layouts[layout].stride = input.stride;
        vertex_desc.layouts[layout].stepFunction =
            input.per_instance ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
        vertex_desc.layouts[layout].stepRate = 1;
        pipeline_key = pipeline_key * 1099511628211ULL ^ ((uint64_t)format << 32 | input.stride);

        GcnBufferBinding binding{};
        binding.msl_index = (uint32_t)layout;
        const uint64_t bytes = (uint64_t)input.stride * input.num_records;
        if (!GcnWrapRange(ctx, input.base, bytes, binding)) {
            return GcnFallback("vertex", "vertex buffer outside guest memory");
        }
        vertex_buffers.push_back({layout, binding});
    }
    // Blend state changes the Metal PSO and therefore belongs in the cache key.
    // Omitting it can reuse a pipeline created for a different guest blend mode.
    pipeline_key = pipeline_key * 1099511628211ULL ^ (uint64_t)pipeline_blend_enable;
    if (pipeline_blend_enable) {
        pipeline_key = pipeline_key * 1099511628211ULL ^
            ((uint64_t)g_state.srcRgbFactor << 24) ^ ((uint64_t)g_state.dstRgbFactor << 16) ^
            ((uint64_t)g_state.rgbOp << 8) ^ ((uint64_t)g_state.srcAlphaFactor << 48) ^
            ((uint64_t)g_state.dstAlphaFactor << 40) ^ ((uint64_t)g_state.alphaOp << 32);
    }

    // Resources the stages read through buffer, texture and sampler slots.
    const auto vs_buffer_resources = GcnTranslate::ResolveBuffers(*vs, g_state.vsUserData);
    const auto ps_buffer_resources = ps
        ? GcnTranslate::ResolveBuffers(*ps, g_state.psUserData)
        : std::vector<GcnTranslate::Resource>{};
    auto resolve = [&](const std::vector<GcnTranslate::Resource> &resources,
                       std::vector<GcnBufferBinding> &out) -> bool {
        for (const auto &res : resources) {
            GcnBufferBinding binding{};
            binding.msl_index = res.msl_index;
            if (!GcnWrapRange(ctx, res.guest_addr, res.guest_bytes, binding)) {
                return false;
            }
            out.push_back(binding);
        }
        return true;
    };
    std::vector<GcnBufferBinding> vs_buffers, ps_buffers;
    if (!resolve(vs_buffer_resources, vs_buffers)) return GcnFallback("vertex", "buffer outside guest memory");
    if (ps && !resolve(ps_buffer_resources, ps_buffers)) return GcnFallback("fragment", "buffer outside guest memory");
    // Push constants carry the user-data words the translated code reads directly.
    const std::vector<uint8_t> vs_push = GcnTranslate::ResolvePushData(*vs, g_state.vsUserData);
    const std::vector<uint8_t> ps_push = ps ? GcnTranslate::ResolvePushData(*ps, g_state.psUserData) : std::vector<uint8_t>{};
    const int32_t vs_push_index = GcnTranslate::PushIndex(*vs);
    const int32_t ps_push_index = ps ? GcnTranslate::PushIndex(*ps) : -1;
    const auto vs_images = GcnTranslate::ResolveImages(*vs, g_state.vsUserData);
    const auto ps_images = ps ? GcnTranslate::ResolveImages(*ps, g_state.psUserData)
                              : std::vector<GcnTranslate::ImageBinding>{};
    // The flattened user-data buffer: constant reads from user-data pointers, filled per draw.
    const auto vs_inline = GcnTranslate::ResolveInlineBuffers(*vs, g_state.vsUserData);
    const auto ps_inline = ps ? GcnTranslate::ResolveInlineBuffers(*ps, g_state.psUserData)
                              : std::vector<GcnTranslate::InlineBuffer>{};

    // Dump one selected draw's actual translated shader source and resolved guest bindings.
    // The encoded count is incremented immediately after the Metal draw call.
    static bool s_probe_dump_emitted = false;
    const uint64_t upcoming_draw = g_draws_encoded_count + 1;
    const bool dump_by_draw = g_gcn_probe_dump_draw != 0 && upcoming_draw == g_gcn_probe_dump_draw;
    const bool dump_by_shader = g_gcn_probe_vs_addr != 0 && g_gcn_probe_ps_addr != 0 &&
                                upcoming_draw >= g_gcn_probe_start_draw &&
                                g_state.vsGpuAddr == g_gcn_probe_vs_addr &&
                                g_state.psGpuAddr == g_gcn_probe_ps_addr;
    const bool log_probe_draw = g_gcn_debug && (dump_by_draw || dump_by_shader) && !s_probe_dump_emitted;
    if (log_probe_draw) {
        s_probe_dump_emitted = true;
        fprintf(stderr,
                "[gcn-probe-draw] encoded=%llu VS=0x%llx PS=0x%llx vs_id=%llu ps_id=%llu "
                "translated=1 primitive=%u target0=0x%llx target_mask=0x%x "
                "viewport=(%.3f,%.3f %.3fx%.3f) scissor=(%lu,%lu %lux%lu) "
                "blend=%u probe_blend_override=%u rgb=(%u,%u,%u) alpha=(%u,%u,%u) "
                "inputs=%zu vs_buffers=%zu ps_buffers=%zu vs_images=%zu ps_images=%zu\n",
                (unsigned long long)upcoming_draw,
                (unsigned long long)g_state.vsGpuAddr, (unsigned long long)g_state.psGpuAddr,
                (unsigned long long)GcnTranslate::Id(*vs),
                (unsigned long long)(ps ? GcnTranslate::Id(*ps) : 0),
                (unsigned)g_state.guestPrimitiveType,
                (unsigned long long)g_state.colorBufferBase[0], g_state.cbTargetMask,
                g_state.viewport.originX, g_state.viewport.originY,
                g_state.viewport.width, g_state.viewport.height,
                (unsigned long)g_state.scissor.x, (unsigned long)g_state.scissor.y,
                (unsigned long)g_state.scissor.width, (unsigned long)g_state.scissor.height,
                g_state.blendEnable ? 1u : 0u,
                probe_blend_override ? 1u : 0u,
                (unsigned)g_state.srcRgbFactor, (unsigned)g_state.dstRgbFactor, (unsigned)g_state.rgbOp,
                (unsigned)g_state.srcAlphaFactor, (unsigned)g_state.dstAlphaFactor, (unsigned)g_state.alphaOp,
                inputs.size(), vs_buffers.size(), ps_buffers.size(), vs_images.size(), ps_images.size());
        for (const auto &input : inputs) {
            fprintf(stderr,
                    "[gcn-probe-draw] vertex_input loc=%u base=0x%llx stride=%u records=%u "
                    "dfmt=%u nfmt=%u class=%u per_instance=%u\n",
                    input.location, (unsigned long long)input.base, input.stride, input.num_records,
                    input.data_format, input.num_format, input.input_class,
                    input.per_instance ? 1u : 0u);
            if (ctx->mem_base && input.base < ctx->mem_size) {
                const uint64_t requested = (uint64_t)input.stride * input.num_records;
                const size_t readable = (size_t)std::min<uint64_t>(
                    std::min<uint64_t>(requested, 48), ctx->mem_size - input.base);
                fprintf(stderr, "[gcn-probe-draw] vertex_input_words loc=%u bytes=%zu data=",
                        input.location, readable);
                for (size_t offset = 0; offset + sizeof(uint32_t) <= readable; offset += sizeof(uint32_t)) {
                    uint32_t word = 0;
                    float value = 0.0f;
                    std::memcpy(&word, ctx->mem_base + input.base + offset, sizeof(word));
                    std::memcpy(&value, &word, sizeof(value));
                    fprintf(stderr, "%s%08x(%.7g)", offset ? "," : "", word, (double)value);
                }
                fputc('\n', stderr);
            }
        }
        fprintf(stderr, "[gcn-probe-draw] pixel_user_data=");
        for (size_t i = 0; i < 16; ++i) {
            fprintf(stderr, "%s%08x", i ? "," : "", g_state.psUserData[i]);
        }
        fputc('\n', stderr);
        for (size_t i = 0; i < ps_buffer_resources.size() && i < ps_buffers.size(); ++i) {
            const auto &resource = ps_buffer_resources[i];
            const auto &binding = ps_buffers[i];
            fprintf(stderr,
                    "[gcn-probe-draw] pixel_buffer msl=%u guest_addr=0x%llx guest_bytes=%llu "
                    "host_length=%lu host_offset=%zu words=",
                    resource.msl_index, (unsigned long long)resource.guest_addr,
                    (unsigned long long)resource.guest_bytes,
                    binding.buffer ? (unsigned long)binding.buffer.length : 0ul, binding.offset);
            const size_t safe_bytes = resource.guest_addr < ctx->mem_size
                ? (size_t)std::min<uint64_t>(16, ctx->mem_size - resource.guest_addr)
                : 0;
            for (size_t offset = 0; offset + sizeof(uint32_t) <= safe_bytes; offset += sizeof(uint32_t)) {
                uint32_t word = 0;
                float value = 0.0f;
                std::memcpy(&word, ctx->mem_base + resource.guest_addr + offset, sizeof(word));
                std::memcpy(&value, &word, sizeof(value));
                fprintf(stderr, "%s%08x(%.7g)%s", offset ? "," : "", word,
                        (double)value, offset + sizeof(uint32_t) > resource.guest_bytes ? "*" : "");
            }
            fputc('\n', stderr);
        }
        for (const auto &image : ps_images) {
            const DecodedTextureInfo decoded = DecodeTextureDescriptorWords(
                image.words.data(), image.words.size(), ctx->mem_size);
            fprintf(stderr,
                    "[gcn-probe-draw] pixel_image slot=%u decode_valid=%u addr=0x%llx "
                    "size=%ux%u type=%u dfmt=%u nfmt=%u tile=%u pitch=%u bytes=%zu words=",
                    image.msl_index, decoded.valid ? 1u : 0u,
                    (unsigned long long)decoded.addr, decoded.width, decoded.height,
                    decoded.imageType, decoded.dfmt, decoded.nfmt, decoded.tileMode,
                    decoded.pitchPixels, decoded.totalBytes);
            for (uint32_t word : image.words) fprintf(stderr, "%08x ", word);
            fputc('\n', stderr);
            for (size_t dword = 0; dword < image.words.size(); ++dword) {
                const bool loaded = (image.load_mask & (1u << dword)) != 0;
                fprintf(stderr,
                        "[gcn-probe-draw] pixel_descriptor_word info=%u msl=%u dw=%zu source=%s "
                        "offset=%u value=%08x resolved=%08x\n",
                        image.info_index, image.msl_index, dword,
                        loaded ? "flatbuf" : "immediate",
                        loaded ? (unsigned)image.fetch_offsets[dword] : 0u,
                        image.fetch_values[dword], image.words[dword]);
            }
        }
        fprintf(stderr, "[gcn-probe-vs-msl-begin]\n%s\n[gcn-probe-vs-msl-end]\n",
                GcnTranslate::MslSource(*vs).c_str());
        if (ps) {
            fprintf(stderr, "[gcn-probe-ps-msl-begin]\n%s\n[gcn-probe-ps-msl-end]\n",
                    GcnTranslate::MslSource(*ps).c_str());
        }
    }
    for (const auto &inline_buffer : vs_inline) {
        if (inline_buffer.bytes.size() > 4096) return GcnFallback("vertex", "flattened buffer too large");
    }
    for (const auto &inline_buffer : ps_inline) {
        if (inline_buffer.bytes.size() > 4096) return GcnFallback("fragment", "flattened buffer too large");
    }

    // Pipeline (created once per program set, input formats and blend state).
    id<MTLRenderPipelineState> pso = nil;
    auto pso_it = g_gcn_pipelines.find(pipeline_key);
    if (pso_it != g_gcn_pipelines.end()) {
        pso = pso_it->second;
    } else {
        id<MTLFunction> vfn = GcnFunction(*vs);
        id<MTLFunction> pfn = ps ? GcnFunction(*ps) : GcnNullFragment();
        if (!vfn || !pfn) return GcnFallback("pipeline", "MSL did not compile");
        MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
        desc.vertexFunction = vfn;
        desc.fragmentFunction = pfn;
        desc.vertexDescriptor = vertex_desc;
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        desc.colorAttachments[0].writeMask = TranslateColorWriteMask(g_state.cbTargetMask);
        desc.colorAttachments[0].blendingEnabled = pipeline_blend_enable;
        if (pipeline_blend_enable) {
            desc.colorAttachments[0].sourceRGBBlendFactor = g_state.srcRgbFactor;
            desc.colorAttachments[0].destinationRGBBlendFactor = g_state.dstRgbFactor;
            desc.colorAttachments[0].rgbBlendOperation = g_state.rgbOp;
            desc.colorAttachments[0].sourceAlphaBlendFactor = g_state.srcAlphaFactor;
            desc.colorAttachments[0].destinationAlphaBlendFactor = g_state.dstAlphaFactor;
            desc.colorAttachments[0].alphaBlendOperation = g_state.alphaOp;
        }
        NSError *err = nil;
        pso = [g_state.device newRenderPipelineStateWithDescriptor:desc error:&err];
        if (!pso) {
            if (g_gcn_debug) {
                fprintf(stderr, "[gcn-debug] pipeline failed; %s\n", GcnTranslate::DebugInputStruct(*vs).c_str());
                for (const auto &input : inputs) {
                    fprintf(stderr, "[gcn-debug]   input loc=%u dfmt=%u nfmt=%u class=%u\n", input.location,
                            input.data_format, input.num_format, input.input_class);
                }
            }
            return GcnFallback("pipeline", err ? err.localizedDescription.UTF8String : "unknown error");
        }
        g_gcn_pipelines[pipeline_key] = pso;
        g_pipelines_created_count++;
    }

    // Everything is valid: encode the bindings.
    id<MTLRenderCommandEncoder> enc = g_state.currentRenderEncoder;
    for (const auto &[layout, binding] : vertex_buffers) {
        [enc setVertexBuffer:binding.buffer offset:binding.offset atIndex:layout];
    }
    for (const auto &binding : vs_buffers) {
        [enc setVertexBuffer:binding.buffer offset:binding.offset atIndex:binding.msl_index];
    }
    for (const auto &binding : ps_buffers) {
        [enc setFragmentBuffer:binding.buffer offset:binding.offset atIndex:binding.msl_index];
    }
    if (vs_push_index >= 0 && !vs_push.empty()) {
        [enc setVertexBytes:vs_push.data() length:vs_push.size() atIndex:(NSUInteger)vs_push_index];
    }
    if (ps_push_index >= 0 && !ps_push.empty()) {
        [enc setFragmentBytes:ps_push.data() length:ps_push.size() atIndex:(NSUInteger)ps_push_index];
    }
    for (const auto &inline_buffer : vs_inline) {
        [enc setVertexBytes:inline_buffer.bytes.data() length:inline_buffer.bytes.size()
                    atIndex:(NSUInteger)inline_buffer.msl_index];
    }
    for (const auto &inline_buffer : ps_inline) {
        [enc setFragmentBytes:inline_buffer.bytes.data() length:inline_buffer.bytes.size()
                      atIndex:(NSUInteger)inline_buffer.msl_index];
    }
    // Textures: each T# goes through the heuristic path's decoder. A descriptor it cannot
    // decode binds the default white texture.
    for (const auto &image : vs_images) {
        id<MTLTexture> tex = ParseAndCacheTextureDescriptor(ctx, image.words.data(), image.words.size());
        tex = GcnSwizzledTexture(tex, image.words[3]);
        [enc setVertexTexture:(tex ? tex : GcnDefaultTexture()) atIndex:(NSUInteger)image.msl_index];
    }
    // GOW3_GCN_DEBUG: one line per translated draw (first 200), with its vertex formats and
    // fragment textures, to find the draws that paint a given color.
    static uint32_t s_draw_logs_left = 200;
    const bool log_draw = g_gcn_debug && s_draw_logs_left > 0;
    if (log_draw) {
        s_draw_logs_left--;
        fprintf(stderr, "[gcn-debug] draw VS=0x%llx PS=0x%llx vs_id=%llu ps_id=%llu inputs:",
                (unsigned long long)g_state.vsGpuAddr, (unsigned long long)g_state.psGpuAddr,
                (unsigned long long)GcnTranslate::Id(*vs), (unsigned long long)(ps ? GcnTranslate::Id(*ps) : 0));
        for (const auto &input : inputs) {
            fprintf(stderr, " loc%u(dfmt%u,nfmt%u,cls%u)", input.location, input.data_format,
                    input.num_format, input.input_class);
        }
        fprintf(stderr, " ps_user:");
        for (int i = 0; i < 4; i++) fprintf(stderr, " %08x", g_state.psUserData[i]);
        for (const auto &buffer : ps_buffers) {
            fprintf(stderr, " ps_buf[slot%u off=%zu]:", buffer.msl_index, buffer.offset);
            if (buffer.buffer) {
                const uint8_t *data = (const uint8_t *)[buffer.buffer contents] + buffer.offset;
                for (int i = 0; i < 4; i++) {
                    uint32_t word;
                    memcpy(&word, data + 4 * i, 4);
                    fprintf(stderr, " %08x(%g)", word, (double)*(const float *)&word);
                }
            }
        }
        for (const auto &inline_buffer : ps_inline) {
            fprintf(stderr, " ps_inline[slot%u]:", inline_buffer.msl_index);
            for (size_t i = 0; i + 4 <= inline_buffer.bytes.size() && i < 16; i += 4) {
                uint32_t word;
                memcpy(&word, inline_buffer.bytes.data() + i, 4);
                fprintf(stderr, " %08x(%g)", word, (double)*(const float *)&word);
            }
        }
        fprintf(stderr, " ps_tex:");
    }
    for (const auto &image : ps_images) {
        id<MTLTexture> tex = ParseAndCacheTextureDescriptor(ctx, image.words.data(), image.words.size());
        tex = GcnSwizzledTexture(tex, image.words[3]);
        if (log_probe_draw) {
            const DecodedTextureInfo decoded = DecodeTextureDescriptorWords(
                image.words.data(), image.words.size(), ctx->mem_size);
            id<MTLTexture> actual = tex ? tex : GcnDefaultTexture();
            fprintf(stderr,
                    "[gcn-probe-draw] pixel_binding msl=%u guest_addr=0x%llx descriptor_type=%u "
                    "decode_valid=%u source=%s host_type=%lu host_size=%lux%lu host_format=%lu\n",
                    image.msl_index,
                    (unsigned long long)decoded.addr,
                    (unsigned)((image.words[3] >> 28) & 0xFu), decoded.valid ? 1u : 0u,
                    tex ? "guest-texture" : "fallback-white",
                    actual ? (unsigned long)actual.textureType : 0ul,
                    actual ? (unsigned long)actual.width : 0ul,
                    actual ? (unsigned long)actual.height : 0ul,
                    actual ? (unsigned long)actual.pixelFormat : 0ul);
        }
        if (log_draw) {
            fprintf(stderr, " [slot%u dfmt%u dst%03x %s]", image.msl_index, (image.words[1] >> 20) & 0x3Fu,
                    image.words[3] & 0xFFFu, tex ? "bound" : "default-white");
        }
        [enc setFragmentTexture:(tex ? tex : GcnDefaultTexture()) atIndex:(NSUInteger)image.msl_index];
    }
    if (log_draw) fprintf(stderr, "\n");
    for (const auto &smp : GcnTranslate::DeclaredSamplers(*vs)) {
        [enc setVertexSamplerState:GcnDefaultSampler() atIndex:smp.msl_index];
    }
    if (ps) for (const auto &smp : GcnTranslate::DeclaredSamplers(*ps)) {
        [enc setFragmentSamplerState:GcnDefaultSampler() atIndex:smp.msl_index];
    }

    g_draw_translated = true;
    g_draw_pso = pso;
    g_gcn_translated_draws++;
    static uint32_t s_logs_left = 20;
    if (s_logs_left > 0) {
        s_logs_left--;
        fprintf(stderr, "[ps4-gnm-metal] GCN draw translated: VS=0x%llx PS=0x%llx inputs=%zu vs_buffers=%zu ps_buffers=%zu\n",
                (unsigned long long)g_state.vsGpuAddr, (unsigned long long)g_state.psGpuAddr,
                inputs.size(), vs_buffers.size(), ps_buffers.size());
        static uint32_t s_dump_left = 3;
        if (g_gcn_debug && s_dump_left > 0 && vs_buffers.size() > 0) {
            s_dump_left--;
            GcnTranslate::DebugDump(*vs, g_state.vsUserData);
            fprintf(stderr, "[gcn-debug]   vs user data:");
            for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", g_state.vsUserData[i]);
            fprintf(stderr, "\n");
        }
        // Diagnostic: vertex-stage buffers (the constant buffer holds the transform) and inputs.
        for (const auto &in : inputs) {
            if (!g_gcn_debug) break;
            fprintf(stderr, "[ps4-gnm-metal]   input loc=%u dfmt=%u nfmt=%u class=%u stride=%u base=0x%llx records=%u\n",
                    in.location, in.data_format, in.num_format, in.input_class, in.stride,
                    (unsigned long long)in.base, in.num_records);
        }
        for (const auto &b : vs_buffers) {
            if (!g_gcn_debug || !b.buffer) continue;
            const float *f = (const float *)((const uint8_t *)[b.buffer contents] + b.offset);
            fprintf(stderr, "[ps4-gnm-metal]   vs buffer msl=%u len=%lu first16=", b.msl_index, (unsigned long)b.buffer.length);
            for (int i = 0; i < 16; i++) fprintf(stderr, " %g", f[i]);
            fprintf(stderr, "\n");
        }
    }
    return true;
}

static void BindVertexAndTexture(GuestContext *ctx) {
    if (!ctx || !g_state.currentRenderEncoder) return;
    if (TryBindTranslatedDraw(ctx)) return;

    size_t vbOffset = 0;
    uint32_t stride = 16;
    uint32_t position_components = 0;
    id<MTLBuffer> vbBuf = ResolveVertexBuffer(ctx, &vbOffset, &stride,
                                               &position_components);
    if (vbBuf) {
        [g_state.currentRenderEncoder setVertexBuffer:vbBuf offset:vbOffset atIndex:0];
        const uint32_t layout[2] = {stride, position_components};
        [g_state.currentRenderEncoder setVertexBytes:layout length:sizeof(layout) atIndex:1];
        g_vertex_buffers_bound_count++;
    } else {
        [g_state.currentRenderEncoder setVertexBuffer:nil offset:0 atIndex:0];
        const uint32_t layout[2] = {0, 0};
        [g_state.currentRenderEncoder setVertexBytes:layout length:sizeof(layout) atIndex:1];
    }

    size_t cbOffset = 0;
    id<MTLBuffer> cbBuf = ResolveConstantBuffer(ctx, &cbOffset);
    if (cbBuf) {
        [g_state.currentRenderEncoder setVertexBuffer:cbBuf offset:cbOffset atIndex:2];
    } else {
        [g_state.currentRenderEncoder setVertexBuffer:nil offset:0 atIndex:2];
    }

    id<MTLTexture> tex = ResolveFragmentTexture(ctx);
    if (tex) {
        [g_state.currentRenderEncoder setFragmentTexture:tex atIndex:0];
        g_textures_resolved_count++;
    } else {
        [g_state.currentRenderEncoder setFragmentTexture:nil atIndex:0];
    }
}

static void EnsureRenderEncoder(GuestContext *ctx) {
    (void)ctx;
    if (g_state.currentRenderEncoder) return;

    if (g_state.currentComputeEncoder) {
        [g_state.currentComputeEncoder endEncoding];
        g_state.currentComputeEncoder = nil;
    }

    if (!g_state.currentCmdBuffer) {
        g_state.currentCmdBuffer = [g_state.cmdQueue commandBuffer];
    }

    id<MTLTexture> screenTex = (__bridge id<MTLTexture>)ps4_metal_screen_get_texture();
    if (!screenTex) return;

    MTLRenderPassDescriptor *passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
    passDesc.colorAttachments[0].texture = screenTex;
    passDesc.colorAttachments[0].loadAction = g_state.needsClear ? MTLLoadActionClear : MTLLoadActionLoad;
    passDesc.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
    g_state.needsClear = false;

    g_state.currentRenderEncoder = [g_state.currentCmdBuffer renderCommandEncoderWithDescriptor:passDesc];
    if (g_state.currentRenderEncoder) {
        [g_state.currentRenderEncoder setViewport:g_state.viewport];
        [g_state.currentRenderEncoder setScissorRect:g_state.scissor];
        [g_state.currentRenderEncoder setRenderPipelineState:g_state.defaultRenderPipeline];
        if (g_state.defaultDepthStencilState) {
            [g_state.currentRenderEncoder setDepthStencilState:g_state.defaultDepthStencilState];
        }
        if (g_state.defaultSamplerState) {
            [g_state.currentRenderEncoder setFragmentSamplerState:g_state.defaultSamplerState atIndex:0];
        }
    }
}

// End and submit the current render pass, then read a few diagnostic pixels
// from the shared screen texture.  The next draw lazily reopens an encoder with
// loadAction=Load, so this preserves ordering and framebuffer contents.
static void ProbeDrawPixels(void) {
    if (!g_gcn_debug || g_gcn_probe_limit == 0 ||
        g_draws_encoded_count < g_gcn_probe_start_draw ||
        g_gcn_probe_count >= g_gcn_probe_limit || !g_state.currentRenderEncoder ||
        (g_gcn_probe_vs_addr != 0 && g_state.vsGpuAddr != g_gcn_probe_vs_addr) ||
        (g_gcn_probe_ps_addr != 0 && g_state.psGpuAddr != g_gcn_probe_ps_addr)) {
        return;
    }

    id<MTLTexture> screenTex = (__bridge id<MTLTexture>)ps4_metal_screen_get_texture();
    if (!screenTex || screenTex.storageMode != MTLStorageModeShared) return;

    const uint32_t probe = ++g_gcn_probe_count;
    [g_state.currentRenderEncoder endEncoding];
    g_state.currentRenderEncoder = nil;
    id<MTLCommandBuffer> submitted = g_state.currentCmdBuffer;
    if (submitted) {
        [submitted commit];
        [submitted waitUntilCompleted];
        g_state.currentCmdBuffer = nil;
    }

    const uint64_t encoded_draw = g_draws_encoded_count;
    const bool save_full_target = !g_gcn_probe_capture_dir.empty() &&
        std::find(g_gcn_probe_capture_draws.begin(), g_gcn_probe_capture_draws.end(), encoded_draw) !=
            g_gcn_probe_capture_draws.end();
    if (save_full_target) {
        const size_t row_bytes = (size_t)screenTex.width * 4;
        const size_t byte_count = row_bytes * (size_t)screenTex.height;
        std::vector<uint8_t> bgra(byte_count);
        const MTLRegion full_region = MTLRegionMake2D(0, 0, screenTex.width, screenTex.height);
        [screenTex getBytes:bgra.data() bytesPerRow:row_bytes fromRegion:full_region mipmapLevel:0];

        char raw_path[4096];
        char json_path[4096];
        snprintf(raw_path, sizeof(raw_path), "%s/rt_after_draw_%06llu.bgra",
                 g_gcn_probe_capture_dir.c_str(), (unsigned long long)encoded_draw);
        snprintf(json_path, sizeof(json_path), "%s/rt_after_draw_%06llu.json",
                 g_gcn_probe_capture_dir.c_str(), (unsigned long long)encoded_draw);
        std::ofstream raw(raw_path, std::ios::binary | std::ios::trunc);
        if (raw) {
            raw.write((const char *)bgra.data(), (std::streamsize)bgra.size());
            raw.flush();
        }
        raw.close();
        const bool raw_ok = raw.good();

        std::ofstream meta(json_path, std::ios::trunc);
        if (meta) {
            meta << "{\n"
                 << "  \"encoded_draw\": " << encoded_draw << ",\n"
                 << "  \"capture_point\": \"after_draw_before_next_draw\",\n"
                 << "  \"source\": \"Metal screen render target\",\n"
                 << "  \"guest_target0\": \"0x" << std::hex << g_state.colorBufferBase[0] << std::dec << "\",\n"
                 << "  \"vertex_shader\": \"0x" << std::hex << g_state.vsGpuAddr << std::dec << "\",\n"
                 << "  \"pixel_shader\": \"0x" << std::hex << g_state.psGpuAddr << std::dec << "\",\n"
                 << "  \"width\": " << screenTex.width << ",\n"
                 << "  \"height\": " << screenTex.height << ",\n"
                 << "  \"texture_type\": " << (unsigned long)screenTex.textureType << ",\n"
                 << "  \"storage_mode\": " << (unsigned long)screenTex.storageMode << ",\n"
                 << "  \"pixel_format\": " << (unsigned long)screenTex.pixelFormat << ",\n"
                 << "  \"byte_order\": \"BGRA8\",\n"
                 << "  \"bytes_per_row\": " << row_bytes << ",\n"
                 << "  \"byte_count\": " << byte_count << ",\n"
                 << "  \"raw_saved\": " << (raw_ok ? "true" : "false") << "\n"
                 << "}\n";
        }
        fprintf(stderr,
                "[gcn-probe-capture] encoded=%llu point=after_draw_before_next_draw source=Metal_screen_RT "
                "guest_target0=0x%llx shader=(0x%llx,0x%llx) size=%lux%lu format=%lu "
                "row_bytes=%zu byte_count=%zu file=%s saved=%u\n",
                (unsigned long long)encoded_draw,
                (unsigned long long)g_state.colorBufferBase[0],
                (unsigned long long)g_state.vsGpuAddr,
                (unsigned long long)g_state.psGpuAddr,
                (unsigned long)screenTex.width, (unsigned long)screenTex.height,
                (unsigned long)screenTex.pixelFormat, row_bytes, byte_count, raw_path,
                raw_ok ? 1u : 0u);
    }

    static const uint32_t sample_xy[][2] = {
        {20, 20}, {160, 135}, {319, 269}, {400, 300}, {960, 540}, {1500, 800}
    };
    uint8_t bgra[sizeof(sample_xy) / sizeof(sample_xy[0])][4] = {};
    for (size_t i = 0; i < sizeof(sample_xy) / sizeof(sample_xy[0]); ++i) {
        const uint32_t x = sample_xy[i][0];
        const uint32_t y = sample_xy[i][1];
        if (x >= screenTex.width || y >= screenTex.height) continue;
        const MTLRegion region = MTLRegionMake2D(x, y, 1, 1);
        [screenTex getBytes:bgra[i] bytesPerRow:4 fromRegion:region mipmapLevel:0];
    }
    fprintf(stderr,
            "[gcn-probe] probe=%u encoded=%llu gcn_translated=%d translated_total=%llu VS=0x%llx PS=0x%llx "
            "inputs=%u target0=0x%llx blend=%u rgb=(%u,%u,%u) "
            "rgba@20,20=%u,%u,%u,%u rgba@160,135=%u,%u,%u,%u "
            "rgba@319,269=%u,%u,%u,%u rgba@400,300=%u,%u,%u,%u "
            "rgba@960,540=%u,%u,%u,%u rgba@1500,800=%u,%u,%u,%u\n",
            probe, (unsigned long long)g_draws_encoded_count,
            g_draw_translated ? 1 : 0,
            (unsigned long long)g_gcn_translated_draws,
            (unsigned long long)g_state.vsGpuAddr, (unsigned long long)g_state.psGpuAddr,
            g_last_gcn_input_count, (unsigned long long)g_state.colorBufferBase[0],
            g_state.blendEnable ? 1u : 0u, (unsigned)g_state.srcRgbFactor,
            (unsigned)g_state.dstRgbFactor, (unsigned)g_state.rgbOp,
            bgra[0][2], bgra[0][1], bgra[0][0], bgra[0][3],
            bgra[1][2], bgra[1][1], bgra[1][0], bgra[1][3],
            bgra[2][2], bgra[2][1], bgra[2][0], bgra[2][3],
            bgra[3][2], bgra[3][1], bgra[3][0], bgra[3][3],
            bgra[4][2], bgra[4][1], bgra[4][0], bgra[4][3],
            bgra[5][2], bgra[5][1], bgra[5][0], bgra[5][3]);
}

static id<MTLRenderPipelineState> GetOrCreateRenderPipeline(GuestContext *ctx) {
    if (g_draw_translated) return g_draw_pso;

    uint64_t key = g_state.vsGpuAddr ^ (g_state.psGpuAddr << 24) ^
                  ((uint64_t)g_state.blendEnable << 48) ^
                  ((uint64_t)g_state.primitiveType << 52) ^
                  ((uint64_t)g_state.cbTargetMask << 56);

    auto it = g_state.renderPipelineCache.find(key);
    if (it != g_state.renderPipelineCache.end()) {
        return it->second;
    }

    // 1. Attempt SPIRV-Cross translation if SPIR-V binary is present at shader address
    if (ctx && ctx->mem_base && g_state.vsGpuAddr >= 0x10000 && g_state.vsGpuAddr < ctx->mem_size) {
        const uint32_t *magic = (const uint32_t *)(ctx->mem_base + g_state.vsGpuAddr);
        if (*magic == 0x07230203) { // SPIR-V Magic Number
            try {
                spirv_cross::CompilerMSL msl(magic, 512);
                spirv_cross::CompilerMSL::Options options;
                options.set_msl_version(2, 3);
                msl.set_msl_options(options);
                std::string mslSource = msl.compile();

                NSError *err = nil;
                id<MTLLibrary> lib = [g_state.device newLibraryWithSource:[NSString stringWithUTF8String:mslSource.c_str()]
                                                                 options:nil
                                                                   error:&err];
                if (lib) {
                    id<MTLFunction> vert = [lib newFunctionWithName:@"main0"];
                    if (vert) {
                        MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
                        desc.vertexFunction = vert;
                        desc.fragmentFunction = [g_state.defaultLibrary newFunctionWithName:@"default_fragment"];
                        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                        desc.colorAttachments[0].writeMask = TranslateColorWriteMask(g_state.cbTargetMask);
                        desc.colorAttachments[0].blendingEnabled = g_state.blendEnable;
                        if (g_state.blendEnable) {
                            desc.colorAttachments[0].sourceRGBBlendFactor = g_state.srcRgbFactor;
                            desc.colorAttachments[0].destinationRGBBlendFactor = g_state.dstRgbFactor;
                            desc.colorAttachments[0].rgbBlendOperation = g_state.rgbOp;
                            desc.colorAttachments[0].sourceAlphaBlendFactor = g_state.srcAlphaFactor;
                            desc.colorAttachments[0].destinationAlphaBlendFactor = g_state.dstAlphaFactor;
                            desc.colorAttachments[0].alphaBlendOperation = g_state.alphaOp;
                        }
                        id<MTLRenderPipelineState> pso = [g_state.device newRenderPipelineStateWithDescriptor:desc error:&err];
                        if (pso) {
                            g_state.renderPipelineCache[key] = pso;
                            g_pipelines_created_count++;
                            return pso;
                        }
                    }
                }
            } catch (...) {
                // Fall back smoothly
            }
        } else {
            // 2. AMD GCN Liverpool Shader Recompilation
            GcnShaderMetadata vs_meta;
            GcnShaderMetadata ps_meta;
            bool vs_ok = GcnRecompiler::AnalyzeShader(ctx->mem_base + g_state.vsGpuAddr,
                                                     ctx->mem_size - g_state.vsGpuAddr,
                                                     GcnStage::Vertex, vs_meta);
            bool ps_ok = false;
            if (g_state.psGpuAddr >= 0x10000 && g_state.psGpuAddr < ctx->mem_size) {
                ps_ok = GcnRecompiler::AnalyzeShader(ctx->mem_base + g_state.psGpuAddr,
                                                     ctx->mem_size - g_state.psGpuAddr,
                                                     GcnStage::Pixel, ps_meta);
            }

            if (vs_ok) {
                std::string mslSource = GcnRecompiler::GenerateDirectMsl(vs_meta, ps_meta);
                NSError *err = nil;
                id<MTLLibrary> lib = [g_state.device newLibraryWithSource:[NSString stringWithUTF8String:mslSource.c_str()]
                                                                 options:nil
                                                                   error:&err];
                if (lib) {
                    id<MTLFunction> vert = [lib newFunctionWithName:@"guest_vertex"];
                    id<MTLFunction> frag = [lib newFunctionWithName:@"guest_fragment"];
                    if (vert && frag) {
                        MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
                        desc.vertexFunction = vert;
                        desc.fragmentFunction = frag;
                        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                        desc.colorAttachments[0].writeMask = TranslateColorWriteMask(g_state.cbTargetMask);
                        desc.colorAttachments[0].blendingEnabled = g_state.blendEnable;
                        if (g_state.blendEnable) {
                            desc.colorAttachments[0].sourceRGBBlendFactor = g_state.srcRgbFactor;
                            desc.colorAttachments[0].destinationRGBBlendFactor = g_state.dstRgbFactor;
                            desc.colorAttachments[0].rgbBlendOperation = g_state.rgbOp;
                            desc.colorAttachments[0].sourceAlphaBlendFactor = g_state.srcAlphaFactor;
                            desc.colorAttachments[0].destinationAlphaBlendFactor = g_state.dstAlphaFactor;
                            desc.colorAttachments[0].alphaBlendOperation = g_state.alphaOp;
                        }
                        id<MTLRenderPipelineState> pso = [g_state.device newRenderPipelineStateWithDescriptor:desc error:&err];
                        if (pso) {
                            g_state.renderPipelineCache[key] = pso;
                            g_pipelines_created_count++;
                            static uint64_t s_recomp_logged = 0;
                            if (++s_recomp_logged <= 20) {
                                fprintf(stderr, "[ps4-gnm-metal] Heuristic MSL substitute compiled (GCN instructions NOT translated): VS=0x%llx (sz=%zu) PS=0x%llx (sz=%zu, tex_hint=%d) -> PSO (total=%llu)\n",
                                        (unsigned long long)g_state.vsGpuAddr, vs_meta.code_size_bytes,
                                        (unsigned long long)g_state.psGpuAddr, ps_meta.code_size_bytes,
                                        (int)ps_meta.uses_texture, (unsigned long long)g_pipelines_created_count);
                            }
                            return pso;
                        }
                    }
                }
            }
        }
    }

    g_state.renderPipelineCache[key] = g_state.defaultRenderPipeline;
    return g_state.defaultRenderPipeline;
}

static void ProcessStatePacket(GuestContext *ctx, uint32_t opcode, const uint32_t *payload, uint32_t payload_dw, const char *source_tag) {
    if (!ctx) return;

    switch (opcode) {
    case PM4_OP_NOP:
        break;

    case PM4_OP_CLEAR_STATE: {
        g_state.viewport = {0.0, 0.0, 1920.0, 1080.0, 0.0, 1.0};
        g_state.scissorRegs = GcnScissorRegisters{};
        UpdateMetalScissor();
        g_state.viewportBounds[0] = 0.0f;
        g_state.viewportBounds[1] = 0.0f;
        g_state.viewportBounds[2] = 0.0f;
        g_state.viewportBounds[3] = 0.0f;
        g_state.viewportBounds[4] = 0.0f;
        g_state.viewportBounds[5] = 0.0f;
        g_state.viewportControl = 0;
        g_state.viewportBoundsSeenMask = 0;
        g_state.primitiveType = MTLPrimitiveTypeTriangle;
        g_state.guestPrimitiveType = 4;
        break;
    }

    case PM4_OP_SET_CONTEXT_REG: {
        if (payload_dw >= 1) {
            uint32_t reg_offset = payload[0] & 0xFFFF;
            for (uint32_t r = 1; r < payload_dw; r++) {
                uint32_t curr_reg = reg_offset + (r - 1);
                uint32_t val = payload[r];

                if (curr_reg >= CTX_REG_CB_COLOR0_BASE && curr_reg < CTX_REG_CB_COLOR0_BASE + 8 * CTX_REG_CB_COLOR_STRIDE) {
                    uint32_t slot = (curr_reg - CTX_REG_CB_COLOR0_BASE) / CTX_REG_CB_COLOR_STRIDE;
                    uint32_t sub_reg = (curr_reg - CTX_REG_CB_COLOR0_BASE) % CTX_REG_CB_COLOR_STRIDE;
                    if (sub_reg == 0) {
                        g_state.colorBufferBase[slot] = (uint64_t)val << 8;
                    } else if (sub_reg == 4) {
                        g_state.colorBufferInfo[slot] = val;
                    }
                } else if (curr_reg == CTX_REG_PA_SC_SCREEN_SCISSOR_TL) {
                    g_state.scissorRegs.screen_tl = val;
                    UpdateMetalScissor();
                } else if (curr_reg == CTX_REG_PA_SC_SCREEN_SCISSOR_BR) {
                    g_state.scissorRegs.screen_br = val;
                    UpdateMetalScissor();
                } else if (curr_reg == CTX_REG_PA_SC_WINDOW_OFFSET) {
                    g_state.scissorRegs.window_offset = val;
                    UpdateMetalScissor();
                } else if (curr_reg == CTX_REG_PA_SC_WINDOW_SCISSOR_TL) {
                    g_state.scissorRegs.window_tl = val;
                    UpdateMetalScissor();
                } else if (curr_reg == CTX_REG_PA_SC_WINDOW_SCISSOR_BR) {
                    g_state.scissorRegs.window_br = val;
                    UpdateMetalScissor();
                } else if (curr_reg == CTX_REG_PA_SC_GENERIC_SCISSOR_TL) {
                    g_state.scissorRegs.generic_tl = val;
                    UpdateMetalScissor();
                } else if (curr_reg == CTX_REG_PA_SC_GENERIC_SCISSOR_BR) {
                    g_state.scissorRegs.generic_br = val;
                    UpdateMetalScissor();
                } else if (curr_reg == CTX_REG_PA_SC_VPORT_SCISSOR0_TL) {
                    g_state.scissorRegs.viewport0_tl = val;
                    UpdateMetalScissor();
                } else if (curr_reg == CTX_REG_PA_SC_VPORT_SCISSOR0_BR) {
                    g_state.scissorRegs.viewport0_br = val;
                    UpdateMetalScissor();
                } else if (curr_reg == CTX_REG_PA_SC_MODE_CONTROL) {
                    g_state.scissorRegs.mode_control = val;
                    UpdateMetalScissor();
                } else if (curr_reg >= CTX_REG_PA_CL_VPORT_XSCALE &&
                           curr_reg <= CTX_REG_PA_CL_VPORT_ZOFFSET) {
                    const uint32_t index = curr_reg - CTX_REG_PA_CL_VPORT_XSCALE;
                    memcpy(&g_state.viewportBounds[index], &val, sizeof(val));
                    if (index == 0) g_state.viewportBoundsSeenMask |= 0x1u;
                    if (index == 2) g_state.viewportBoundsSeenMask |= 0x4u;
                    UpdateMetalViewport();
                } else if (curr_reg == CTX_REG_PA_CL_VPORT_CONTROL) {
                    g_state.viewportControl = val;
                    UpdateMetalViewport();
                } else if (curr_reg >= CTX_REG_SPI_PS_INPUT_CNTL_0 &&
                           curr_reg < CTX_REG_SPI_PS_INPUT_CNTL_0 + 32) {
                    g_state.fragmentState.input_cntl[curr_reg - CTX_REG_SPI_PS_INPUT_CNTL_0] = val;
                } else if (curr_reg == CTX_REG_SPI_PS_IN_CONTROL) {
                    g_state.fragmentState.input_count = val & 0x3F;
                } else if (curr_reg == CTX_REG_SPI_PS_INPUT_ENA) {
                    g_state.fragmentState.input_ena = val;
                } else if (curr_reg == CTX_REG_SPI_PS_INPUT_ADDR) {
                    g_state.fragmentState.input_addr = val;
                } else if (curr_reg == CTX_REG_SPI_SHADER_COL_FORMAT) {
                    g_state.fragmentState.color_export_formats = val;
                } else if (curr_reg == CTX_REG_SPI_SHADER_Z_FORMAT) {
                    g_state.fragmentState.z_export_format = val;
                } else if (curr_reg == CTX_REG_CB_TARGET_MASK) {
                    g_state.cbTargetMask = val;
                } else if (curr_reg == CTX_REG_VGT_INDX_OFFSET) {
                    g_state.vgtIndexOffset = (int32_t)val;
                } else if (curr_reg == CTX_REG_CB_BLEND0_CONTROL) {
                    g_state.blendEnable = (val & 1) != 0;
                    g_state.srcRgbFactor = TranslateBlendFactor((val >> 1) & 0x1F);
                    g_state.dstRgbFactor = TranslateBlendFactor((val >> 6) & 0x1F);
                    g_state.rgbOp = TranslateBlendOp((val >> 11) & 0x7);
                    g_state.srcAlphaFactor = TranslateBlendFactor((val >> 16) & 0x1F);
                    g_state.dstAlphaFactor = TranslateBlendFactor((val >> 21) & 0x1F);
                    g_state.alphaOp = TranslateBlendOp((val >> 26) & 0x7);
                } else if (curr_reg == CTX_REG_DB_DEPTH_CONTROL) {
                    g_state.depthWriteEnable = (val & 0x1) != 0;
                    g_state.depthCompareFunc = TranslateCompareFunc((val >> 1) & 0x7);
                }
            }
        }
        break;
    }

    case PM4_OP_SET_SH_REG: {
        if (payload_dw >= 1) {
            uint32_t reg_offset = payload[0] & 0xFFFF;
            for (uint32_t r = 1; r < payload_dw; r++) {
                uint32_t curr_reg = reg_offset + (r - 1);
                uint32_t val = payload[r];

                if (curr_reg == SH_REG_SPI_SHADER_PGM_LO_VS) {
                    g_state.vsGpuAddr = (g_state.vsGpuAddr & 0xFFFFFF0000000000ULL) | ((uint64_t)val << 8);
                } else if (curr_reg == SH_REG_SPI_SHADER_PGM_HI_VS) {
                    g_state.vsGpuAddr = (g_state.vsGpuAddr & 0x000000FFFFFFFFFFULL) | (((uint64_t)val & 0xFFu) << 40);
                } else if (curr_reg == SH_REG_SPI_SHADER_PGM_LO_PS) {
                    g_state.psGpuAddr = (g_state.psGpuAddr & 0xFFFFFF0000000000ULL) | ((uint64_t)val << 8);
                } else if (curr_reg == SH_REG_SPI_SHADER_PGM_HI_PS) {
                    g_state.psGpuAddr = (g_state.psGpuAddr & 0x000000FFFFFFFFFFULL) | (((uint64_t)val & 0xFFu) << 40);
                } else if (curr_reg >= SH_REG_SPI_SHADER_USER_DATA_VS_0 && curr_reg < SH_REG_SPI_SHADER_USER_DATA_VS_0 + 16) {
                    g_state.vsUserData[curr_reg - SH_REG_SPI_SHADER_USER_DATA_VS_0] = val;
                } else if (curr_reg >= SH_REG_SPI_SHADER_USER_DATA_PS_0 && curr_reg < SH_REG_SPI_SHADER_USER_DATA_PS_0 + 16) {
                    g_state.psUserData[curr_reg - SH_REG_SPI_SHADER_USER_DATA_PS_0] = val;
                }
            }
        }
        break;
    }

    case PM4_OP_SET_CONFIG_REG:
        break;

    case PM4_OP_SET_UCONFIG_REG: {
        if (payload_dw >= 1) {
            const uint32_t reg_offset = payload[0] & 0xFFFFu;
            for (uint32_t r = 1; r < payload_dw; ++r) {
                const uint32_t curr_reg = reg_offset + (r - 1);
                if (curr_reg == UCONFIG_REG_VGT_PRIMITIVE_TYPE) {
                    g_state.guestPrimitiveType = payload[r] & 0x3Fu;
                    g_state.primitiveType = TranslatePrimitiveType(g_state.guestPrimitiveType);
                }
            }
        }
        break;
    }

    case PM4_OP_INDEX_BASE: {
        if (payload_dw >= 2) {
            g_state.indexBufferGpuAddr = (uint64_t)payload[0] | ((uint64_t)payload[1] << 32);
        }
        break;
    }

    case PM4_OP_INDEX_BUFFER_SIZE: {
        if (payload_dw >= 1) {
            g_state.indexBufferSize = payload[0];
        }
        break;
    }

    case PM4_OP_INDEX_TYPE: {
        if (payload_dw >= 1) {
            g_state.indexType = (payload[0] == 0) ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
        }
        break;
    }

    case PM4_OP_EVENT_WRITE:
        break;

    case PM4_OP_EVENT_WRITE_EOP: {
        if (payload_dw >= 4) {
            uint64_t addr_lo = (uint64_t)(payload[1] & ~0x7u);
            uint64_t addr_hi = ((uint64_t)(payload[2] & 0xFFFFu)) << 32;
            uint64_t dst_addr = addr_hi | addr_lo;
            uint32_t data_sel = (payload[2] >> 29) & 0x7;
            uint32_t int_sel  = (payload[2] >> 24) & 0x7;
            uint64_t write_val = 0;

            if (dst_addr != 0 && ctx->mem_base && dst_addr + 8 <= ctx->mem_size) {
                if (data_sel == 1) {
                    uint32_t val32 = payload[3];
                    write_val = val32;
                    __atomic_store_n((uint32_t *)(ctx->mem_base + dst_addr), val32, __ATOMIC_RELEASE);
                    if ((dst_addr & 7) == 0 && payload_dw == 4) {
                        __atomic_store_n((uint64_t *)(ctx->mem_base + dst_addr), (uint64_t)val32, __ATOMIC_RELEASE);
                    }
                } else if (data_sel == 2) {
                    write_val = (uint64_t)payload[3] | ((payload_dw >= 5) ? ((uint64_t)payload[4] << 32) : 0ULL);
                    __atomic_store_n((uint64_t *)(ctx->mem_base + dst_addr), write_val, __ATOMIC_RELEASE);
                } else if (data_sel == 3) {
                    static std::atomic<uint64_t> s_gpu_ts{1000};
                    write_val = s_gpu_ts.fetch_add(100);
                    __atomic_store_n((uint64_t *)(ctx->mem_base + dst_addr), write_val, __ATOMIC_RELEASE);
                }
                g_last_eop_dst.store(dst_addr);
                g_last_eop_val.store(write_val);
                std::atomic_thread_fence(std::memory_order_seq_cst);
#if defined(__aarch64__)
                asm volatile("dmb ish" ::: "memory");
#endif
                static uint64_t s_eop_logs = 0;
                if (++s_eop_logs <= 5 || (s_eop_logs % 120) == 0) {
                    fprintf(stderr, "[ps4-gnm-metal] %s EVENT_WRITE_EOP: dst=0x%llx data_sel=%u val=0x%llx int_sel=%u (total=%llu)\n",
                            source_tag, (unsigned long long)dst_addr, data_sel, (unsigned long long)write_val, int_sel, (unsigned long long)s_eop_logs);
                }
            }
            ps4_gnm_notify_eop();
        }
        break;
    }

    case PM4_OP_WRITE_DATA: {
        if (payload_dw >= 3) {
            uint64_t dst_addr = (uint64_t)payload[1] | ((uint64_t)payload[2] << 32);
            if (dst_addr != 0 && ctx->mem_base) {
                uint32_t *dst_mem = (uint32_t *)(ctx->mem_base + dst_addr);
                for (uint32_t w = 3; w < payload_dw; w++) {
                    *dst_mem++ = payload[w];
                }
            }
        }
        break;
    }

    default:
        break;
    }
}

int32_t ps4_gnm_metal_process_command_buffers(GuestContext *ctx, uint32_t count,
                                             const uint64_t *dcb_gpu_addrs, const uint32_t *dcb_sizes_in_bytes,
                                             const uint64_t *ccb_gpu_addrs, const uint32_t *ccb_sizes_in_bytes) {
    if (!ctx || count == 0) return 0;
    std::scoped_lock lock(g_state.mutex);
    if (!g_state.initialized) {
        if (ps4_gnm_metal_init(ctx) != 0) return -1;
    }

    @autoreleasepool {
        for (uint32_t b = 0; b < count; b++) {
            // 1. Process CCB (Constant Command Buffer)
            // In Liverpool GNM architecture, the Constant Engine emits all register state,
            // User Data SGPRs (SRTs, constant buffers, textures), and render targets into CCB.
            if (ccb_gpu_addrs && ccb_sizes_in_bytes && ccb_gpu_addrs[b] && ccb_sizes_in_bytes[b]) {
                const uint32_t *ccb_words = (const uint32_t *)(ctx->mem_base + ccb_gpu_addrs[b]);
                uint32_t ccb_dw = ccb_sizes_in_bytes[b] / 4;
                uint32_t cpc = 0;
                while (cpc < ccb_dw) {
                    uint32_t header = ccb_words[cpc];
                    uint32_t type = (header >> 30) & 0x3;
                    if (type == 3) {
                        g_commands_decoded_count++;
                        uint32_t opcode = (header >> 8) & 0xFF;
                        uint32_t payload_dw = ((header >> 16) & 0x3FFF) + 1;
                        if (cpc + 1 + payload_dw > ccb_dw) break;
                        const uint32_t *payload = &ccb_words[cpc + 1];

                        ProcessStatePacket(ctx, opcode, payload, payload_dw, "CCB");
                        cpc += 1 + payload_dw;
                    } else if (type == 0) {
                        uint32_t count_dw = (header >> 16) & 0x3FFF;
                        cpc += 1 + count_dw + 1;
                    } else {
                        cpc += 1;
                    }
                }
            }

            // 2. Process DCB (Draw Command Buffer)
            // Draw Engine decodes draw calls, dispatches, and any local state modifications.
            if (dcb_gpu_addrs && dcb_sizes_in_bytes && dcb_gpu_addrs[b] && dcb_sizes_in_bytes[b]) {
                const uint32_t *dcb_words = (const uint32_t *)(ctx->mem_base + dcb_gpu_addrs[b]);
                uint32_t dcb_dw = dcb_sizes_in_bytes[b] / 4;
                uint32_t pc = 0;

                while (pc < dcb_dw) {
                    uint32_t header = dcb_words[pc];
                    uint32_t type = (header >> 30) & 0x3;

                    if (type == 3) {
                        g_commands_decoded_count++;
                        uint32_t opcode = (header >> 8) & 0xFF;
                        uint32_t payload_dw = ((header >> 16) & 0x3FFF) + 1;
                        if (pc + 1 + payload_dw > dcb_dw) break;
                        const uint32_t *payload = &dcb_words[pc + 1];

                        switch (opcode) {
                        case PM4_OP_DRAW_INDEX_2: {
                            g_draws_intercepted_count++;
                            if (payload_dw >= 3) {
                                uint32_t indexCount = payload[0];
                                uint64_t indexBase = (uint64_t)payload[1] | ((uint64_t)payload[2] << 32);
                                if (indexBase == 0) indexBase = g_state.indexBufferGpuAddr;

                                EnsureRenderEncoder(ctx);
                                if (g_state.currentRenderEncoder && indexCount > 0 && indexBase != 0) {
                                    size_t ibOffset = 0;
                                    size_t ibBytes = indexCount * (g_state.indexType == MTLIndexTypeUInt16 ? 2 : 4);
                                    id<MTLBuffer> ibBuf = WrapGuestMemory(ctx, indexBase, ibBytes, &ibOffset);

                                    if (ibBuf) {
                                        BindVertexAndTexture(ctx);
                                        CaptureLargestDrawState(ctx, dcb_gpu_addrs[b], dcb_sizes_in_bytes[b],
                                                                pc, opcode, payload, payload_dw,
                                                                indexBase, indexCount, ibBytes);
                                        id<MTLRenderPipelineState> pso = GetOrCreateRenderPipeline(ctx);
                                        [g_state.currentRenderEncoder setRenderPipelineState:pso];
                                        [g_state.currentRenderEncoder setViewport:g_state.viewport];
                                        [g_state.currentRenderEncoder setScissorRect:g_state.scissor];

                                        [g_state.currentRenderEncoder drawIndexedPrimitives:g_state.primitiveType
                                                                                 indexCount:indexCount
                                                                                  indexType:g_state.indexType
                                                                                indexBuffer:ibBuf
                                                                          indexBufferOffset:ibOffset
                                                                              instanceCount:1
                                                                                 baseVertex:g_state.vgtIndexOffset
                                                                               baseInstance:0];
                                        g_draws_encoded_count++;
                                        ProbeDrawPixels();
                                        g_index_buffers_bound_count++;
                                    }
                                }
                            }
                            break;
                        }

                        case PM4_OP_DRAW_INDEX_AUTO: {
                            g_draws_intercepted_count++;
                            if (payload_dw >= 1) {
                                uint32_t vertexCount = payload[0];
                                EnsureRenderEncoder(ctx);
                                if (g_state.currentRenderEncoder && vertexCount > 0) {
                                    BindVertexAndTexture(ctx);
                                    id<MTLRenderPipelineState> pso = GetOrCreateRenderPipeline(ctx);
                                    [g_state.currentRenderEncoder setRenderPipelineState:pso];
                                    [g_state.currentRenderEncoder setViewport:g_state.viewport];
                                    [g_state.currentRenderEncoder setScissorRect:g_state.scissor];

                                    [g_state.currentRenderEncoder drawPrimitives:g_state.primitiveType
                                                                     vertexStart:0
                                                                     vertexCount:vertexCount];
                                    g_draws_encoded_count++;
                                    ProbeDrawPixels();
                                }
                            }
                            break;
                        }

                        case PM4_OP_DRAW_INDEX_OFFSET_2: {
                            g_draws_intercepted_count++;
                            if (payload_dw >= 3) {
                                uint32_t indexCount = payload[0];
                                uint32_t indexOffset = payload[1];
                                EnsureRenderEncoder(ctx);
                                if (g_state.currentRenderEncoder && indexCount > 0 && g_state.indexBufferGpuAddr != 0) {
                                    size_t ibOffset = 0;
                                    size_t ibBytes = (indexCount + indexOffset) * (g_state.indexType == MTLIndexTypeUInt16 ? 2 : 4);
                                    id<MTLBuffer> ibBuf = WrapGuestMemory(ctx, g_state.indexBufferGpuAddr, ibBytes, &ibOffset);

                                    if (ibBuf) {
                                        BindVertexAndTexture(ctx);
                                        id<MTLRenderPipelineState> pso = GetOrCreateRenderPipeline(ctx);
                                        [g_state.currentRenderEncoder setRenderPipelineState:pso];
                                        [g_state.currentRenderEncoder setViewport:g_state.viewport];
                                        [g_state.currentRenderEncoder setScissorRect:g_state.scissor];

                                        size_t elemSize = (g_state.indexType == MTLIndexTypeUInt16 ? 2 : 4);
                                        [g_state.currentRenderEncoder drawIndexedPrimitives:g_state.primitiveType
                                                                                 indexCount:indexCount
                                                                                  indexType:g_state.indexType
                                                                                indexBuffer:ibBuf
                                                                          indexBufferOffset:ibOffset + indexOffset * elemSize
                                                                              instanceCount:1
                                                                                 baseVertex:g_state.vgtIndexOffset
                                                                               baseInstance:0];
                                        g_draws_encoded_count++;
                                        ProbeDrawPixels();
                                        g_index_buffers_bound_count++;
                                    }
                                }
                            }
                            break;
                        }

                        case PM4_OP_DRAW_INDEX_INDIRECT: {
                            g_draws_intercepted_count++;
                            if (payload_dw >= 2) {
                                uint64_t indirectGpuAddr = (uint64_t)payload[0] | ((uint64_t)payload[1] << 32);
                                EnsureRenderEncoder(ctx);
                                if (g_state.currentRenderEncoder && indirectGpuAddr != 0 && g_state.indexBufferGpuAddr != 0) {
                                    size_t indOffset = 0;
                                    id<MTLBuffer> indBuf = WrapGuestMemory(ctx, indirectGpuAddr, 32, &indOffset);
                                    size_t ibOffset = 0;
                                    id<MTLBuffer> ibBuf = WrapGuestMemory(ctx, g_state.indexBufferGpuAddr, 65536 * 4, &ibOffset);

                                    if (indBuf && ibBuf) {
                                        BindVertexAndTexture(ctx);
                                        id<MTLRenderPipelineState> pso = GetOrCreateRenderPipeline(ctx);
                                        [g_state.currentRenderEncoder setRenderPipelineState:pso];
                                        [g_state.currentRenderEncoder setViewport:g_state.viewport];
                                        [g_state.currentRenderEncoder setScissorRect:g_state.scissor];

                                        [g_state.currentRenderEncoder drawIndexedPrimitives:g_state.primitiveType
                                                                                  indexType:g_state.indexType
                                                                                indexBuffer:ibBuf
                                                                          indexBufferOffset:ibOffset
                                                                              indirectBuffer:indBuf
                                                                        indirectBufferOffset:indOffset];
                                        g_draws_encoded_count++;
                                        ProbeDrawPixels();
                                        g_index_buffers_bound_count++;
                                    }
                                }
                            }
                            break;
                        }

                        case PM4_OP_DRAW_INDIRECT: {
                            g_draws_intercepted_count++;
                            if (payload_dw >= 2) {
                                uint64_t indirectGpuAddr = (uint64_t)payload[0] | ((uint64_t)payload[1] << 32);
                                EnsureRenderEncoder(ctx);
                                if (g_state.currentRenderEncoder && indirectGpuAddr != 0) {
                                    size_t indOffset = 0;
                                    id<MTLBuffer> indBuf = WrapGuestMemory(ctx, indirectGpuAddr, 32, &indOffset);

                                    if (indBuf) {
                                        BindVertexAndTexture(ctx);
                                        id<MTLRenderPipelineState> pso = GetOrCreateRenderPipeline(ctx);
                                        [g_state.currentRenderEncoder setRenderPipelineState:pso];
                                        [g_state.currentRenderEncoder setViewport:g_state.viewport];
                                        [g_state.currentRenderEncoder setScissorRect:g_state.scissor];

                                        [g_state.currentRenderEncoder drawPrimitives:g_state.primitiveType
                                                                      indirectBuffer:indBuf
                                                                indirectBufferOffset:indOffset];
                                        g_draws_encoded_count++;
                                        ProbeDrawPixels();
                                    }
                                }
                            }
                            break;
                        }

                        case PM4_OP_DISPATCH_DIRECT: {
                            if (payload_dw >= 3) {
                                uint32_t dim_x = payload[0];
                                uint32_t dim_y = payload[1];
                                uint32_t dim_z = payload[2];
                                g_dispatches_encoded_count++;

                                if (dim_x > 0 && dim_y > 0 && dim_z > 0 && g_state.computePipelineCache.size() > 0) {
                                    if (g_state.currentRenderEncoder) {
                                        [g_state.currentRenderEncoder endEncoding];
                                        g_state.currentRenderEncoder = nil;
                                    }
                                    if (!g_state.currentCmdBuffer) {
                                        g_state.currentCmdBuffer = [g_state.cmdQueue commandBuffer];
                                    }
                                    if (!g_state.currentComputeEncoder) {
                                        g_state.currentComputeEncoder = [g_state.currentCmdBuffer computeCommandEncoder];
                                    }
                                    id<MTLComputePipelineState> cpso = g_state.computePipelineCache.begin()->second;
                                    [g_state.currentComputeEncoder setComputePipelineState:cpso];
                                    MTLSize gridSize = MTLSizeMake(dim_x, dim_y, dim_z);
                                    MTLSize tgSize = MTLSizeMake(8, 8, 1);
                                    [g_state.currentComputeEncoder dispatchThreadgroups:gridSize threadsPerThreadgroup:tgSize];
                                }
                            }
                            break;
                        }

                        default:
                            ProcessStatePacket(ctx, opcode, payload, payload_dw, "DCB");
                            break;
                        }

                        pc += 1 + payload_dw;
                    } else if (type == 0) {
                        uint32_t count_dw = (header >> 16) & 0x3FFF;
                        pc += 1 + count_dw + 1;
                    } else {
                        pc += 1;
                    }
                }
            }
        }

        // Commit active Metal command buffer
        if (g_state.currentRenderEncoder) {
            [g_state.currentRenderEncoder endEncoding];
            g_state.currentRenderEncoder = nil;
        }
        if (g_state.currentComputeEncoder) {
            [g_state.currentComputeEncoder endEncoding];
            g_state.currentComputeEncoder = nil;
        }
        if (g_state.currentCmdBuffer) {
            [g_state.currentCmdBuffer commit];
            g_state.currentCmdBuffer = nil;
        }
    }

    return 0;
}

extern "C" int ps4_gnm_metal_present(GuestContext *ctx, uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch,
                                     uint32_t tiling_mode, uint32_t pixel_format, void (*on_done)(void *), void *arg) {
    if (!ctx || width == 0 || height == 0) return 0;
    std::scoped_lock lock(g_state.mutex);
    if (!g_state.initialized) {
        if (ps4_gnm_metal_init(ctx) != 0) return 0;
    }

    @autoreleasepool {
        id<MTLTexture> screenTex = (__bridge id<MTLTexture>)ps4_metal_screen_get_texture();
        if (!screenTex) return 0;

        // The Metal command processor renders into screenTex, not into PS4 guest
        // framebuffer memory. Treating any nonzero guest-memory sample as a valid
        // completed frame replaces that rendering with unrelated/stale guest bytes
        // (observed as flickering horizontal red/white corruption). Keep the old
        // upload path available for explicit experiments, but never select it from
        // a heuristic sample by default.
        static const bool s_allow_guest_memory_present = [] {
            const char *value = getenv("PS4_GNM_METAL_PRESENT_GUEST_MEMORY");
            return value && (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
                             strcmp(value, "yes") == 0);
        }();
        bool has_guest_pixels = false;
        bool guest_frame_range_valid = false;
        size_t guest_bytes_per_row = 0;
        size_t guest_frame_bytes = 0;
        if (s_allow_guest_memory_present && addr != 0 && ctx->mem_base && addr < ctx->mem_size &&
            width <= screenTex.width && height <= screenTex.height) {
            const size_t effective_pitch = pitch > 0 ? pitch : width;
            guest_bytes_per_row = effective_pitch * 4;
            guest_frame_range_valid = effective_pitch >= width &&
                height <= SIZE_MAX / guest_bytes_per_row &&
                (guest_frame_bytes = guest_bytes_per_row * height) <= ctx->mem_size - (size_t)addr;
        }
        if (guest_frame_range_valid) {
            size_t total_pixels = (size_t)width * height;
            size_t step = total_pixels > 256 ? (total_pixels / 256) : 1;
            for (size_t i = 0; i < total_pixels; i += step) {
                const size_t y = i / width;
                const size_t x = i % width;
                const uint32_t pixel = *(const uint32_t *)(ctx->mem_base + addr +
                    y * guest_bytes_per_row + x * sizeof(uint32_t));
                if ((pixel & 0x00FFFFFFu) != 0) {
                    has_guest_pixels = true;
                    break;
                }
            }
        }

        static uint64_t s_present_calls = 0;
        s_present_calls++;
        if (s_present_calls == 1) {
            fprintf(stderr,
                    "[ps4-gnm-metal] presentation source: %s (guest-memory override %s; addr=0x%llx range=%s)\n",
                    s_allow_guest_memory_present ? "guest-memory-if-nonzero" : "Metal render target",
                    s_allow_guest_memory_present ? "enabled" : "disabled",
                    (unsigned long long)addr, guest_frame_range_valid ? "valid" : "not-used-or-invalid");
        }
        if ((s_present_calls % 60) == 1) {
            fprintf(stderr, "[ps4-gnm-metal] present #%llu: addr=0x%llx %ux%u (tmode=%u, fmt=0x%x) | PM4: decoded=%llu, draws_in=%llu, draws_enc=%llu, vb_bound=%llu, ib_bound=%llu, tex_bound=%llu, disp=%llu, pso=%llu, rt0=0x%llx\n",
                    (unsigned long long)s_present_calls, (unsigned long long)addr, width, height,
                    tiling_mode, pixel_format,
                    (unsigned long long)g_commands_decoded_count,
                    (unsigned long long)g_draws_intercepted_count,
                    (unsigned long long)g_draws_encoded_count,
                    (unsigned long long)g_vertex_buffers_bound_count,
                    (unsigned long long)g_index_buffers_bound_count,
                    (unsigned long long)g_textures_resolved_count,
                    (unsigned long long)g_dispatches_encoded_count,
                    (unsigned long long)g_pipelines_created_count,
                    (unsigned long long)g_state.colorBufferBase[0]);
        }
        g_state.needsClear = true;

        if (has_guest_pixels) {
            if (tiling_mode != 0 && g_state.detileComputePipeline) {
                // Detile using GPU compute shader
                if (!g_state.currentCmdBuffer) {
                    g_state.currentCmdBuffer = [g_state.cmdQueue commandBuffer];
                }
                id<MTLComputeCommandEncoder> compEncoder = [g_state.currentCmdBuffer computeCommandEncoder];
                [compEncoder setComputePipelineState:g_state.detileComputePipeline];

                size_t in_bytes = (size_t)width * height * 4;
                size_t offset = 0;
                id<MTLBuffer> srcBuf = WrapGuestMemory(ctx, addr, in_bytes, &offset);
                if (srcBuf) {
                    [compEncoder setBuffer:srcBuf offset:offset atIndex:0];
                    [compEncoder setTexture:screenTex atIndex:0];
                    uint32_t dims[2] = {width, height};
                    [compEncoder setBytes:dims length:sizeof(dims) atIndex:1];

                    MTLSize gridSize = MTLSizeMake((width + 7) / 8 * 8, (height + 7) / 8 * 8, 1);
                    MTLSize tgSize = MTLSizeMake(8, 8, 1);
                    [compEncoder dispatchThreads:gridSize threadsPerThreadgroup:tgSize];
                }
                [compEncoder endEncoding];
            } else {
                // Linear guest pixels: upload directly to screenTex
                MTLRegion region = MTLRegionMake2D(0, 0, width, height);
                [screenTex replaceRegion:region mipmapLevel:0 withBytes:ctx->mem_base + addr
                             bytesPerRow:guest_bytes_per_row];
            }
        }

        // Commit active render/compute work and present screenTex to CAMetalDrawable
        if (g_state.currentRenderEncoder) {
            [g_state.currentRenderEncoder endEncoding];
            g_state.currentRenderEncoder = nil;
        }
        if (g_state.currentComputeEncoder) {
            [g_state.currentComputeEncoder endEncoding];
            g_state.currentComputeEncoder = nil;
        }
        if (g_state.currentCmdBuffer) {
            [g_state.currentCmdBuffer commit];
            g_state.currentCmdBuffer = nil;
        }

        // Present screenTex without overwriting it with black bytes
        ps4_metal_screen_present_frame(NULL, 0, (int)width, (int)height);

        if (on_done) {
            on_done(arg);
        }
    }
    return 1;
}

extern "C" int ps4_gnm_read_display_cpu(GuestContext *ctx, uint64_t addr, uint32_t width, uint32_t height, uint32_t pitch,
                                        uint32_t tiling_mode, uint32_t pixel_format, uint8_t *out_bgra) {
    if (!ctx || !ctx->mem_base || addr == 0 || !out_bgra) return 0;
    if (tiling_mode == 0) {
        size_t bpr = pitch > 0 ? (size_t)pitch * 4 : (size_t)width * 4;
        const uint8_t *src = (const uint8_t *)(ctx->mem_base + addr);
        for (uint32_t y = 0; y < height; y++) {
            memcpy(out_bgra + y * width * 4, src + y * bpr, width * 4);
        }
        return 1;
    }

    const uint32_t *src_tiled = (const uint32_t *)(ctx->mem_base + addr);
    uint32_t *dst_linear = (uint32_t *)out_bgra;
    uint32_t tiles_per_row = (width + 7) / 8;

    for (uint32_t y = 0; y < height; y++) {
        uint32_t tile_y = y / 8;
        uint32_t in_y = y % 8;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t tile_x = x / 8;
            uint32_t in_x = x % 8;
            uint32_t tile_idx = tile_y * tiles_per_row + tile_x;
            uint32_t morton = ((in_x & 1) << 0) | ((in_y & 1) << 1) |
                              ((in_x & 2) << 1) | ((in_y & 2) << 2) |
                              ((in_x & 4) << 2) | ((in_y & 4) << 3);
            uint32_t pixel_idx = tile_idx * 64 + morton;
            dst_linear[y * width + x] = src_tiled[pixel_idx];
        }
    }
    return 1;
}

extern "C" uint64_t ps4_gnm_get_commands_decoded_count(void) { return g_commands_decoded_count; }
extern "C" uint64_t ps4_gnm_get_draws_encoded_count(void) { return g_draws_encoded_count; }
extern "C" uint64_t ps4_gnm_get_last_eop_dst(void) { return g_last_eop_dst.load(); }
extern "C" uint64_t ps4_gnm_get_last_eop_val(void) { return g_last_eop_val.load(); }
extern "C" bool ps4_gnm_metal_is_initialized(void) { return g_state.initialized; }

extern "C" void ps4_gnm_metal_wait_gpu_idle(void) {
    std::scoped_lock lock(g_state.mutex);
    if (!g_state.initialized || !g_state.cmdQueue) return;

    if (g_state.currentRenderEncoder) {
        [g_state.currentRenderEncoder endEncoding];
        g_state.currentRenderEncoder = nil;
    }
    if (g_state.currentComputeEncoder) {
        [g_state.currentComputeEncoder endEncoding];
        g_state.currentComputeEncoder = nil;
    }
    if (g_state.currentCmdBuffer) {
        [g_state.currentCmdBuffer commit];
        [g_state.currentCmdBuffer waitUntilCompleted];
        g_state.currentCmdBuffer = nil;
    } else {
        id<MTLCommandBuffer> barrier = [g_state.cmdQueue commandBuffer];
        if (barrier) {
            [barrier commit];
            [barrier waitUntilCompleted];
        }
    }
}

extern "C" void ps4_gnm_metal_destroy(void) {
    std::scoped_lock lock(g_state.mutex);
    if (!g_state.initialized) return;

    fprintf(stderr, "[ps4-gnm-metal] Shutting down Metal GNM Command Processor and releasing GPU resources\n");

    // 1. Wait for in-flight GPU execution to complete
    ps4_gnm_metal_wait_gpu_idle();

    // 2. Clear pipeline and texture caches (releases strong ARC/ObjC references)
    g_state.renderPipelineCache.clear();
    g_state.computePipelineCache.clear();
    g_state.textureCache.clear();

    // 3. Release Metal state objects
    g_state.defaultLibrary = nil;
    g_state.defaultRenderPipeline = nil;
    g_state.detileComputePipeline = nil;
    g_state.defaultDepthStencilState = nil;
    g_state.defaultSamplerState = nil;

    // 4. Release command queue and device
    g_state.cmdQueue = nil;
    g_state.device = nil;

    // 5. Reset tracking state
    g_state.needsClear = true;
    g_state.indexBufferGpuAddr = 0;
    g_state.indexBufferSize = 0;
    g_state.vsGpuAddr = 0;
    g_state.psGpuAddr = 0;
    g_state.csGpuAddr = 0;
    memset(g_state.vsUserData, 0, sizeof(g_state.vsUserData));
    memset(g_state.psUserData, 0, sizeof(g_state.psUserData));
    memset(g_state.csUserData, 0, sizeof(g_state.csUserData));
    memset(g_state.colorBufferBase, 0, sizeof(g_state.colorBufferBase));
    memset(g_state.colorBufferInfo, 0, sizeof(g_state.colorBufferInfo));
    g_state.depthBufferBase = 0;

    g_state.initialized = false;
    fprintf(stderr, "[ps4-gnm-metal] Metal GNM Command Processor shutdown complete.\n");
}

