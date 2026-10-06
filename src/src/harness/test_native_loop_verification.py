import unittest
from run_native_loop_verification import summarize, read_samples


class NativeVerificationTest(unittest.TestCase):
    def test_entry_only_is_not_candidate_coverage(self):
        text = '[NativeLoopMask] requested=31 applied=31\n[NativeVerify] sample=120 entries=400 players=8 objects=0 playerHits=3 objectHits=0 live=5 maxLive=8'
        self.assertFalse(summarize([text, text], [31, 31], True)[0]['passed'])

    def test_hit_and_installation_are_required(self):
        text = '[NativeLoopMask] requested=31 applied=31\n[NativeVerify] sample=120 entries=400 players=8 objects=9 playerHits=3 objectHits=2 live=5 maxLive=8'
        self.assertTrue(summarize([text, text], [31, 31], True)[0]['passed'])
        self.assertFalse(summarize([text, text], [0, 31], True)[0]['passed'])
        self.assertFalse(summarize([text.replace('objectHits=2', 'objectHits=0'), text], [31, 31], True)[0]['passed'])

    def test_warmup_and_noncombat_are_excluded(self):
        text = '\n'.join(f'[Pace] f={f} work={w} play={p}' for f, w, p in [(131073, 999, 1), (131600, 40, 1), (131601, 55, 0), (131602, 0, 1)])
        self.assertEqual(read_samples(text), {131600: 40})

    def test_only_common_frames_and_enough_samples(self):
        text = '[NativeLoopMask] requested=31 applied=31\n' + '\n'.join(f'[Pace] f={131600+i} work=40 play=1' for i in range(501))
        result, rows = summarize([text, text.replace('work=40', 'work=30')], [31, 31], False)
        self.assertTrue(result['passed'])
        self.assertEqual(result['median_side2_minus_side1_us'], -10)
        self.assertEqual(len(rows), 501)
        self.assertFalse(summarize([text, text[:1000]], [31, 31], False)[0]['passed'])


if __name__ == '__main__':
    unittest.main()
