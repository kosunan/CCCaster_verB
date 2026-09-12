import unittest
from verify_session_score_pair import compare

FIRST = '[SessionScore] revision=1 match=65536 p1=1 p2=0 unresolved=0 rounds=2/0 required=2\n'
SECOND = '[SessionScore] revision=2 match=327680 p1=1 p2=1 unresolved=0 rounds=1/2 required=2\n'


class ScoreComparisonTests(unittest.TestCase):
    def test_valid_two_matches(self):
        self.assertTrue(compare(FIRST + SECOND, FIRST + SECOND)['passed'])

    def test_missing_results(self):
        self.assertFalse(compare('', '')['passed'])

    def test_duplicate(self):
        self.assertFalse(compare(FIRST + FIRST, FIRST + FIRST)['passed'])

    def test_peer_difference(self):
        self.assertFalse(compare(FIRST, FIRST + SECOND)['passed'])

    def test_incorrect_win_increment(self):
        bad = FIRST.replace('p1=1', 'p1=2')
        self.assertFalse(compare(bad, bad)['passed'])

    def test_unresolved(self):
        draw = FIRST.replace('p1=1', 'p1=0').replace('unresolved=0', 'unresolved=1').replace('rounds=2/0', 'rounds=2/2')
        self.assertTrue(compare(draw, draw)['passed'])
