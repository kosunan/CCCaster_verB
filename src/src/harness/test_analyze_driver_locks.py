import tempfile
import unittest
from pathlib import Path
from analyze_driver_locks import analyze


class DriverLockAnalysis(unittest.TestCase):
    def report(self, text):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'game.log'
            path.write_text(text, encoding='utf-8')
            return analyze(path)

    def test_owner_srw_and_thread_attribution(self):
        report = self.report('[DriverLockModule] base=4096 size=256 path=C:\\driver.dll\n'
            '[DriverLock] f=2 kind=1 tid=10 owner=20 lock=100 caller=4100 begin=600 end=12600\n'
            '[DriverLock] f=2 kind=2 tid=20 owner=0 lock=104 caller=4352 begin=600 end=6600\n'
            '[DrawWork] f=3 tid=10 pid=1 begin=500 end=13000\n')
        a, b = report['events']
        self.assertEqual(a['rva'], 4)
        self.assertEqual(a['acquire_us'], 200)
        self.assertTrue(a['other_owner_observed'])
        self.assertEqual(a['draw_frames'], [3])
        self.assertIsNone(b['module'])
        self.assertFalse(b['other_owner_observed'])
        self.assertEqual(b['draw_frames'], [])

    def test_missing_hook_and_dropped_are_visible(self):
        report = self.report('[DriverLockHook] name=RtlEnterCriticalSection create=0 enable=0\n'
            '[DriverLockDropped] count=3\n[DriverLockDropped] count=4\n')
        self.assertFalse(report['hooks_ok'])
        self.assertEqual(report['dropped'], 7)

    def test_recursive_owner_not_competitor(self):
        report = self.report('[DriverLock] f=2 kind=1 tid=10 owner=10 lock=100 caller=4100 begin=600 end=12600\n')
        self.assertFalse(report['events'][0]['other_owner_observed'])


if __name__ == '__main__':
    unittest.main()
