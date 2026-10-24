"""Checks for trace accounting and XML reference/encoder correlation errors."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "metal_analysis", Path(__file__).resolve().parents[1] / "scripts/lab/analyze_metal_trace.py")
trace = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(trace)


def table(columns, records):
    return '<trace-query-result><schema>' + ''.join(
        f'<col><mnemonic>{col}</mnemonic></col>' for col in columns
    ) + '</schema>' + ''.join('<row>' + row + '</row>' for row in records) + '</trace-query-result>'


class MetalAnalysisTests(unittest.TestCase):
    def test_overlapping_stages_and_other_process_are_not_added(self):
        self.assertEqual(trace.length([(0, 5), (3, 8), (10, 12)]), 10)
        self.assertEqual(trace.intersection([(0, 8), (10, 12)], [(5, 11)]), 4)

    def test_reference_join_and_missing_label_coverage(self):
        columns = ['start', 'duration', 'state', 'process', 'cmdbuffer-id', 'encoder-id', 'channel-name']
        gpu = table(columns, [
            '<t>0</t><d>1000000</d><s id="1">Active</s><p id="2">ps4_app (7)</p><c id="3">10</c><e id="4">20</e><ch>Vertex</ch>',
            '<t>500000</t><d>1000000</d><s ref="1"/><p ref="2"/><c ref="3"/><e ref="4"/><ch>Fragment</ch>',
            '<t>2000000</t><d>1000000</d><s ref="1"/><p ref="2"/><c>11</c><e>21</e><ch>Compute</ch>',
            '<t>1500000</t><d>500000</d><s ref="1"/><p>WindowServer (8)</p><c>10</c><e>20</e><ch>Fragment</ch>',
        ])
        enc = table(['process', 'cmdbuffer-id', 'encoder-id', 'encoder-label'], [
            '<p>ps4_app (7)</p><c>10</c><e>20</e><label>pass aabb:1920x1080s2:R8G8B8A8Srgb</label>',
            '<p>WindowServer (8)</p><c>11</c><e>21</e><label>wrong process</label>',
        ])
        with tempfile.TemporaryDirectory() as tmp:
            gpu_path, enc_path = Path(tmp)/'gpu.xml', Path(tmp)/'enc.xml'
            gpu_path.write_text(gpu)
            enc_path.write_text(enc)
            result = trace.analyze(gpu_path, enc_path)
        self.assertEqual(result['game_busy_ms'], 2.5)
        self.assertEqual(result['game_inactive_ms'], 0.5)
        self.assertEqual(result['other_gpu_work_during_game_gaps_ms'], 0.5)
        self.assertEqual(result['no_attributed_gpu_work_during_game_gaps_ms'], 0)
        self.assertEqual(result['matched_encoders'], 1)
        self.assertEqual(result['label_coverage_percent_of_game_busy'], 60)
        self.assertEqual(result['top_pass_families'][0]['family'], 'pass 1920x1080s2:R8G8B8A8Srgb')

    def test_object_label_fallback_obeys_time_and_pid(self):
        gpu = table(['start', 'duration', 'state', 'process', 'cmdbuffer-id', 'encoder-id', 'channel-name'], [
            '<t>100</t><d>100</d><s>Active</s><p>ps4_app (7)</p><c>10</c><e>20</e><ch>Fragment</ch>',
        ])
        labels = table(['timestamp', 'pid', 'object-id', 'label'], [
            '<t>0</t><p>7</p><e>20</e><l>pass aa:1920x1080s2:R8G8B8A8Srgb</l>',
            '<t>50</t><p>8</p><e>20</e><l>pass bb:1280x720s1:R8G8B8A8Srgb</l>',
            '<t>300</t><p>7</p><e>20</e><l>pass cc:960x540s1:R8G8B8A8Srgb</l>',
        ])
        with tempfile.TemporaryDirectory() as tmp:
            gpu_path, labels_path = Path(tmp)/'gpu.xml', Path(tmp)/'labels.xml'
            gpu_path.write_text(gpu)
            labels_path.write_text(labels)
            result = trace.analyze(gpu_path, object_labels_path=labels_path)
        self.assertEqual(result['matched_encoders'], 1)
        self.assertEqual(result['label_coverage_percent_of_game_busy'], 100)
        self.assertEqual(result['top_pass_families'][0]['family'], 'pass 1920x1080s2:R8G8B8A8Srgb')


if __name__ == '__main__':
    unittest.main()
