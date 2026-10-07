import unittest
from run_boundary_comparison import metric, summarize, boundary_rows, environment


class BoundaryComparisonTests(unittest.TestCase):
    def test_normal_comparison_explicitly_disables_fault_injection(self):
        self.assertEqual(environment(4, 1)['CCCASTER_TEST_BOUNDARY_STALL_1'], '0')
        self.assertEqual(environment(4, 1, True)['CCCASTER_TEST_BOUNDARY_STALL_1'], '1')

    def test_split_and_legacy_logs_keep_full_timestamp(self):
        value = 123456789012345678
        self.assertEqual(boundary_rows(dict(BoundaryRace=[dict(f=1, s0=value)], BoundarySlots=[]))[1]['s0'], value)
        self.assertEqual(boundary_rows(dict(BoundaryRace=[dict(f=1)], BoundarySlots=[dict(f=1, s0=value)]))[1]['s0'], value)

    def sample(self, due=1000):
        return dict(due=due, boundary=due+6, game=due+600, received=due+660,
                    armed=due-12000, covered=4, valid=4, requested=4, workers=4,
                    winner=0, s0=due+6)

    def test_handoff_delay_is_not_hidden_in_game_measurement(self):
        previous = self.sample()
        current = self.sample(1001000)
        current['received'] += 600
        row = metric(dict(actual=current['received']), dict(actual=previous['received']), current, previous)
        self.assertEqual(row['boundary_late_us'], .1)
        self.assertEqual(row['boundary_interval_error_us'], 0)
        self.assertEqual(row['game_interval_error_us'], 10)
        self.assertEqual(row['handoff_us'], 20.9)

    def test_actual_cannot_be_replaced_with_boundary(self):
        current = self.sample()
        with self.assertRaises(ValueError):
            metric(dict(actual=current['boundary']), dict(actual=0), current, current)

    def test_reject_wrong_winner_or_missing_worker(self):
        for field, value in [('s0', 999), ('workers', 3), ('boundary', 999)]:
            current = self.sample()
            current[field] = value
            with self.assertRaises(ValueError):
                metric(dict(actual=current['received']), dict(actual=0), current, self.sample())

    def test_all_stalled_fallback_remains_a_failure_sample(self):
        current = self.sample()
        current.update(boundary=current['game'], winner=-1, valid=0, covered=0)
        row = metric(dict(actual=current['received']), dict(actual=0), current, self.sample())
        self.assertEqual(summarize([row])['over_3us']['boundary_late_us'], 1)
        self.assertEqual(row['helper_won'], 0)


if __name__ == '__main__':
    unittest.main()
