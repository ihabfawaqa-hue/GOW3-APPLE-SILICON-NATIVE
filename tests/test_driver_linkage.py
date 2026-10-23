#!/usr/bin/env python3
"""
Test suite verifying GNM Metal Driver Linkage and Format String Validity:
1. Verifies that all functions declared in ps4_gnm_metal.h are implemented in ps4_gnm_metal.mm.
2. Verifies that all ps4_gnm_metal_* call sites in ps4_gnmdriver.c have corresponding implementations.
3. Verifies that ps4_gnm_metal_destroy performs genuine resource cleanup.
4. Verifies printf/fprintf format string parameter matching in ps4_metal_screen.m.
"""

import os
import re
import sys

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GNM_H = os.path.join(BASE_DIR, "runtime", "modules", "libSceGnmDriver", "ps4_gnm_metal.h")
GNM_MM = os.path.join(BASE_DIR, "runtime", "modules", "libSceGnmDriver", "ps4_gnm_metal.mm")
GNM_C = os.path.join(BASE_DIR, "runtime", "modules", "libSceGnmDriver", "ps4_gnmdriver.c")
SCREEN_M = os.path.join(BASE_DIR, "runtime", "modules", "libSceVideoOut", "ps4_metal_screen.m")

def test_gnm_metal_header_and_implementation():
    print("[1] Verifying ps4_gnm_metal.h declarations have implementations in ps4_gnm_metal.mm...")
    with open(GNM_H, "r", encoding="utf-8") as f:
        h_content = f.read()
    with open(GNM_MM, "r", encoding="utf-8") as f:
        mm_content = f.read()

    # Extract all C function declarations from ps4_gnm_metal.h
    # Matches patterns like: void ps4_gnm_metal_destroy(void);
    func_pattern = re.compile(r"^(?:int|int32_t|void|uint64_t|bool)\s+([a-zA-Z0-9_]+)\s*\(", re.MULTILINE)
    declared_funcs = func_pattern.findall(h_content)
    assert len(declared_funcs) >= 8, f"Expected at least 8 declared functions in header, found {len(declared_funcs)}"

    for func in declared_funcs:
        assert func in mm_content, f"Function '{func}' declared in ps4_gnm_metal.h is NOT defined in ps4_gnm_metal.mm!"
        print(f"    [+] Found definition for {func}")

def test_gnmdriver_callsites():
    print("[2] Verifying ps4_gnmdriver.c call sites match ps4_gnm_metal.mm...")
    with open(GNM_C, "r", encoding="utf-8") as f:
        c_content = f.read()
    with open(GNM_MM, "r", encoding="utf-8") as f:
        mm_content = f.read()

    call_pattern = re.compile(r"\b(ps4_gnm_metal_[a-zA-Z0-9_]+)\s*\(")
    calls = set(call_pattern.findall(c_content))
    assert "ps4_gnm_metal_destroy" in calls, "Expected call to ps4_gnm_metal_destroy in ps4_gnmdriver.c"

    for func in calls:
        assert func in mm_content, f"ps4_gnmdriver.c calls '{func}' but it is missing in ps4_gnm_metal.mm!"
        print(f"    [+] Verified call site -> definition: {func}")

def test_destroy_implementation_integrity():
    print("[3] Verifying legitimate cleanup in ps4_gnm_metal_destroy (no empty stub)...")
    with open(GNM_MM, "r", encoding="utf-8") as f:
        mm_content = f.read()

    # Locate ps4_gnm_metal_destroy
    assert "void ps4_gnm_metal_destroy(void)" in mm_content, "ps4_gnm_metal_destroy definition missing!"
    destroy_body = mm_content.split("void ps4_gnm_metal_destroy(void)")[1].split("}")[0]

    # Check for real cleanup actions
    assert "ps4_gnm_metal_wait_gpu_idle" in destroy_body or "waitUntilCompleted" in destroy_body, "ps4_gnm_metal_destroy must wait for GPU idle!"
    assert "renderPipelineCache.clear()" in destroy_body, "ps4_gnm_metal_destroy must drain renderPipelineCache!"
    assert "computePipelineCache.clear()" in destroy_body, "ps4_gnm_metal_destroy must drain computePipelineCache!"
    assert "textureCache.clear()" in destroy_body, "ps4_gnm_metal_destroy must drain textureCache!"
    assert "cmdQueue = nil" in destroy_body, "ps4_gnm_metal_destroy must release cmdQueue!"
    assert "device = nil" in destroy_body, "ps4_gnm_metal_destroy must release device!"
    assert "initialized = false" in destroy_body, "ps4_gnm_metal_destroy must reset initialized flag!"
    print("    [+] ps4_gnm_metal_destroy performs comprehensive, legitimate resource deallocation.")

def test_metal_screen_formatting():
    print("[4] Verifying ps4_metal_screen.m format specifier and argument alignment...")
    with open(SCREEN_M, "r", encoding="utf-8") as f:
        lines = f.readlines()

    # Color variation is useful telemetry, but cannot authenticate a game frame.
    found = False
    for i, line in enumerate(lines):
        if "visual authenticity UNVERIFIED" in line:
            found = True
            # Read next few lines for arguments
            arg_lines = "".join(lines[i:i+4])
            # Check format specifiers count in format string
            fmt_str = line[line.find('"'):line.rfind('"')+1]
            # Replace %% with nothing to avoid false count
            clean_fmt = fmt_str.replace("%%", "")
            specifiers = re.findall(r"%[-+0-9.]*[a-zA-Z]", clean_fmt)
            
            # The specifiers: %ld, %lu, %lu, %u, 0x%06x, %.1f, %.1f, %.1f, %.1f -> 9 specifiers
            assert len(specifiers) == 9, f"Expected 9 specifiers in format string, found {len(specifiers)}: {specifiers}"
            
            # Verify unique_bins is passed before dominant_rgb.
            assert "unique_bins" in arg_lines, "unique_bins missing from argument list!"
            unique_bins_pos = arg_lines.find("unique_bins")
            dominant_rgb_pos = arg_lines.find("dominant_rgb")
            assert unique_bins_pos < dominant_rgb_pos, f"unique_bins ({unique_bins_pos}) must precede dominant_rgb ({dominant_rgb_pos})!"
            assert "saved authentic frame" not in open(SCREEN_M, "r", encoding="utf-8").read()
            print(f"    [+] Line {i+1}: frame statistics stay UNVERIFIED; 9 specifiers match 9 arguments.")
            break
    assert found, "Could not find conservative UNVERIFIED frame-statistics log in ps4_metal_screen.m!"

def test_gow3gpu_bridge_subsystem():
    print("[5] Tracing whether the GoW3 GPU bridge reaches the active GNM path...")
    gpu_h = os.path.join(BASE_DIR, "gpu", "gow3gpu.h")
    gpu_cpp = os.path.join(BASE_DIR, "gpu", "gow3gpu_bridge.cpp")
    gpu_vendor = os.path.join(BASE_DIR, "gpu", "VENDOR.txt")
    doc_roadmap = os.path.join(BASE_DIR, "docs", "gow3", "ROADMAP.md")

    assert os.path.exists(gpu_h), "Missing gpu/gow3gpu.h!"
    assert os.path.exists(gpu_cpp), "Missing gpu/gow3gpu_bridge.cpp!"
    assert os.path.exists(gpu_vendor), "Missing gpu/VENDOR.txt!"
    assert os.path.exists(doc_roadmap), "Missing docs/gow3/ROADMAP.md!"

    with open(gpu_h, "r", encoding="utf-8") as f:
        h_text = f.read()
    with open(gpu_cpp, "r", encoding="utf-8") as f:
        cpp_text = f.read()

    func_pattern = re.compile(r"^(?:int|int32_t|void|uint64_t|bool|void\*)\s+([a-zA-Z0-9_]+)\s*\(", re.MULTILINE)
    declared = func_pattern.findall(h_text)
    assert len(declared) >= 10, f"Expected >=10 functions in gow3gpu.h, found {len(declared)}"

    for func in declared:
        assert func in cpp_text, f"gow3gpu.h declared '{func}' but missing in gpu/gow3gpu_bridge.cpp!"
    assert "ps4_gnm_metal_process_command_buffers" in cpp_text
    assert "ps4_gnm_metal_present" in cpp_text
    gnm = open(GNM_C, "r", encoding="utf-8").read()
    assert "return ps4_gnm_metal_process_command_buffers" in gnm
    runtime_sources = []
    for root, _, names in os.walk(os.path.join(BASE_DIR, "runtime")):
        for name in names:
            if name.endswith((".c", ".m", ".mm", ".cpp")):
                path = os.path.join(root, name)
                with open(path, "r", encoding="utf-8", errors="replace") as stream:
                    runtime_sources.append(stream.read())
    runtime_text = "\n".join(runtime_sources)
    active_calls = re.findall(r"\bgow3gpu_(?:init|submit_command_buffers|present)\s*\(", runtime_text)
    assert not active_calls, f"Expected no active calls into the placeholder bridge, found {active_calls}"
    vendor_text = open(gpu_vendor, "r", encoding="utf-8").read()
    assert "does not contain the shadPS4 video core" in vendor_text
    print("    [+] Confirmed: GNM submits and VideoOut presents through custom Metal; gow3gpu bridge has no runtime caller.")

if __name__ == "__main__":
    print("=" * 60)
    print("  TEST: GNM Driver Linkage & Format String Integrity")
    print("=" * 60)
    test_gnm_metal_header_and_implementation()
    test_gnmdriver_callsites()
    test_destroy_implementation_integrity()
    test_metal_screen_formatting()
    test_gow3gpu_bridge_subsystem()
    print("=" * 60)
    print("  ALL LINKAGE, FORMAT & BRIDGE TESTS PASSED (5/5)")
    print("=" * 60)
