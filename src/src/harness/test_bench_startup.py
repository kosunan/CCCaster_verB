import argparse
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from bench_startup import diagnostic_environment, protected_files, readiness


class StartupBenchmarkTests(unittest.TestCase):
    def test_comparison_is_isolated_from_parent_diagnostics(self):
        args = argparse.Namespace(variant='baseline', comparison='remaining', profile=False,
                                  verify_io=False, verify_assets=False)
        parent = {'PATH': 'kept', 'CCCASTER_STARTUP_PROFILE': '1',
                  'cccaster_test_network': '1,2,3', 'CCBENCH_FAKE': '1'}
        with patch.dict(os.environ, parent, clear=True):
            before = dict(os.environ)
            baseline = diagnostic_environment(args)
            self.assertEqual(dict(os.environ), before)
            args.variant = 'fast'
            fast = diagnostic_environment(args)
        self.assertEqual(fast, {'PATH': 'kept', 'CCCASTER_STARTUP_TRACE': '1'})
        self.assertEqual(set(baseline) - set(fast),
                         {'CCCASTER_STARTUP_IO_BASELINE', 'CCCASTER_STARTUP_FONTS_BASELINE'})

    def test_readiness_requires_both_paths_and_correct_mode(self):
        mode = '[StartupMode] target=1 mode=20 kind=4112 versus=0'
        display = '[Startup] event=chara_present'
        inputs = '[Startup] event=chara_input'
        self.assertTrue(readiness('\n'.join([mode, display, inputs]), 'training')['ready'])
        for content in [mode + '\n' + display, mode + '\n' + inputs,
                        '\n'.join([mode, display, inputs])]:
            expected = inputs in content and display in content
            self.assertEqual(readiness(content, 'training')['ready'], expected)
            self.assertFalse(readiness(content, 'versus')['ready'])

    def test_restore_comparison_only_disables_persistent_restore(self):
        args = argparse.Namespace(variant='baseline', comparison='restore', profile=False,
                                  verify_io=False, verify_assets=False, verify_restore=False)
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(diagnostic_environment(args), {
                'CCCASTER_STARTUP_TRACE': '1', 'CCCASTER_STARTUP_RESTORE_BASELINE': '1'})
            args.variant = 'fast'
            args.verify_restore = True
            self.assertEqual(diagnostic_environment(args), {
                'CCCASTER_STARTUP_TRACE': '1', 'CCCASTER_STARTUP_RESTORE_VERIFY': '1'})

    def test_protection_detects_contents_and_added_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            side = Path(tmp)
            (side / 'MBAA.exe').write_bytes(b'game')
            (side / 'pad.ini').write_bytes(b'old')
            original = protected_files([side])
            (side / 'pad.ini').write_bytes(b'new')
            self.assertNotEqual(original, protected_files([side]))
            (side / 'pad.ini').write_bytes(b'old')
            (side / 'other.ini').write_bytes(b'added')
            self.assertNotEqual(original, protected_files([side]))


if __name__ == '__main__':
    unittest.main()
