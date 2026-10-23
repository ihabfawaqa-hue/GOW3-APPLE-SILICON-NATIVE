#!/usr/bin/env python3
"""Keep heuristic Metal shader generation distinct from GCN translation."""

import sys
import unittest
from pathlib import Path

LAB_DIR = Path(__file__).resolve().parents[1] / "scripts" / "lab"
sys.path.insert(0, str(LAB_DIR))
import run_iteration  # noqa: E402


class ShaderTelemetryTruthTests(unittest.TestCase):
    def test_heuristic_pipeline_is_not_reported_as_gcn_translation(self):
        metrics = run_iteration.parse_log_metrics(
            "",
            "[ps4-gnm-metal] Heuristic MSL substitute compiled (GCN instructions NOT translated): PSO\n",
        )
        self.assertEqual(metrics["heuristic_msl_pipeline_log_entries"], 1)
        self.assertEqual(metrics["gcn_translation_path"], "BUILT_OPT_IN_NOT_SELECTED")
        self.assertEqual(metrics["gcn_translated_draw_debug_records"], 0)

    def test_legacy_recompile_wording_is_not_counted(self):
        metrics = run_iteration.parse_log_metrics(
            "", "[ps4-gnm-metal] GCN shader recompiled: metadata -> PSO\n"
        )
        self.assertEqual(metrics["heuristic_msl_pipeline_log_entries"], 0)
        self.assertEqual(metrics["gcn_translation_path"], "BUILT_OPT_IN_NOT_SELECTED")

    def test_enabled_msl_path_is_not_mistaken_for_vulkan_backend(self):
        metrics = run_iteration.parse_log_metrics(
            "", "[ps4-gnm-metal] GCN translated shader path: enabled (GOW3_GCN=1)\n"
               "[gcn-debug] draw VS=0x10000 PS=0x20000 vs_id=1 ps_id=2 inputs:\n"
        )
        self.assertEqual(metrics["gcn_translation_path"], "MSL_TRANSLATOR_SELECTED")
        self.assertEqual(metrics["gcn_translated_draw_debug_records"], 1)
        self.assertEqual(metrics["active_backend"], "not-confirmed")

    def test_capture_limit_does_not_mask_first_guest_unsupported_operation(self):
        metrics = run_iteration.parse_log_metrics(
            "",
            "[ps4-gnm-metal] GPU_CAPTURE: draw_largest.json; replay not implemented\n"
            "[ps4-recomp] WARN: Called unimplemented host shim 'shim_sceVideodecQueryResourceInfo'\n",
        )
        self.assertIn("shim_sceVideodecQueryResourceInfo", metrics["first_unsupported_operation"])


if __name__ == "__main__":
    unittest.main()
