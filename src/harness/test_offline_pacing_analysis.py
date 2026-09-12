"""共通観測と待機診断のF対応・誤差の分解を検査する。"""
import json
from pathlib import Path
import tempfile
import unittest
from test_legacy_benchmark import recording
from analyze_offline_pacing import summarize


class OfflinePacingAnalysis(unittest.TestCase):
    def fixture(self, folder, omit=None):
        rows, trace = [], []
        for frame in range(1700):
            due = (frame + 10) * 1000000
            late = 1200 if frame == 300 else 0
            tail = 1800 if frame == 300 else 600
            end = due + late + tail
            rows.append((end, frame, 1, 0, 0))
            if frame != omit:
                trace.append(f'[OfflinePacing] f={frame} due={due} waitQpc={due+late} '
                             f'waitAudio={due+late} tailQpc={end} ppm=0')
        (folder/'frames.bin').write_bytes(recording(rows))
        (folder/'cccaster_hook_log.txt').write_text('\n'.join(trace), encoding='utf-8')
        (folder/'result.json').write_text(json.dumps(dict(passed=True, scene={}, files={}, environment={})), encoding='utf-8')

    def test_interval_error_is_difference_of_wait_and_tail_delays(self):
        with tempfile.TemporaryDirectory() as name:
            folder = Path(name)
            self.fixture(folder)
            result = summarize(folder)
            self.assertTrue(result['passed'])
            self.assertEqual(result['matched_frames'], 1501)
            largest = {r['frame']:r for r in result['largest_intervals']}
            self.assertAlmostEqual(largest[300]['interval_error_us'], 40)
            self.assertEqual(largest[300]['delta_wait_us'], 20)
            self.assertEqual(largest[300]['delta_tail_us'], 20)
            self.assertAlmostEqual(largest[301]['interval_error_us'], -40)
            self.assertLess(result['residual_abs_us']['max'], 1e-6)

    def test_missing_correspondence_is_incomplete(self):
        with tempfile.TemporaryDirectory() as name:
            folder = Path(name)
            self.fixture(folder, omit=300)
            result = summarize(folder)
            self.assertFalse(result['passed'])
            self.assertEqual(result['matched_frames'], 1500)


if __name__ == '__main__':
    unittest.main()
