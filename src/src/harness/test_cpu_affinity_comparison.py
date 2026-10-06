import unittest
from run_cpu_affinity_comparison import affinity, summarize, confirmed_metrics


class AffinityComparisonTests(unittest.TestCase):
    def test_peer_shutdown_tail_is_not_part_of_confirmed_battle(self):
        values = {197965: {'abs_error_us': 1}, 197966: {'abs_error_us': 4763},
                  262145: {'abs_error_us': 2}}
        self.assertEqual(confirmed_metrics(values, {'3': 197965}), {197965: {'abs_error_us': 1}})
        self.assertIn(197966, values)  # 原本の終了後スパイクも保持する。

    def test_unpinned_keeps_guard_and_high_priority(self):
        text = ('[GameCpuGuard] tid=7 disabled=0 process=ffff before=ffff exclude=3 chosen=fffc applied=1 error=0\n'
                '[TimingThread] role=game tid=7 MMCSS=1 high=1\n')
        self.assertEqual(affinity(text, 'unpinned')['allowed_cpus'], list(range(2, 16)))
        for invalid in (text.replace('chosen=fffc', 'chosen=400'),
                        text.replace('chosen=fffc', 'chosen=ffff'),
                        text.replace('MMCSS=1', 'MMCSS=0')):
            with self.assertRaises(ValueError):
                affinity(invalid, 'unpinned')

    def test_pinned_requires_successful_single_cpu_auto_pin(self):
        text = ('[GameCpuGuard] tid=7 disabled=0 process=ffff before=ffff exclude=3 chosen=400 applied=1 error=0\n'
                '[GameCpuPin] tid=7 cpu=10 guard=fffc chosen=400 enabled=1 error=0 auto=1\n'
                '[TimingThread] role=game tid=7 MMCSS=1 high=1\n')
        self.assertEqual(affinity(text, 'pinned')['allowed_cpus'], [10])
        with self.assertRaises(ValueError):
            affinity(text.replace('enabled=1', 'enabled=0'), 'pinned')

    def test_interval_error_and_release_late_have_separate_thresholds(self):
        values = [dict(interval_us=16666.6667, abs_error_us=3, late_us=0,
                       ready_margin_us=190, schedule_error_us=3),
                  dict(interval_us=16666.6667, abs_error_us=4, late_us=5,
                       ready_margin_us=-1, schedule_error_us=-1)]
        result = summarize(values)
        self.assertEqual(result['outside_3us'], 1)
        self.assertEqual(result['late_over_3us'], 1)
        self.assertEqual(result['ready_after_deadline'], 1)
        self.assertEqual(result['outside_3us_percent'], 50)


if __name__ == '__main__':
    unittest.main()
