"""判定器自身へデシンク・欠落・破損を注入し、偽の合格を防ぐ。"""
import tempfile
import unittest
from pathlib import Path
from compare_rollback_pair import compare, FIELDS


class ComparatorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.rows = ['[Rollback] BEGIN depth=2']
        self.rows += [f'{tag} {f} ' + ' '.join(['0']*len(fields))
                      for f in range(65537, 65541) for tag, fields in FIELDS.items()]
        self.rows += ['[CONFIRMED] 65540']

    def check(self, other=None, own=None, **kwargs):
        (self.root/'game_1.log').write_text('\n'.join(own or self.rows), encoding='utf-8')
        (self.root/'game_2.log').write_text('\n'.join(self.rows if other is None else other), encoding='utf-8')
        return compare(self.root, min_frames=4, **kwargs)

    def test_equal(self):
        self.assertTrue(self.check()['passed'])

    def test_mutation_has_field_and_values(self):
        rows = [r.replace('[STATE] 65538 0', '[STATE] 65538 123') for r in self.rows]
        result = self.check(rows)
        self.assertFalse(result['passed'])
        self.assertEqual(result['comparisons']['[STATE]']['details'][0]['values'][0],
                         dict(field='rng_hash', host='0', client='123'))

    def test_missing_on_both_sides(self):
        rows = [r for r in self.rows if not r.startswith('[MEM] 65538 ')]
        self.assertFalse(self.check(rows, rows)['passed'])

    def test_unmatched_epoch(self):
        self.assertFalse(self.check(self.rows + ['[CONFIRMED] 131073'])['passed'])

    def test_no_confirmation(self):
        rows = [r for r in self.rows if not r.startswith('[CONFIRMED]')]
        self.assertFalse(self.check(rows, rows)['passed'])

    def test_confirmation_sentinel(self):
        self.assertTrue(self.check(['[CONFIRMED] 131072'] + self.rows)['passed'])

    def test_malformed(self):
        self.assertFalse(self.check(self.rows + ['[MEM] 65539 0'])['passed'])

    def test_final_replay_wins(self):
        rows = ['[STATE] 65538 99 0 0 0 0'] + self.rows
        self.assertTrue(self.check(rows)['passed'])

    def test_unconfirmed_tail_is_excluded(self):
        self.assertTrue(self.check(self.rows + ['[STATE] 65541 99 0 0 0 0'])['passed'])

    def test_no_rollback_is_explicit(self):
        rows = self.rows[1:]
        self.assertFalse(self.check(rows, rows)['passed'])
        self.assertTrue(self.check(rows, rows, require_rollback=False)['passed'])

    def test_failure_or_drop(self):
        for row in ('[SceneRunner] FAILED', '[NumericLogDrop] count=1', '[DeferredTraceDropped] count=1'):
            self.assertFalse(self.check(self.rows + [row])['passed'])

    def test_documented_exclusion_only(self):
        values = ['0'] * 16
        values[3] = values[6] = '42'
        self.assertTrue(self.check(self.rows + ['[MEM] 65538 ' + ' '.join(values)])['passed'])
        values[7] = '7'
        self.assertFalse(self.check(self.rows + ['[MEM] 65538 ' + ' '.join(values)])['passed'])


if __name__ == '__main__':
    unittest.main()
