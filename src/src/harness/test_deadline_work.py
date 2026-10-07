import unittest
from analyze_deadline_work import analyze


class DeadlineWorkTests(unittest.TestCase):
    def row(self, **changes):
        values = dict(f=1, worker=1, due=60000, start=60006, done=60012, pub=60018,
                      seen=66000, reader=0, armed=1000)
        values.update(changes)
        return '[DeadlineWork] '+' '.join(f'{k}={v}' for k,v in values.items())

    def test_rejected_workers_and_receiver_do_not_change_selected_lateness(self):
        text = self.row()+'\n[BoundaryWorkStall] due=60000 started=60000 resumed=900000\n'
        text += '[BoundarySlots] f=1 s0=900000 s1=60006 s2=990000 s3=999000\n'
        text += '[InputSpin] f=1 begin=0 end=999000\n'
        result = analyze(text)
        self.assertEqual(result['adopted_complete_late_us']['maximum'], .2)
        self.assertEqual(result['adopted_publication_upper_late_us']['maximum'], .3)
        self.assertEqual(result['receiver_after_completion_us']['maximum'], 99.8)

    def test_selected_lateness_and_fallback_are_not_discarded(self):
        result = analyze(self.row(worker=-1, start=66000, done=66006, pub=66012, seen=66018))
        self.assertEqual(result['adopted_complete_late_us']['maximum'], 100.1)
        self.assertEqual(result['adopted_workers'], {'-1': 1})

    def test_reader_bound_is_explicit(self):
        result = analyze(self.row(reader=1, pub=66000))
        self.assertEqual(result['publication_upper_from_reader'], 1)

    def test_late_submission_is_counted_not_reported_as_precision_success(self):
        result = analyze(self.row(armed=60000))
        self.assertEqual(result['late_submission_or_clamped'], 1)
        self.assertEqual(result['adopted_complete_late_us']['count'], 0)

    def test_missing_or_corrupt_data_is_rejected(self):
        for text in ['', '[DeadlineWork] f=1', self.row(done=59999), self.row(worker=4)]:
            with self.assertRaises(ValueError):
                analyze(text)


if __name__ == '__main__':
    unittest.main()
