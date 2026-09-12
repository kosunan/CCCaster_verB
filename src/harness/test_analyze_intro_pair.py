import tempfile
import unittest
from pathlib import Path
from analyze_intro_pair import read


class IntroAnalysisTest(unittest.TestCase):
    def test_replay_does_not_count_as_normal_interval(self):
        lines = '''[FRAME] 131073 4 2 0 0
[INTRO] 131073 2 1 0 1 1 0 1
[UpdateCadence] n=1 f=131073 ticks=1000000
[FRAME] 131074 4 2 1 0
[INTRO] 131074 2 1 1 1 1 0 1
[UpdateCadence] n=2 f=131074 ticks=2000000
[Rollback] BEGIN frame=131073 target=131075 depth=2 forced=1 intro=2 targetIntro=1
[FRAME] 131073 4 2 0 0
[INTRO] 131073 2 1 0 0 0 0 1
[FRAME] 131074 4 2 1 0
[FRAME] 131075 4 1 2 0
[UpdateCadence] n=3 f=131075 ticks=3000240
[FRAME] 196609 4 2 0 0
[UpdateCadence] n=4 f=196609 ticks=4000240
[FRAME] 196611 4 2 2 0
[UpdateCadence] n=6 f=196611 ticks=6000240
'''
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'game.log'
            p.write_text(lines, encoding='utf-8')
            states, rb, cadence = read(p)
        self.assertEqual(states[131073], [2, 1, 0, 0, 0, 0, 1])
        self.assertEqual(rb, {'2->1': 1})
        self.assertEqual(cadence['2']['intervals'], 1)
        self.assertEqual(cadence['2']['max_abs_error_us'], 0)
        self.assertEqual(cadence['2->1']['max_abs_error_us'], 4)
        self.assertEqual(cadence['2->1']['over_3us'], 1)


if __name__ == '__main__':
    unittest.main()
