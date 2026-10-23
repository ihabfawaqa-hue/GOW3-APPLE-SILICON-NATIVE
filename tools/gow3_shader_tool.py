#!/usr/bin/env python3
"""Experimental GCN opcode-pattern scanner and placeholder code emitter.

This does not decode GCN instruction semantics and does not produce a usable
GoW3 shader translation. Generated SPIR-V/MSL is structural scaffolding only.
"""

import os
import sys
import struct
import json
import argparse

class SpirvBuilder:
    def __init__(self):
        self.words = []
        self.bound = 1

    def alloc_id(self):
        id_ = self.bound
        self.bound += 1
        return id_

    def emit(self, opcode, *operands):
        flat_ops = []
        for op in operands:
            if isinstance(op, int):
                flat_ops.append(op & 0xFFFFFFFF)
            elif isinstance(op, float):
                flat_ops.append(struct.unpack('<I', struct.pack('<f', op))[0])
            elif isinstance(op, str):
                b = op.encode('utf-8') + b'\x00'
                while len(b) % 4 != 0:
                    b += b'\x00'
                for i in range(0, len(b), 4):
                    flat_ops.append(struct.unpack('<I', b[i:i+4])[0])
            elif isinstance(op, list):
                flat_ops.extend(op)
        length = len(flat_ops) + 1
        self.words.append((length << 16) | opcode)
        self.words.extend(flat_ops)

    def get_binary(self):
        header = [0x07230203, 0x00010000, 0x00000000, self.bound, 0x00000000]
        return struct.pack(f'<{len(header) + len(self.words)}I', *(header + self.words))

def analyze_gcn_bytecode(gcn_bytes, stage_hint="vertex"):
    """
    Scans a few opcode patterns for rough stage/resource hints; not a decoder.
    """
    if len(gcn_bytes) < 4:
        return None

    words = list(struct.unpack(f'<{len(gcn_bytes)//4}I', gcn_bytes[:(len(gcn_bytes)//4)*4]))
    metadata = {
        "stage": stage_hint,
        "code_size_bytes": len(words) * 4,
        "instruction_count": 0,
        "has_position_export": False,
        "param_exports_count": 0,
        "has_mrt0": False,
        "uses_texture": False,
        "uses_interp": False,
        "uses_cbuffer": False,
        "unsupported_opcodes": []
    }

    pc = 0
    while pc < len(words):
        w1 = words[pc]
        metadata["instruction_count"] += 1

        if w1 == 0xbf810000:  # s_endpgm
            metadata["code_size_bytes"] = (pc + 1) * 4
            break

        # EXP instruction (64-bit): 0xf8...
        if (w1 & 0xfe000000) == 0xf8000000:
            tgt = (w1 >> 4) & 0x3f
            if 12 <= tgt <= 15:
                metadata["has_position_export"] = True
                metadata["stage"] = "vertex"
            elif 32 <= tgt <= 63:
                metadata["param_exports_count"] += 1
            elif tgt == 0:
                metadata["has_mrt0"] = True
                metadata["stage"] = "pixel"
            pc += 2
            continue

        # MIMG instruction (image_sample): 0xf0...
        if (w1 & 0xf8000000) == 0xf0000000:
            metadata["uses_texture"] = True
            pc += 2
            continue

        # VINTRP instruction: 0xc8...
        if (w1 & 0xfc000000) == 0xc8000000:
            metadata["uses_interp"] = True
            pc += 1
            continue

        # SMEM constant load: 0xc0...
        if (w1 & 0xf8000000) == 0xc0000000:
            metadata["uses_cbuffer"] = True
            pc += 1
            continue

        # Standard ALU instruction
        pc += 1

    return metadata

def translate_to_spirv(metadata):
    """
    Emits a generic stage template from metadata; not a translation of GCN code.
    """
    b = SpirvBuilder()
    b.emit(17, 1)     # OpCapability Shader
    b.emit(14, 0, 1)  # OpMemoryModel Logical, GLSL450

    ext_glsl = b.alloc_id()
    main_fn = b.alloc_id()
    type_void = b.alloc_id()
    type_fn_void = b.alloc_id()
    type_float = b.alloc_id()
    type_v2f = b.alloc_id()
    type_v4f = b.alloc_id()

    b.emit(11, ext_glsl, "GLSL.std.450")

    if metadata["stage"] == "vertex":
        in_pos = b.alloc_id()
        in_uv = b.alloc_id()
        out_pos = b.alloc_id()
        out_uv = b.alloc_id()

        # OpEntryPoint Vertex %main "main" %out_pos %out_uv %in_pos %in_uv
        b.words.append((8 << 16) | 15)
        b.words.extend([0, main_fn, 0x6e69616d, 0, out_pos, out_uv, in_pos, in_uv])

        b.emit(71, out_pos, 11, 0) # Position
        b.emit(71, out_uv, 30, 0)  # Location 0
        b.emit(71, in_pos, 30, 0)  # Location 0
        b.emit(71, in_uv, 30, 1)   # Location 1

        b.emit(19, type_void)
        b.emit(33, type_fn_void, type_void)
        b.emit(22, type_float, 32)
        b.emit(23, type_v2f, type_float, 2)
        b.emit(23, type_v4f, type_float, 4)

        type_ptr_in_v4f = b.alloc_id()
        type_ptr_in_v2f = b.alloc_id()
        type_ptr_out_v4f = b.alloc_id()
        type_ptr_out_v2f = b.alloc_id()

        b.emit(32, type_ptr_in_v4f, 1, type_v4f)
        b.emit(32, type_ptr_in_v2f, 1, type_v2f)
        b.emit(32, type_ptr_out_v4f, 3, type_v4f)
        b.emit(32, type_ptr_out_v2f, 3, type_v2f)

        b.emit(59, type_ptr_in_v4f, in_pos, 1)
        b.emit(59, type_ptr_in_v2f, in_uv, 1)
        b.emit(59, type_ptr_out_v4f, out_pos, 3)
        b.emit(59, type_ptr_out_v2f, out_uv, 3)

        b.emit(54, type_void, main_fn, 0, type_fn_void)
        entry_lbl = b.alloc_id()
        b.emit(248, entry_lbl)

        pos_val = b.alloc_id()
        b.emit(61, type_v4f, pos_val, in_pos)
        b.emit(62, out_pos, pos_val)

        uv_val = b.alloc_id()
        b.emit(61, type_v2f, uv_val, in_uv)
        b.emit(62, out_uv, uv_val)

        b.emit(253) # OpReturn
        b.emit(56)  # OpFunctionEnd

    else:
        in_uv = b.alloc_id()
        out_color = b.alloc_id()
        tex_var = b.alloc_id()
        smp_var = b.alloc_id()

        # OpEntryPoint Fragment %main "main" %out_color %in_uv
        b.words.append((6 << 16) | 15)
        b.words.extend([4, main_fn, 0x6e69616d, 0, out_color, in_uv])

        b.emit(16, main_fn, 7)     # OriginUpperLeft
        b.emit(71, in_uv, 30, 0)     # Location 0
        b.emit(71, out_color, 30, 0) # Location 0
        b.emit(71, tex_var, 34, 0)   # DescriptorSet 0
        b.emit(71, tex_var, 33, 0)   # Binding 0
        b.emit(71, smp_var, 34, 0)   # DescriptorSet 0
        b.emit(71, smp_var, 33, 1)   # Binding 1

        b.emit(19, type_void)
        b.emit(33, type_fn_void, type_void)
        b.emit(22, type_float, 32)
        b.emit(23, type_v2f, type_float, 2)
        b.emit(23, type_v4f, type_float, 4)

        type_image = b.alloc_id()
        type_sampler = b.alloc_id()
        type_sampled_img = b.alloc_id()

        b.words.append((9 << 16) | 25) # OpTypeImage
        b.words.extend([type_image, type_float, 1, 0, 0, 0, 1, 0])
        b.emit(26, type_sampler)
        b.emit(27, type_sampled_img, type_image)

        type_ptr_in_v2f = b.alloc_id()
        type_ptr_out_v4f = b.alloc_id()
        type_ptr_uniform_img = b.alloc_id()
        type_ptr_uniform_smp = b.alloc_id()

        b.emit(32, type_ptr_in_v2f, 1, type_v2f)
        b.emit(32, type_ptr_out_v4f, 3, type_v4f)
        b.emit(32, type_ptr_uniform_img, 0, type_image)
        b.emit(32, type_ptr_uniform_smp, 0, type_sampler)

        b.emit(59, type_ptr_in_v2f, in_uv, 1)
        b.emit(59, type_ptr_out_v4f, out_color, 3)
        b.emit(59, type_ptr_uniform_img, tex_var, 0)
        b.emit(59, type_ptr_uniform_smp, smp_var, 0)

        b.emit(54, type_void, main_fn, 0, type_fn_void)
        entry_lbl = b.alloc_id()
        b.emit(248, entry_lbl)

        uv_val = b.alloc_id()
        b.emit(61, type_v2f, uv_val, in_uv)
        tex_val = b.alloc_id()
        smp_val = b.alloc_id()
        b.emit(61, type_image, tex_val, tex_var)
        b.emit(61, type_sampler, smp_val, smp_var)
        sampled_img = b.alloc_id()
        b.emit(86, type_sampled_img, sampled_img, tex_val, smp_val)
        color_val = b.alloc_id()
        b.emit(87, type_v4f, color_val, sampled_img, uv_val)
        b.emit(62, out_color, color_val)

        b.emit(253)
        b.emit(56)

    return b.get_binary()

def generate_metal_source(metadata):
    """
    Generates heuristic placeholder MSL; it does not correspond to GCN semantics.
    """
    if metadata["stage"] == "vertex":
        return """#include <metal_stdlib>
using namespace metal;

struct RasterizerData {
    float4 position [[position]];
    float2 texcoord [[user(locn0)]];
    float4 color    [[user(locn1)]];
};

vertex RasterizerData guest_vertex(uint vid [[vertex_id]],
                                  constant uint8_t *raw_vb [[buffer(0)]],
                                  constant uint32_t *stride_ptr [[buffer(1)]],
                                  constant float4x4 *cbuffer [[buffer(2)]]) {
    RasterizerData out;
    if (raw_vb && stride_ptr && *stride_ptr >= 8) {
        uint32_t stride = *stride_ptr;
        constant float *p = (constant float *)(raw_vb + vid * stride);
        float x = p[0];
        float y = p[1];
        float z = (stride >= 12) ? p[2] : 0.0f;
        float w = (stride >= 16) ? p[3] : 1.0f;
        if (w == 0.0f) w = 1.0f;

        if (cbuffer != nullptr && (abs((*cbuffer)[0][0]) > 1e-4f || abs((*cbuffer)[1][1]) > 1e-4f)) {
            float4 pos = (*cbuffer) * float4(x, y, z, 1.0f);
            if (pos.w == 0.0f) pos.w = 1.0f;
            out.position = pos;
        } else if (abs(x) <= 1.05f && abs(y) <= 1.05f) {
            out.position = float4(x, y, z, 1.0f);
        } else if (x >= 0.0f && x <= 1920.0f && y >= 0.0f && y <= 1080.0f) {
            out.position = float4((x / 960.0f) - 1.0f, 1.0f - (y / 540.0f), z, 1.0f);
        } else {
            out.position = float4(x, y, z, w);
        }

        if (stride >= 24) {
            out.texcoord = float2(p[4], p[5]);
        } else {
            out.texcoord = float2((out.position.x + 1.0f) * 0.5f, (1.0f - out.position.y) * 0.5f);
        }
        out.color = float4(1.0f, 1.0f, 1.0f, 1.0f);
    } else {
        out.position = float4(0.0f, 0.0f, 0.0f, 0.0f);
        out.texcoord = float2(0.0f, 0.0f);
        out.color = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    return out;
}
"""
    else:
        return """#include <metal_stdlib>
using namespace metal;

struct RasterizerData {
    float4 position [[position]];
    float2 texcoord [[user(locn0)]];
    float4 color    [[user(locn1)]];
};

fragment float4 guest_fragment(RasterizerData in [[stage_in]],
                                texture2d<float> colorTexture [[texture(0)]],
                                sampler textureSampler [[sampler(0)]]) {
    if (!is_null_texture(colorTexture)) {
        float4 texColor = colorTexture.sample(textureSampler, in.texcoord);
        return texColor * in.color;
    }
    return in.color;
}
"""

def main():
    parser = argparse.ArgumentParser(description="Experimental GCN opcode scanner; emitted code is placeholder only")
    parser.add_argument("--input", "-i", help="Path to input GCN bytecode binary file")
    parser.add_argument("--stage", "-s", choices=["vertex", "pixel"], default="vertex", help="Shader stage hint")
    parser.add_argument("--out-spv", help="Output SPIR-V binary path")
    parser.add_argument("--out-msl", help="Output MSL source path")
    parser.add_argument("--summary", help="Output JSON summary path")
    args = parser.parse_args()

    if not args.input:
        print("Usage: gow3_shader_tool.py --input <shader.bin> [--out-spv <shader.spv>] [--out-msl <shader.metal>]")
        return 0

    with open(args.input, "rb") as f:
        data = f.read()

    meta = analyze_gcn_bytecode(data, stage_hint=args.stage)
    spv_bytes = translate_to_spirv(meta)
    msl_source = generate_metal_source(meta)

    summary = {
        "input": args.input,
        "metadata": meta,
        "spirv_bytes": len(spv_bytes),
        "msl_lines": len(msl_source.splitlines()),
        "status": "HEURISTIC_PLACEHOLDER_NOT_TRANSLATED",
        "gcn_instruction_semantics_decoded": False,
        "shader_translation_verified": False,
    }

    if args.out_spv:
        with open(args.out_spv, "wb") as f:
            f.write(spv_bytes)
        print(f"[+] Wrote SPIR-V binary to {args.out_spv}")

    if args.out_msl:
        with open(args.out_msl, "w", encoding="utf-8") as f:
            f.write(msl_source)
        print(f"[+] Wrote MSL source to {args.out_msl}")

    if args.summary:
        with open(args.summary, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
        print(f"[+] Wrote summary to {args.summary}")

    print(json.dumps(summary, indent=2))
    return 0

if __name__ == "__main__":
    sys.exit(main())
