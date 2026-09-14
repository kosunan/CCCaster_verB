import copy
import unittest
from regression_checks import resumed_frames, baseline_failures


class RegressionChecks(unittest.TestCase):
    def test_previous_rounds_do_not_prove_rematch(self):
        retry = {'sides': [{'resolutions': [{'epoch': 4*65536}]}]*2}
        data = {'confirmed_by_epoch': {'2': 2*65536+718, '3': 3*65536+718}}
        self.assertEqual(resumed_frames(data, retry), 0)
        data['confirmed_by_epoch']['5'] = 5*65536+130
        self.assertEqual(resumed_frames(data, retry), 130)

    def test_performance_regression_and_wrong_conditions(self):
        previous = dict(name='combat_0', case='combat', seconds=45, network='15,25,5', cheats={},
                        comparison={s: {'rollup_p99_us': 1000} for s in ('host', 'client')})
        current = copy.deepcopy(previous)
        self.assertFalse(baseline_failures(current, previous, 1.5))
        current['comparison']['client']['rollup_p99_us'] = 1501
        self.assertTrue(baseline_failures(current, previous, 1.5))
        current['comparison']['client']['rollup_p99_us'] = 0
        self.assertTrue(baseline_failures(current, previous, 1.5))
        current = copy.deepcopy(previous)
        current['network'] = '60,96,5'
        self.assertTrue(baseline_failures(current, previous, 1.5))


if __name__ == '__main__':
    unittest.main()
