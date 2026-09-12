import unittest
from verify_selection_pair import validate_load_sequence

C = '[Select] COMMIT p1=0/0/0 p2=11/0/0 stage=46'
L = C.replace('COMMIT', 'LOADED')


class SelectionSequenceTests(unittest.TestCase):
    def test_rounds_and_once_reuse_selection(self):
        self.assertFalse(validate_load_sequence('\n'.join([C, L, L, L])))

    def test_reselection(self):
        self.assertFalse(validate_load_sequence('\n'.join([C, L, C.replace('46', '7'), L.replace('46', '7')])))

    def test_later_load_mutation(self):
        self.assertTrue(validate_load_sequence('\n'.join([C, L, L.replace('46', '7')])))

    def test_missing_load(self):
        self.assertTrue(validate_load_sequence('\n'.join([C, C, L])))
        self.assertTrue(validate_load_sequence(C))

    def test_loaded_before_commit(self):
        self.assertTrue(validate_load_sequence('\n'.join([L, C, L])))

    def test_empty(self):
        self.assertTrue(validate_load_sequence(''))


if __name__ == '__main__':
    unittest.main()
