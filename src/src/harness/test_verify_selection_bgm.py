import random
import unittest
from verify_selection_bgm import compare_pcm


class SelectionBgmTest(unittest.TestCase):
    def setUp(self):
        self.pcm=random.Random(7).randbytes(1800)

    def test_equal_music_with_different_capture_start(self):
        result=compare_pcm(self.pcm[10:],self.pcm[35:],10,35,100)
        self.assertTrue(result['passed'])
        self.assertEqual(result['different_bytes'],0)

    def test_restart_after_first_selection_is_rejected(self):
        replayed=self.pcm[:600]+self.pcm[:1200]
        result=compare_pcm(replayed,self.pcm,0,0,100)
        self.assertFalse(result['passed'])
        self.assertEqual(result['first_difference_seconds'],6)

    def test_skipped_stream_block_is_rejected(self):
        skipped=self.pcm[:600]+self.pcm[800:]
        self.assertFalse(compare_pcm(skipped,self.pcm,0,0,100)['passed'])

    def test_silence_and_short_capture_do_not_pass(self):
        self.assertFalse(compare_pcm(bytes(1500),bytes(1500),0,0,100)['passed'])
        self.assertFalse(compare_pcm(self.pcm[:900],self.pcm[:900],0,0,100)['passed'])

if __name__=='__main__': unittest.main()
