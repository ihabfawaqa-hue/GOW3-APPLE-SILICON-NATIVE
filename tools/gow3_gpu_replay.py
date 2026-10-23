#!/usr/bin/env python3
"""Decode PM4 packet structure from a manifest.

This utility does not replay commands through a GPU backend, compile guest
shaders, fetch captured resources, or render an image. Its result is deliberately
labelled packet-decode-only.
"""

import os
import sys
import json
import struct
import argparse
import hashlib

# PM4 Packet Types
PM4_TYPE_3 = 3

# PM4 Type-3 Liverpool Opcodes
IT_SET_CONTEXT_REG = 0x69
IT_DRAW_INDEX_AUTO = 0x2D
IT_DRAW_INDEX_2 = 0x27
IT_SET_PREDICATION = 0x20
IT_EVENT_WRITE = 0x46
IT_EVENT_WRITE_EOP = 0x47

class Gow3GpuReplayer:
    def __init__(self, manifest_path):
        self.manifest_path = manifest_path
        self.base_dir = os.path.dirname(os.path.abspath(manifest_path))
        with open(manifest_path, "r", encoding="utf-8") as f:
            self.manifest = json.load(f)

        self.context_regs = {}
        self.vs_user_data = [0] * 16
        self.ps_user_data = [0] * 16
        self.decoded_packets = 0
        self.encoded_draws = 0
        self.predication_enabled = False
        self.current_rt0 = 0
        self.current_vb_addr = 0
        self.current_vb_size = 0
        self.current_ib_addr = 0
        self.current_ib_size = 0

    def replay_submission(self, submission):
        """
        Replays one captured submission containing DCB/CCB words.
        """
        sub_id = submission.get("submission_id", 0)
        dcb_words = submission.get("dcb_words", [])

        pc = 0
        while pc < len(dcb_words):
            header = dcb_words[pc]
            pkt_type = (header >> 30) & 0x3
            count = (header >> 16) & 0x3FFF
            opcode = (header >> 8) & 0xFF

            if pkt_type == PM4_TYPE_3:
                payload = dcb_words[pc + 1 : pc + 2 + count]
                self.process_type3_packet(opcode, payload)
                self.decoded_packets += 1
                pc += 2 + count
            else:
                pc += 1

        return {
            "submission_id": sub_id,
            "packets_decoded": self.decoded_packets,
            "draws_encoded": self.encoded_draws,
            "rt0_base": hex(self.current_rt0),
            "status": "PACKET_DECODE_ONLY"
        }

    def process_type3_packet(self, opcode, payload):
        if opcode == IT_SET_CONTEXT_REG:
            if len(payload) >= 2:
                reg_offset = payload[0] & 0xFFFF
                for i in range(1, len(payload)):
                    r = reg_offset + (i - 1)
                    self.context_regs[r] = payload[i]
                    if r == 0x0318:  # CB_COLOR0_BASE (DW[0])
                        self.current_rt0 = (self.current_rt0 & ~0xFFFFFFFF) | payload[i]
                    elif r == 0x0319: # CB_COLOR0_BASE (DW[1])
                        self.current_rt0 = (self.current_rt0 & 0xFFFFFFFF) | ((payload[i] & 0xFF) << 32)

        elif opcode in (IT_DRAW_INDEX_AUTO, IT_DRAW_INDEX_2):
            self.encoded_draws += 1

        elif opcode == IT_SET_PREDICATION:
            # GoW3 adaptation: Non-draining predication update
            if len(payload) >= 1:
                self.predication_enabled = bool(payload[0] & 1)

    def run(self):
        results = []
        for sub in self.manifest.get("submissions", []):
            res = self.replay_submission(sub)
            results.append(res)

        summary = {
            "manifest": self.manifest_path,
            "total_submissions": len(results),
            "total_packets_decoded": self.decoded_packets,
            "total_draws_encoded": self.encoded_draws,
            "final_rt0": hex(self.current_rt0),
            "submissions": results,
            "replayer_status": "PACKET_DECODE_ONLY_NO_GPU_RENDER",
            "gpu_render_executed": False,
            "shader_translation_executed": False,
            "captured_resources_consumed": False,
        }
        return summary

def main():
    parser = argparse.ArgumentParser(description="GOW3 Deterministic GPU Replay Engine")
    parser.add_argument("--manifest", "-m", required=True, help="Path to capture manifest.json")
    parser.add_argument("--out-summary", "-o", help="Path to write replay summary JSON")
    args = parser.parse_args()

    replayer = Gow3GpuReplayer(args.manifest)
    summary = replayer.run()

    if args.out_summary:
        with open(args.out_summary, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)
        print(f"[+] Replay summary written to {args.out_summary}")

    print(json.dumps(summary, indent=2))
    return 0

if __name__ == "__main__":
    sys.exit(main())
