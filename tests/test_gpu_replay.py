#!/usr/bin/env python3
"""Test the PM4 packet decoder using a synthetic stream; no GPU render occurs."""

import os
import sys
import json
import tempfile

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(BASE_DIR, "tools"))

from gow3_gpu_replay import Gow3GpuReplayer

def test_deterministic_gpu_replay():
    print("[1] Building a synthetic PM4 packet-decoder fixture (not a guest capture)...")
    # PM4 Type 3 packet builder helper
    def make_type3_packet(opcode, payload):
        header = (3 << 30) | ((len(payload) - 1) << 16) | (opcode << 8)
        return [header] + payload

    # 1. Packet setting CB_COLOR0_BASE to 0xbbe08000 (GoW3 display buffer)
    # Reg 0x0318 (CB_COLOR0_BASE low), Reg 0x0319 (CB_COLOR0_BASE high)
    pkt_rt0 = make_type3_packet(0x69, [0x0318, 0xbbe08000, 0x00000000])

    # 2. Packet setting non-draining predication
    pkt_pred = make_type3_packet(0x20, [0x00000001, 0x00000000])

    # 3. Two draw packets (DRAW_INDEX_AUTO and DRAW_INDEX_2)
    pkt_draw1 = make_type3_packet(0x2D, [36, 1])
    pkt_draw2 = make_type3_packet(0x27, [120, 0, 0, 1])

    dcb_stream = pkt_rt0 + pkt_pred + pkt_draw1 + pkt_draw2

    manifest = {
        "capture_version": 1,
        "title_id": "CUSA01623",
        "submissions": [
            {
                "submission_id": 1,
                "dcb_words": dcb_stream
            }
        ]
    }

    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump(manifest, f)
        temp_manifest_path = f.name

    try:
        print("[2] Executing Gow3GpuReplayer offline...")
        replayer = Gow3GpuReplayer(temp_manifest_path)
        summary = replayer.run()

        print(f"    [+] Submissions replayed: {summary['total_submissions']}")
        print(f"    [+] Packets decoded: {summary['total_packets_decoded']}")
        print(f"    [+] Draws encoded: {summary['total_draws_encoded']}")
        print(f"    [+] Final Render Target Base: {summary['final_rt0']}")

        assert summary["total_submissions"] == 1
        assert summary["total_packets_decoded"] == 4, f"Expected 4 packets, got {summary['total_packets_decoded']}"
        assert summary["total_draws_encoded"] == 2, f"Expected 2 draws, got {summary['total_draws_encoded']}"
        assert summary["final_rt0"] == "0xbbe08000", f"Expected 0xbbe08000, got {summary['final_rt0']}"
        assert summary["replayer_status"] == "PACKET_DECODE_ONLY_NO_GPU_RENDER"
        assert summary["gpu_render_executed"] is False
        assert summary["shader_translation_executed"] is False
        assert summary["captured_resources_consumed"] is False
        print("    [+] Packet counts and register decoding verified; GPU replay/render remains unimplemented.")

    finally:
        if os.path.exists(temp_manifest_path):
            os.remove(temp_manifest_path)

if __name__ == "__main__":
    print("=" * 60)
    print("  TEST: Synthetic PM4 Packet Decoder (No GPU Render)")
    print("=" * 60)
    test_deterministic_gpu_replay()
    print("=" * 60)
    print("  ALL PACKET DECODER TESTS PASSED (1/1)")
    print("=" * 60)
