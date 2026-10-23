#!/usr/bin/env python3
"""Test opcode-pattern scanning and placeholder code generation only.

These small synthetic fixtures do not verify GCN instruction semantics or real
shader translation. The emitted SPIR-V/MSL is structural scaffolding only.
"""

import os
import sys
import struct

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(BASE_DIR, "tools"))

from gow3_shader_tool import analyze_gcn_bytecode, translate_to_spirv, generate_metal_source

def test_vertex_shader_recompilation():
    print("[1] Testing a synthetic vertex-stage opcode-pattern fixture...")
    gcn_vs_words = [
        0xc0020000, 0x00000000, # s_buffer_load_dwordx4
        0x02000200,             # v_add_f32 v0, v0, v1
        0xf80008c0, 0x00000000, # exp pos0, v0, v1, v2, v3 (target 12)
        0xf8000a00, 0x00000000, # exp param0, v4, v5, off, off (target 32)
        0xbf810000              # s_endpgm
    ]
    raw_vs = struct.pack(f'<{len(gcn_vs_words)}I', *gcn_vs_words)

    meta = analyze_gcn_bytecode(raw_vs, stage_hint="vertex")
    assert meta is not None, "Metadata analysis failed!"
    assert meta["stage"] == "vertex", f"Expected stage vertex, got {meta['stage']}"
    assert meta["has_position_export"] is True, "Position export not detected!"
    assert meta["param_exports_count"] == 1, f"Expected 1 param export, got {meta['param_exports_count']}"

    # Translate to SPIR-V
    spv_bytes = translate_to_spirv(meta)
    assert len(spv_bytes) >= 100, f"SPIR-V binary too small: {len(spv_bytes)} bytes"
    magic = struct.unpack('<I', spv_bytes[:4])[0]
    assert magic == 0x07230203, f"Invalid SPIR-V magic: 0x{magic:08x}"

    # Generate MSL
    msl = generate_metal_source(meta)
    assert "vertex RasterizerData guest_vertex" in msl, "Missing guest_vertex function in MSL"
    assert "out.position" in msl, "Missing out.position assignment in MSL"
    print("    [+] Pattern metadata and placeholder output structure checked; shader translation is NOT VERIFIED.")

def test_pixel_shader_recompilation():
    print("[2] Testing a synthetic pixel-stage opcode-pattern fixture...")
    gcn_ps_words = [
        0xc0020000, 0x00000000, # s_buffer_load
        0xf0000000, 0x00000000, # image_sample (MIMG)
        0xf8000800, 0x00000000, # exp mrt0, v0, v1, v2, v3 (target 0)
        0xbf810000              # s_endpgm
    ]
    raw_ps = struct.pack(f'<{len(gcn_ps_words)}I', *gcn_ps_words)

    meta = analyze_gcn_bytecode(raw_ps, stage_hint="pixel")
    assert meta is not None, "Metadata analysis failed!"
    assert meta["stage"] == "pixel", f"Expected stage pixel, got {meta['stage']}"
    assert meta["has_mrt0"] is True, "MRT0 color export not detected!"
    assert meta["uses_texture"] is True, "Texture sample not detected!"

    # Translate to SPIR-V
    spv_bytes = translate_to_spirv(meta)
    assert len(spv_bytes) >= 100, f"SPIR-V binary too small: {len(spv_bytes)} bytes"
    magic = struct.unpack('<I', spv_bytes[:4])[0]
    assert magic == 0x07230203, f"Invalid SPIR-V magic: 0x{magic:08x}"

    # Generate MSL
    msl = generate_metal_source(meta)
    assert "fragment float4 guest_fragment" in msl, "Missing guest_fragment in MSL"
    assert "colorTexture.sample" in msl, "Missing texture sampling in MSL"
    print("    [+] Pattern metadata and placeholder output structure checked; shader translation is NOT VERIFIED.")

if __name__ == "__main__":
    print("=" * 60)
    print("  TEST: GCN Opcode Heuristics & Placeholder Output (Not a Recompiler)")
    print("=" * 60)
    test_vertex_shader_recompilation()
    test_pixel_shader_recompilation()
    print("=" * 60)
    print("  ALL HEURISTIC TESTS PASSED (2/2); shader semantics remain unverified")
    print("=" * 60)
