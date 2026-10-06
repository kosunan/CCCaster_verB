import copy
import unittest

from run_state_sweep import summarize, coverage_gaps, batch_coverage, binary_build_id, CHARACTERS, ALL_COMBINATIONS


class StateSweepReportTests(unittest.TestCase):
    def test_build_identity_rejects_unknown_or_ambiguous_binary(self):
        identity = b'a' * 64
        self.assertEqual(binary_build_id(b'[BOOT_READY] build=' + identity + b' stage=running'), 'a' * 64)
        self.assertEqual(binary_build_id(b'SBCC\x01\x00\x00\x00\x50\x00\x00\x00' + identity + b'\x00', True), 'a' * 64)
        for data in (b'unknown', b'[BOOT_READY] build=' + identity + b' stage=running [BOOT_READY] build=' + b'b'*64 + b' stage=running'):
            with self.assertRaises(ValueError):
                binary_build_id(data)

    def setUp(self):
        self.events = [
            {'event': 'start'},
            {'event': 'definition', 'slot': 0, 'pattern': 100, 'state': 0, 'selected': True},
            {'event': 'catalog_end', 'definitions': 1, 'selected': 1},
            {'event': 'begin_case', 'case': 0, 'slot': 0, 'pattern': 100, 'state': 0},
            {'event': 'result', 'case': 0, 'equal': True, 'different_bytes': 0},
            {'event': 'end', 'status': 'complete', 'defined': 1, 'checked': 1, 'mismatches': 0},
        ]

    def test_finite_scope_never_claims_all_game_states(self):
        result = summarize(self.events)
        self.assertTrue(result['passed'])
        self.assertFalse(result['all_game_states_covered'])

    def test_crash_after_begin_is_not_success(self):
        result = summarize(self.events[:4])
        self.assertFalse(result['passed'])
        self.assertEqual(result['last_case']['pattern'], 100)

    def test_unchecked_definitions_cannot_be_hidden_by_end(self):
        events = copy.deepcopy(self.events)
        events.insert(2, {'event': 'definition', 'slot': 0, 'pattern': 100, 'state': 1, 'selected': True})
        self.assertFalse(summarize(events)['passed'])

    def test_duplicate_results_or_wrong_case_are_rejected(self):
        events = copy.deepcopy(self.events)
        events[4]['case'] = 1
        self.assertFalse(summarize(events)['passed'])
        events = copy.deepcopy(self.events)
        events[3]['state'] = 2
        self.assertFalse(summarize(events)['passed'])

    def test_snapshot_equality_does_not_hide_independent_watch_difference(self):
        events = copy.deepcopy(self.events)
        events.insert(3, {'event': 'watch', 'kind': 'restore', 'address': 0x55512C})
        result = summarize(events)
        self.assertFalse(result['passed'])
        self.assertTrue(result['comparison_complete'])
        self.assertEqual(result['watch_status'], 'needs_review')

    def test_mismatch_or_truncated_log_is_not_success(self):
        events = copy.deepcopy(self.events)
        events[4]['equal'] = False
        self.assertFalse(summarize(events)['passed'])
        self.assertFalse(summarize(self.events + [{'event': 'truncated'}])['passed'])

    def test_catalog_is_not_comparison(self):
        events = copy.deepcopy(self.events[:3]) + [
            {'event': 'end', 'status': 'catalog', 'defined': 1, 'checked': 0}]
        result = summarize(events, catalog=True)
        self.assertTrue(result['passed'])
        self.assertFalse(result['comparison_complete'])
        self.assertFalse(summarize(events)['passed'])


class StateFrameCoverageTests(unittest.TestCase):
    def events(self, ages=(0, 1, 2), entry_only=False):
        rows = [
            {'event': 'start', 'schema': 2, 'entry_only': entry_only},
            {'event': 'definition', 'slot': 0, 'pattern': 1, 'state': 2, 'duration': 3, 'selected': True},
            {'event': 'catalog_end', 'definitions': 1, 'selected': 1, 'nominal_frame_points': 3},
            {'event': 'begin_case', 'case': 0, 'slot': 0, 'pattern': 1, 'state': 2}]
        for frame, age in enumerate(ages):
            rows += [dict(event='frame_begin', case=0, frame=frame, state_age=age),
                     dict(event='result', case=0, frame=frame, state_age=age, equal=True, different_bytes=0)]
        covered = len({age for age in ages if 0 <= age < 3})
        rows += [dict(event='end_case', case=0, frames=len(ages), covered_frame_points=covered,
                      nominal_frame_points=3, reason='entry_only' if entry_only else 'nominal_complete'),
                 dict(event='end', status='complete', defined=1, checked=len(ages), ended_cases=1,
                      mismatches=0, watch_changes=0, nominal_frame_points=3, covered_frame_points=covered)]
        return rows

    def test_counts_states_and_actual_frame_comparisons_separately(self):
        r = summarize(self.events())
        self.assertTrue(r['passed'])
        self.assertEqual((r['selected'], r['checked_frames']), (1, 3))
        self.assertTrue(r['all_selected_nominal_frames_covered'])
        self.assertFalse(r['all_game_states_covered'])

    def test_early_transition_and_repeated_age_do_not_claim_full_coverage(self):
        for ages in ((0,), (0, 0, 0)):
            r = summarize(self.events(ages))
            self.assertFalse(r['passed'])
            self.assertTrue(r['comparison_complete'])
            self.assertEqual(r['missing_frame_points'], 2)

    def test_missing_duplicate_or_invented_frame_results_fail(self):
        for index, key, value in ((5, 'frame', 2), (5, 'state_age', 1), (-1, 'checked', 4),
                                  (-2, 'covered_frame_points', 2)):
            events = self.events()
            events[index][key] = value
            self.assertFalse(summarize(events)['passed'])
        self.assertFalse(summarize(self.events()[:-2])['passed'])

    def test_entry_only_success_is_never_full_frame_success(self):
        r = summarize(self.events((0,), entry_only=True))
        self.assertTrue(r['passed'])
        self.assertFalse(r['all_selected_nominal_frames_covered'])
        self.assertEqual(r['missing_frame_points'], 2)

    def test_pending_sound_watch_difference_is_not_ignored(self):
        events = self.events()
        events.insert(5, dict(event='watch', kind='restore', address=0x76E008))
        self.assertFalse(summarize(events)['passed'])

    def test_missing_watch_record_is_not_success(self):
        events = self.events()
        events[-1]['watch_changes'] = 1
        self.assertFalse(summarize(events)['passed'])

    def test_attempts_union_ages_without_counting_duplicates(self):
        events = self.events((0, 0, 1, 2))
        self.assertTrue(summarize(events)['passed'])
        self.assertEqual(summarize(events)['covered_frame_points'], 3)
        self.assertEqual(coverage_gaps(events)['missing_frame_points'], 0)

    def test_gap_report_keeps_missing_middle_and_end(self):
        gaps = coverage_gaps(self.events((0, 0)))
        self.assertEqual(gaps['gaps'][0]['missing_age_ranges_end_exclusive'], [[1, 3]])
        self.assertEqual(gaps['missing_frame_points'], 2)

    def test_actor_prepared_frames_are_compared_but_not_world_reachability(self):
        events = self.events()
        for e in events:
            if e.get('event') in ('frame_begin', 'result') and e['state_age'] > 0:
                e['frame_source'] = 'native_actor_setup'
        result = summarize(events)
        self.assertTrue(result['passed'])
        self.assertEqual(result['world_update_frame_points'], 1)
        self.assertEqual(result['actor_setup_only_frame_points'], 2)
        self.assertEqual(result['actor_setup_comparisons'], 2)
        self.assertFalse(result['all_frames_reached_by_world_updates'])
        events[5]['frame_source'] = 'native_actor_setup'
        self.assertFalse(summarize(events)['passed'])

    def test_unknown_frame_preparation_or_mismatched_attempt_is_rejected(self):
        for field, value in (('frame_source', 'invented'), ('attempt', 106)):
            events = self.events()
            events[5][field] = value
            self.assertFalse(summarize(events)['passed'])

    def test_all_characters_requires_exact_93_complete_unfiltered_combinations(self):
        cases = [dict(character=c, moon=m, passed=True, all_selected_nominal_frames_covered=True)
                 for c in CHARACTERS for m in range(3)]
        self.assertTrue(batch_coverage(cases, CHARACTERS, range(3))['all_standard_character_motion_frames_covered'])
        self.assertFalse(batch_coverage(cases, CHARACTERS, range(3))['all_character_motion_frames_covered'])
        for bad in (cases[:-1], cases[:-1] + [cases[0]]):
            self.assertFalse(batch_coverage(bad, CHARACTERS, range(3))['all_standard_character_motion_frames_covered'])
        self.assertFalse(batch_coverage(cases, CHARACTERS, range(3), True)['all_standard_character_motion_frames_covered'])
        cases[-1]['all_selected_nominal_frames_covered'] = False
        self.assertFalse(batch_coverage(cases, CHARACTERS, range(3))['all_standard_character_motion_frames_covered'])

    def test_all_characters_includes_10_extra_asset_combinations(self):
        cases = [dict(character=c, moon=m, passed=True, all_selected_nominal_frames_covered=True)
                 for c, m in ALL_COMBINATIONS]
        self.assertEqual(len(cases), 103)
        self.assertTrue(batch_coverage(cases, (), (), combinations=ALL_COMBINATIONS)['all_character_motion_frames_covered'])
        self.assertFalse(batch_coverage(cases[:-1], (), (), combinations=ALL_COMBINATIONS)['all_character_motion_frames_covered'])


if __name__ == '__main__':
    unittest.main()
