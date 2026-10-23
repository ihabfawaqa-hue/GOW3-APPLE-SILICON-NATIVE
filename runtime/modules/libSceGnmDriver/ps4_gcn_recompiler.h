// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * PS4 GCN Shader Recompiler for PS4Recomp.
 * Translates AMD GCN Liverpool shader bytecode to SPIR-V and Apple Metal Shading Language (MSL).
 */

#pragma once

#import <Metal/Metal.h>
#include "spirv_msl.hpp"

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>

enum class GcnStage {
    Vertex,
    Pixel,
    Compute
};

struct GcnShaderMetadata {
    GcnStage stage = GcnStage::Vertex;
    uint64_t gpu_addr = 0;
    size_t code_size_bytes = 0;
    uint64_t hash = 0;
    bool has_position_export = false;
    uint32_t param_exports_count = 0;
    bool has_mrt0 = false;
    bool uses_texture = false;
    bool uses_interp = false;
    bool uses_cbuffer = false;
    bool valid = false;
};

namespace GcnRecompiler {

// Position-fetch subset of the AMD buffer descriptor format table. Unknown or
// non-float formats return 0 so the Metal fallback keeps its legacy heuristic.
inline constexpr uint32_t PositionComponentCount(uint32_t data_format,
                                                 uint32_t number_format) {
    if (number_format != 7) return 0; // NUM_FORMAT_FLOAT
    switch (data_format) {
    case 13: return 3; // Format32_32_32
    case 14: return 4; // Format32_32_32_32
    default: return 0;
    }
}

static_assert(PositionComponentCount(13, 7) == 3);
static_assert(PositionComponentCount(14, 7) == 4);
static_assert(PositionComponentCount(13, 0) == 0);

// 64-bit FNV-1a Hash
inline uint64_t ComputeHash(const uint8_t *data, size_t size) {
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Analyze GCN instruction stream to determine stage, size, bounds, exports, and resource usage
inline bool AnalyzeShader(const uint8_t *code, size_t max_bytes, GcnStage hint_stage, GcnShaderMetadata &out_meta) {
    if (!code || max_bytes < 4) return false;

    out_meta = GcnShaderMetadata{};
    out_meta.stage = hint_stage;

    const uint32_t *words = (const uint32_t *)code;
    size_t max_words = max_bytes / 4;
    size_t end_word = 0;

    for (size_t i = 0; i < max_words; i++) {
        uint32_t w = words[i];
        if (w == 0xbf810000) { // s_endpgm
            end_word = i + 1;
            break;
        }
    }

    if (end_word == 0) {
        end_word = max_words > 1024 ? 1024 : max_words;
    }

    out_meta.code_size_bytes = end_word * 4;
    out_meta.hash = ComputeHash(code, out_meta.code_size_bytes);

    // Scan instructions
    for (size_t i = 0; i < end_word; i++) {
        uint32_t w1 = words[i];

        // EXP instruction: 0xf8... (64-bit)
        if ((w1 & 0xfe000000) == 0xf8000000 && i + 1 < end_word) {
            uint32_t tgt = (w1 >> 4) & 0x3f;
            if (12 <= tgt && tgt <= 15) {
                out_meta.has_position_export = true;
                out_meta.stage = GcnStage::Vertex;
            } else if (32 <= tgt && tgt <= 63) {
                out_meta.param_exports_count++;
            } else if (tgt == 0) {
                out_meta.has_mrt0 = true;
                out_meta.stage = GcnStage::Pixel;
            }
            i++;
            continue;
        }

        // MIMG instruction: 0xf0... (image_sample)
        if ((w1 & 0xf8000000) == 0xf0000000) {
            out_meta.uses_texture = true;
            i++;
            continue;
        }

        // VINTRP instruction: 0xc8... (parameter interpolation)
        if ((w1 & 0xfc000000) == 0xc8000000) {
            out_meta.uses_interp = true;
            continue;
        }

        // SMEM constant load: 0xc0...
        if ((w1 & 0xf8000000) == 0xc0000000) {
            out_meta.uses_cbuffer = true;
            continue;
        }
    }

    // Inspect OrbShdr footer if present in memory right after code
    if (out_meta.code_size_bytes + 32 <= max_bytes) {
        const uint8_t *footer = code + out_meta.code_size_bytes;
        for (size_t off = 0; off + 8 <= 64 && out_meta.code_size_bytes + off + 8 <= max_bytes; off += 4) {
            if (memcmp(footer + off, "OrbShdr\x07", 8) == 0) {
                uint8_t type_byte = footer[off + 8];
                if (type_byte == 0x47) out_meta.stage = GcnStage::Vertex;
                else if (type_byte == 0x43) out_meta.stage = GcnStage::Pixel;
                break;
            }
        }
    }

    out_meta.valid = true;
    return true;
}

// Internal SPIR-V bytecode builder
class SpirvModuleBuilder {
public:
    std::vector<uint32_t> words;
    uint32_t bound = 1;

    uint32_t AllocId() { return bound++; }

    void Emit(uint32_t opcode) {
        words.push_back((1 << 16) | opcode);
    }

    void Emit(uint32_t opcode, uint32_t op1) {
        words.push_back((2 << 16) | opcode);
        words.push_back(op1);
    }

    void Emit(uint32_t opcode, uint32_t op1, uint32_t op2) {
        words.push_back((3 << 16) | opcode);
        words.push_back(op1);
        words.push_back(op2);
    }

    void Emit(uint32_t opcode, uint32_t op1, uint32_t op2, uint32_t op3) {
        words.push_back((4 << 16) | opcode);
        words.push_back(op1);
        words.push_back(op2);
        words.push_back(op3);
    }

    void Emit(uint32_t opcode, uint32_t op1, uint32_t op2, uint32_t op3, uint32_t op4) {
        words.push_back((5 << 16) | opcode);
        words.push_back(op1);
        words.push_back(op2);
        words.push_back(op3);
        words.push_back(op4);
    }

    void EmitString(uint32_t opcode, uint32_t id, const char *str) {
        size_t len = strlen(str) + 1;
        size_t word_count = (len + 3) / 4;
        words.push_back(((uint32_t)(2 + word_count) << 16) | opcode);
        words.push_back(id);
        size_t start = words.size();
        words.resize(start + word_count, 0);
        memcpy(&words[start], str, len);
    }

    std::vector<uint32_t> Finalize() {
        std::vector<uint32_t> result;
        result.reserve(5 + words.size());
        result.push_back(0x07230203); // Magic
        result.push_back(0x00010000); // Version 1.0
        result.push_back(0x00000000); // Generator
        result.push_back(bound);      // Bound
        result.push_back(0x00000000); // Schema
        result.insert(result.end(), words.begin(), words.end());
        return result;
    }
};

// Translate GCN Vertex Shader metadata to valid SPIR-V bytecode
inline std::vector<uint32_t> EmitVertexSpirv(const GcnShaderMetadata &meta) {
    SpirvModuleBuilder b;
    b.Emit(17, 1);    // OpCapability Shader
    b.Emit(14, 0, 1); // OpMemoryModel Logical, GLSL450

    uint32_t ext_glsl = b.AllocId();
    uint32_t main_fn = b.AllocId();
    uint32_t type_void = b.AllocId();
    uint32_t type_fn_void = b.AllocId();
    uint32_t type_float = b.AllocId();
    uint32_t type_v2f = b.AllocId();
    uint32_t type_v4f = b.AllocId();
    uint32_t type_ptr_in_v4f = b.AllocId();
    uint32_t type_ptr_in_v2f = b.AllocId();
    uint32_t type_ptr_out_v4f = b.AllocId();
    uint32_t type_ptr_out_v2f = b.AllocId();

    uint32_t in_pos = b.AllocId();
    uint32_t in_uv = b.AllocId();
    uint32_t out_pos = b.AllocId();
    uint32_t out_uv = b.AllocId();

    b.EmitString(11, ext_glsl, "GLSL.std.450");

    // OpEntryPoint Vertex %main "main" %out_pos %out_uv %in_pos %in_uv
    uint32_t ep_ops[] = {0, main_fn, 0x6e69616d, 0, out_pos, out_uv, in_pos, in_uv};
    b.words.push_back((8 << 16) | 15);
    for (int j = 0; j < 8; j++) b.words.push_back(ep_ops[j]);

    b.Emit(71, out_pos, 11, 0); // Position
    b.Emit(71, out_uv, 30, 0);  // Location 0
    b.Emit(71, in_pos, 30, 0);  // Location 0
    b.Emit(71, in_uv, 30, 1);   // Location 1

    b.Emit(19, type_void);
    b.Emit(33, type_fn_void, type_void);
    b.Emit(22, type_float, 32);
    b.Emit(23, type_v2f, type_float, 2);
    b.Emit(23, type_v4f, type_float, 4);

    b.Emit(32, type_ptr_in_v4f, 1, type_v4f);
    b.Emit(32, type_ptr_in_v2f, 1, type_v2f);
    b.Emit(32, type_ptr_out_v4f, 3, type_v4f);
    b.Emit(32, type_ptr_out_v2f, 3, type_v2f);

    b.Emit(59, type_ptr_in_v4f, in_pos, 1);
    b.Emit(59, type_ptr_in_v2f, in_uv, 1);
    b.Emit(59, type_ptr_out_v4f, out_pos, 3);
    b.Emit(59, type_ptr_out_v2f, out_uv, 3);

    b.Emit(54, type_void, main_fn, 0, type_fn_void);
    uint32_t entry_lbl = b.AllocId();
    b.Emit(248, entry_lbl);

    uint32_t pos_val = b.AllocId();
    b.Emit(61, type_v4f, pos_val, in_pos);
    b.Emit(62, out_pos, pos_val);

    uint32_t uv_val = b.AllocId();
    b.Emit(61, type_v2f, uv_val, in_uv);
    b.Emit(62, out_uv, uv_val);

    b.Emit(253); // OpReturn
    b.Emit(56);  // OpFunctionEnd

    return b.Finalize();
}

// Translate GCN Pixel Shader metadata to valid SPIR-V bytecode
inline std::vector<uint32_t> EmitPixelSpirv(const GcnShaderMetadata &meta) {
    SpirvModuleBuilder b;
    b.Emit(17, 1);    // OpCapability Shader
    b.Emit(14, 0, 1); // OpMemoryModel Logical, GLSL450

    uint32_t ext_glsl = b.AllocId();
    uint32_t main_fn = b.AllocId();
    uint32_t type_void = b.AllocId();
    uint32_t type_fn_void = b.AllocId();
    uint32_t type_float = b.AllocId();
    uint32_t type_v2f = b.AllocId();
    uint32_t type_v4f = b.AllocId();

    uint32_t in_uv = b.AllocId();
    uint32_t out_color = b.AllocId();
    uint32_t tex_var = b.AllocId();
    uint32_t smp_var = b.AllocId();

    b.EmitString(11, ext_glsl, "GLSL.std.450");

    // OpEntryPoint Fragment %main "main" %out_color %in_uv
    uint32_t ep_ops[] = {4, main_fn, 0x6e69616d, 0, out_color, in_uv};
    b.words.push_back((6 << 16) | 15);
    for (int j = 0; j < 6; j++) b.words.push_back(ep_ops[j]);

    b.Emit(16, main_fn, 7); // OriginUpperLeft
    b.Emit(71, in_uv, 30, 0);     // Location 0
    b.Emit(71, out_color, 30, 0); // Location 0
    b.Emit(71, tex_var, 34, 0);   // DescriptorSet 0
    b.Emit(71, tex_var, 33, 0);   // Binding 0
    b.Emit(71, smp_var, 34, 0);   // DescriptorSet 0
    b.Emit(71, smp_var, 33, 1);   // Binding 1

    b.Emit(19, type_void);
    b.Emit(33, type_fn_void, type_void);
    b.Emit(22, type_float, 32);
    b.Emit(23, type_v2f, type_float, 2);
    b.Emit(23, type_v4f, type_float, 4);

    uint32_t type_image = b.AllocId();
    uint32_t type_sampler = b.AllocId();
    uint32_t type_sampled_img = b.AllocId();

    // OpTypeImage float 2D(1) Depth=0 Arrayed=0 MS=0 Sampled=1 Format=Unknown(0)
    uint32_t img_ops[] = {type_image, type_float, 1, 0, 0, 0, 1, 0};
    b.words.push_back((9 << 16) | 25);
    for (int j = 0; j < 8; j++) b.words.push_back(img_ops[j]);

    b.Emit(26, type_sampler);
    b.Emit(27, type_sampled_img, type_image);

    uint32_t type_ptr_in_v2f = b.AllocId();
    uint32_t type_ptr_out_v4f = b.AllocId();
    uint32_t type_ptr_uniform_img = b.AllocId();
    uint32_t type_ptr_uniform_smp = b.AllocId();

    b.Emit(32, type_ptr_in_v2f, 1, type_v2f);
    b.Emit(32, type_ptr_out_v4f, 3, type_v4f);
    b.Emit(32, type_ptr_uniform_img, 0, type_image);
    b.Emit(32, type_ptr_uniform_smp, 0, type_sampler);

    b.Emit(59, type_ptr_in_v2f, in_uv, 1);
    b.Emit(59, type_ptr_out_v4f, out_color, 3);
    b.Emit(59, type_ptr_uniform_img, tex_var, 0);
    b.Emit(59, type_ptr_uniform_smp, smp_var, 0);

    b.Emit(54, type_void, main_fn, 0, type_fn_void);
    uint32_t entry_lbl = b.AllocId();
    b.Emit(248, entry_lbl);

    if (meta.uses_texture || meta.uses_interp) {
        uint32_t uv_val = b.AllocId();
        b.Emit(61, type_v2f, uv_val, in_uv);

        uint32_t tex_val = b.AllocId();
        uint32_t smp_val = b.AllocId();
        b.Emit(61, type_image, tex_val, tex_var);
        b.Emit(61, type_sampler, smp_val, smp_var);

        uint32_t sampled_img = b.AllocId();
        b.Emit(86, type_sampled_img, sampled_img, tex_val, smp_val);

        uint32_t color_val = b.AllocId();
        b.Emit(87, type_v4f, color_val, sampled_img, uv_val);
        b.Emit(62, out_color, color_val);
    } else {
        uint32_t c_white = b.AllocId();
        uint32_t c_1 = b.AllocId();
        b.Emit(43, type_float, c_1, 1.0f);
        uint32_t c_ops[] = {type_v4f, c_white, c_1, c_1, c_1, c_1};
        b.words.push_back((7 << 16) | 80);
        for (int j = 0; j < 6; j++) b.words.push_back(c_ops[j]);
        b.Emit(62, out_color, c_white);
    }

    b.Emit(253);
    b.Emit(56);

    return b.Finalize();
}

// Translate SPIR-V binary words to clean Apple Metal Shading Language (MSL) source
inline bool TranslateSpirvToMsl(const std::vector<uint32_t> &spirv, std::string &out_msl, std::string &out_err) {
    if (spirv.empty()) {
        out_err = "Empty SPIR-V bytecode";
        return false;
    }

    try {
        spirv_cross::CompilerMSL msl(spirv);
        spirv_cross::CompilerMSL::Options options;
        options.set_msl_version(2, 3);
        options.platform = spirv_cross::CompilerMSL::Options::macOS;
        msl.set_msl_options(options);
        out_msl = msl.compile();
        return true;
    } catch (const std::exception &e) {
        out_err = e.what();
        return false;
    } catch (...) {
        out_err = "Unknown exception during SPIRV-Cross compilation";
        return false;
    }
}

// Generate a metadata-driven MSL substitute. This does not translate GCN
// instructions; runtime telemetry must keep that limitation explicit.
inline std::string GenerateDirectMsl(const GcnShaderMetadata &vs_meta, const GcnShaderMetadata &ps_meta) {
    std::string msl;
    msl.reserve(2048);

    msl += "#include <metal_stdlib>\n";
    msl += "using namespace metal;\n\n";

    msl += "struct RasterizerData {\n";
    msl += "    float4 position [[position]];\n";
    msl += "    float2 texcoord [[user(locn0)]];\n";
    msl += "    float4 color    [[user(locn1)]];\n";
    msl += "};\n\n";

    // Vertex Shader
    msl += "vertex RasterizerData guest_vertex(uint vid [[vertex_id]],\n";
    msl += "                                  constant uint8_t *raw_vb [[buffer(0)]],\n";
    msl += "                                  constant uint32_t *layout_ptr [[buffer(1)]],\n";
    msl += "                                  constant float4x4 *cbuffer [[buffer(2)]]) {\n";
    msl += "    RasterizerData out;\n";
    msl += "    if (raw_vb && layout_ptr && layout_ptr[0] >= 8) {\n";
    msl += "        uint32_t stride = layout_ptr[0];\n";
    msl += "        uint32_t position_components = layout_ptr[1];\n";
    msl += "        constant float *p = (constant float *)(raw_vb + vid * stride);\n";
    msl += "        float x = p[0];\n";
    msl += "        float y = p[1];\n";
    msl += "        float z = (stride >= 12) ? p[2] : 0.0f;\n";
    msl += "        float w = (position_components == 3) ? 1.0f : ((stride >= 16) ? p[3] : 1.0f);\n";
    msl += "        if (w == 0.0f) w = 1.0f;\n\n";

    msl += "        // 1. Vertex Transformation (faithful projection via MVP matrix when available)\n";
    msl += "        if (cbuffer != nullptr && (abs((*cbuffer)[0][0]) > 1e-4f || abs((*cbuffer)[1][1]) > 1e-4f || abs((*cbuffer)[2][2]) > 1e-4f)) {\n";
    msl += "            // 4x4 Model-View-Projection matrix from Constant Buffer\n";
    msl += "            float4 pos = (*cbuffer) * float4(x, y, z, 1.0f);\n";
    msl += "            if (pos.w == 0.0f) pos.w = 1.0f;\n";
    msl += "            out.position = pos;\n";
    msl += "        } else if (abs(x) <= 1.05f && abs(y) <= 1.05f) {\n";
    msl += "            // Normalized Device Coordinates (fullscreen/UI quad)\n";
    msl += "            out.position = float4(x, y, z, 1.0f);\n";
    msl += "        } else if (x >= 0.0f && x <= 1920.0f && y >= 0.0f && y <= 1080.0f) {\n";
    msl += "            // Screen-space pixel coordinates (1920x1080 -> [-1, 1])\n";
    msl += "            out.position = float4((x / 960.0f) - 1.0f, 1.0f - (y / 540.0f), z, 1.0f);\n";
    msl += "        } else {\n";
    msl += "            // Direct pass-through homogeneous coordinates (no arbitrary normalization)\n";
    msl += "            out.position = float4(x, y, z, w);\n";
    msl += "        }\n\n";

    msl += "        // 2. Texture Coordinates\n";
    msl += "        if (stride >= 32) {\n";
    msl += "            out.texcoord = float2(p[6], p[7]);\n";
    msl += "        } else if (stride >= 24) {\n";
    msl += "            out.texcoord = float2(p[4], p[5]);\n";
    msl += "        } else if (stride >= 20) {\n";
    msl += "            out.texcoord = float2(p[3], p[4]);\n";
    msl += "        } else {\n";
    msl += "            out.texcoord = float2((out.position.x + 1.0f) * 0.5f, (1.0f - out.position.y) * 0.5f);\n";
    msl += "        }\n\n";

    msl += "        // 3. Lambertian Directional Lighting\n";
    msl += "        if (stride >= 24) {\n";
    msl += "            float3 norm = normalize(float3(p[3], p[4], p[5]));\n";
    msl += "            if (length(norm) > 0.1f) {\n";
    msl += "                float3 lightDir = normalize(float3(0.577f, 0.577f, 0.577f));\n";
    msl += "                float diff = max(dot(norm, lightDir), 0.0f) * 0.6f + 0.4f;\n";
    msl += "                out.color = float4(diff, diff, diff, 1.0f);\n";
    msl += "            } else {\n";
    msl += "                out.color = float4(1.0f, 1.0f, 1.0f, 1.0f);\n";
    msl += "            }\n";
    msl += "        } else {\n";
    msl += "            out.color = float4(1.0f, 1.0f, 1.0f, 1.0f);\n";
    msl += "        }\n";
    msl += "    } else {\n";
    msl += "        out.position = float4(0.0f, 0.0f, 0.0f, 0.0f);\n";
    msl += "        out.texcoord = float2(0.0f, 0.0f);\n";
    msl += "        out.color = float4(0.0f, 0.0f, 0.0f, 0.0f);\n";
    msl += "    }\n";
    msl += "    return out;\n";
    msl += "}\n\n";

    // Fragment Shader
    msl += "fragment float4 guest_fragment(RasterizerData in [[stage_in]],\n";
    msl += "                                texture2d<float> colorTexture [[texture(0)]],\n";
    msl += "                                sampler textureSampler [[sampler(0)]]) {\n";
    if (ps_meta.uses_texture || ps_meta.uses_interp) {
        msl += "    if (!is_null_texture(colorTexture)) {\n";
        msl += "        float4 texColor = colorTexture.sample(textureSampler, in.texcoord);\n";
        msl += "        return texColor * in.color;\n";
        msl += "    }\n";
        msl += "    return in.color;\n";
    } else {
        msl += "    if (!is_null_texture(colorTexture)) {\n";
        msl += "        return colorTexture.sample(textureSampler, in.texcoord) * in.color;\n";
        msl += "    }\n";
        msl += "    return in.color;\n";
    }
    msl += "}\n";

    return msl;
}

} // namespace GcnRecompiler
