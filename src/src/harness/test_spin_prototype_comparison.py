import unittest
from collections import defaultdict
from run_spin_prototype_comparison import measure, trial_environment


class SpinComparisonTest(unittest.TestCase):
    def tables(self):
        rows = defaultdict(list)
        rows['BoundaryWorker'] = [dict(cpu=c, active=1) for c in (6, 8, 10, 12)]
        for f in range(65540, 66740):
            rows['UpdateCadence'].append(dict(f=f, play=1, consecutive=1, error=0))
            rows['CaptureStage'].append(dict(f=f, late=0, lock=0))
            rows['DisplayPace'].append(dict(f=f, late=1))
            rows['BoundaryRace'].append(dict(f=f, due=100, boundary=101, game=110, received=120))
            for name in ('PresentRace', 'PublicationRace'):
                rows[name].append(dict(f=f, observed=100, actual=110, workers=4, winner=1))
            rows['CaptureRace'].append(dict(f=f, due=100, captured=101, worker=0))
        return rows

    def test_valid_and_environment(self):
        measured, stats = measure(self.tables(), {'1': 66740}, 'all', False)
        self.assertEqual(len(measured), 1200)
        self.assertEqual(stats['capture']['helper_wins'], 1200)
        env = trial_environment('publication')
        self.assertEqual(env['CCCASTER_SPIN_PUBLICATION_1'], '1')
        self.assertEqual(env['CCCASTER_SPIN_CAPTURE_1'], '0')
        self.assertEqual(env['CCCASTER_TEST_CAPTURE_STALL_1'], '0')
        self.assertNotIn('CCCASTER_SPIN_PRESENT_1', env)

    def test_current_all_does_not_require_removed_present(self):
        rows = self.tables(); rows['PresentRace'] = []
        measured, _ = measure(rows, {'1': 66740}, 'all', False, present_removed=True)
        self.assertEqual(len(measured), 1200)
        rows['PresentRace'].append(dict(f=65540, observed=100, actual=110, workers=4, winner=0))
        with self.assertRaisesRegex(ValueError, '無効化'):
            measure(rows, {'1': 66740}, 'all', False, present_removed=True)
        with self.assertRaisesRegex(ValueError, '撤去済み'):
            trial_environment('present')

    def test_faults_are_not_normal_evidence(self):
        rows = self.tables(); rows['CaptureStall'].append(dict(f=65540))
        with self.assertRaisesRegex(ValueError, '故障注入'):
            measure(rows, {'1': 66740}, 'all', False)

    def test_duplicate_capture(self):
        rows = self.tables(); rows['CaptureRace'].append(rows['CaptureRace'][0])
        with self.assertRaisesRegex(ValueError, '二重採取'):
            measure(rows, {'1': 66740}, 'all', False)

    def test_early_capture(self):
        rows = self.tables(); rows['CaptureRace'][0]['captured'] = 99
        with self.assertRaisesRegex(ValueError, '締切前'):
            measure(rows, {'1': 66740}, 'all', False)

    def test_missing_present_is_not_success(self):
        rows = self.tables(); rows['PresentRace'] = []
        with self.assertRaisesRegex(ValueError, '実通過'):
            measure(rows, {'1': 66740}, 'all', False)

    def test_wrong_cpu(self):
        rows = self.tables(); rows['BoundaryWorker'][0]['cpu'] = 4
        with self.assertRaisesRegex(ValueError, '配置'):
            measure(rows, {'1': 66740}, 'all', False)


if __name__ == '__main__':
    unittest.main()
