import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from compare_rollback_pair import FIELDS
from real_game_checkpoint import clean_environment, evaluate, monitor, snapshot


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.runtime = self.root / 'runtime'
        self.output = self.root / 'output'
        self.output.mkdir()
        self.rows = ['[Rollback] BEGIN depth=2']
        self.rows += [f'{tag} {f} ' + ' '.join(['0'] * len(fields))
                      for f in range(65537, 66537) for tag, fields in FIELDS.items()]
        self.rows += ['[CONFIRMED] 66536']

    def write(self, other=None):
        for side, rows in ((1, self.rows), (2, self.rows if other is None else other)):
            path = self.runtime / f'MBAACC_{side}/cccaster_B/cccaster_hook_log.txt'
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('\n'.join(rows) + '\n', encoding='utf-8')

    def test_stops_on_conditions_and_final_log_is_rechecked(self):
        self.write()
        self.assertEqual(monitor(self.runtime, self.output, {}, 1, .01), 0)
        report = json.loads((self.output / 'checkpoint.json').read_text())
        self.assertEqual(report['stop_reason'], 'conditions_met')
        self.assertEqual(report['attempts'], 1)
        self.write(self.rows + ['[STATE] 65538 123 0 0 0 0'])
        snapshot(self.runtime, self.output, False)
        self.assertFalse(evaluate(self.output, {})['passed'])

    def test_missing_rollback_frames_confirmation_or_state_cannot_finish(self):
        variants = [self.rows[:-1],
                    [r for r in self.rows if not r.startswith('[MEM] 65538 ')],
                    self.rows + ['[STATE] 65538 123 0 0 0 0'],
                    self.rows + ['[CONFIRMED] 131073'],
                    self.rows + ['[SceneRunner] FAILED']]
        for rows in variants:
            with self.subTest(rows=rows[-1]):
                self.write(rows)
                snapshot(self.runtime, self.output, False)
                self.assertFalse(evaluate(self.output, {})['passed'])
        self.rows = self.rows[1:]
        self.write()
        snapshot(self.runtime, self.output, False)
        self.assertFalse(evaluate(self.output, {})['passed'])

    def test_timeout_is_failure_with_evidence(self):
        self.write(self.rows[:-1] + ['[CONFIRMED] 66535'])
        self.assertEqual(monitor(self.runtime, self.output, {}, 0), 1)
        report = json.loads((self.output / 'checkpoint.json').read_text())
        self.assertEqual(report['stop_reason'], 'timeout')
        self.assertFalse(report['passed'])

    def test_partial_tail_is_deferred_but_completed_corruption_fails(self):
        self.write()
        path = self.runtime / 'MBAACC_2/cccaster_B/cccaster_hook_log.txt'
        with path.open('ab') as log:
            log.write(b'[MEM] 66537 0')
        snapshot(self.runtime, self.output, False)
        self.assertTrue(evaluate(self.output, {})['passed'])
        with path.open('ab') as log:
            log.write(b'\n')
        snapshot(self.runtime, self.output, False)
        self.assertFalse(evaluate(self.output, {})['passed'])

    def test_sync_alone_does_not_satisfy_rematch(self):
        self.write()
        snapshot(self.runtime, self.output, False)
        result = evaluate(self.output, dict(scenario='fixed'))
        self.assertTrue(result['sync']['passed'])
        self.assertFalse(result['passed'])

    def test_foreign_session_log_fails_immediately(self):
        self.write(self.rows + ['[InitThread] Starting hook initialization...'] * 2)
        self.assertEqual(monitor(self.runtime, self.output, {}, 60), 1)
        report = json.loads((self.output / 'checkpoint.json').read_text(encoding='utf-8'))
        self.assertEqual(report['stop_reason'], 'invalid_log')
        self.assertEqual(report['attempts'], 1)

    def test_environment_isolated_without_mutating_parent(self):
        with patch.dict('os.environ', {'CCCASTER_TIME_SCALE': '20', 'CCBENCH_FOO': '1', 'PATH': 'ok'}, clear=True):
            self.assertEqual(clean_environment(), {'PATH': 'ok'})
            import os
            self.assertEqual(os.environ['CCCASTER_TIME_SCALE'], '20')


if __name__ == '__main__':
    unittest.main()
