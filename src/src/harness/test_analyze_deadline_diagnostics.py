import tempfile
import unittest
from pathlib import Path
from analyze_deadline_diagnostics import analyze, release_metrics


class DeadlineAnalysisTest(unittest.TestCase):
    def test_clamp_does_not_become_a_scheduled_period_change(self):
        lines = [
            '[ReleaseGate] f=1 ready=999880 due=1000000 actual=1000000 readyLate=-120',
            '[ReleaseGate] f=2 ready=2000300 due=2000300 actual=2000306 readyLate=300',
            '[ReleaseGate] f=3 ready=2999900 due=3000000 actual=3000000 readyLate=-100',
            '[UpdateCadence] f=2 prev=1 play=1 consecutive=1 interval=1000306 dropped=0',
            '[UpdateCadence] f=3 prev=2 play=1 consecutive=1 interval=999694 dropped=0',
        ]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'game.log'
            path.write_text('\n'.join(lines), encoding='utf-8')
            report = analyze(path)
        self.assertEqual(report['ready_after_deadline'], 1)
        self.assertEqual(report['clamped_deadlines'], 1)
        self.assertEqual(report['stats']['scheduled_us']['count'], 0)
        self.assertEqual(report['stats']['release_late_us']['count'], 1)
        self.assertIsNone(report['top_late'][0]['release_late_us'])
        self.assertEqual(report['samples'], 2)

    def test_clamped_gate_must_not_hide_a_late_arrival(self):
        r = release_metrics(dict(ready=2000000, due=2000000, actual=2000006, readyLate=300000))
        self.assertTrue(r['deadline_was_clamped'])
        self.assertEqual(r['ready_late_audio_us'], 5000)
        self.assertEqual(r['ready_margin_us'], -5000)
        self.assertEqual(r['gate_exit_late_us'], .1)
        self.assertIsNone(r['release_late_us'])

    def test_schedule_change_is_not_release_lateness(self):
        lines = [
            '[ReleaseGate] f=1 ready=999880 due=1000000 actual=1000060',
            '[ReleaseGate] f=2 ready=2000600 due=2000300 actual=2000660',
            '[UpdateCadence] f=2 prev=1 play=1 consecutive=1 interval=1000600 dropped=0',
        ]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'game.log'
            path.write_text('\n'.join(lines), encoding='utf-8')
            report = analyze(path)
            row = report['top_late'][0]
            self.assertEqual(row['error_us'], 10)
            self.assertEqual(row['late_change_us'], 5)
            self.assertEqual(row['release_late_us'], 6)
            self.assertEqual(report['ready_after_deadline'], 1)
            path.write_text('\n'.join(lines).replace('interval=1000600', 'interval=1000000'), encoding='utf-8')
            with self.assertRaises(ValueError):
                analyze(path)


if __name__ == '__main__':
    unittest.main()
