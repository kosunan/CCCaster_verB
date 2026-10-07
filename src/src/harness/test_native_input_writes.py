import unittest
from verify_native_input_writes import verify, verify_local, verify_local_disabled


class NativeInputWritesTests(unittest.TestCase):
    def test_retired_local_rollback_rejects_activation_and_missing_initialization(self):
        ready = '[SceneRunner] rollback session initialized role=host\n'
        self.assertTrue(verify_local_disabled(ready)['passed'])
        self.assertFalse(verify_local_disabled('')['passed'])
        for unexpected in ('[LocalInputRollback] ACTIVE mode=1', '[NativeInputWrite] ACTIVE',
                           '[Rollback] BEGIN frame=100', '[LocalInputRollback] END mode=5'):
            self.assertFalse(verify_local_disabled(ready + unexpected)['passed'])

    def rows(self):
        rows = []
        for player, address in ((1, '0055541B'), (2, '00555F17')):
            key = f'frame=100 player={player} address={address} raw=00040010'
            rows += [f'[NativeInputWrite] MISMATCH {key} beforeDir=0 beforeButtons=00000000 beforeRelease=00000000 expectedDir=4 expectedButtons=00001001 expectedRelease=00000000',
                     f'[NativeInputWrite] WRITE {key} direction=4 buttons=00001001 released=00000000 equal=1',
                     f'[NativeInputWrite] CORRECTED {key} direction=4 buttons=00001001 released=00000000']
        return rows + ['[CONFIRMED] 100']

    def test_both_addresses_observed_and_corrected(self):
        self.assertTrue(verify('\n'.join(self.rows()), 1)['passed'])

    def test_input_history_correction_alone_is_insufficient(self):
        self.assertFalse(verify('[PresentRollback] CHECK frame=100 from=99 local=1 remote=1 lead=1 world=300', 1)['passed'])

    def test_training_player_scope_does_not_weaken_offline_requirement(self):
        text = '\n'.join(r for r in self.rows() if 'player=2' not in r)
        self.assertTrue(verify(text, 1, players=(1,))['passed'])
        self.assertFalse(verify(text, 1)['passed'])

    def test_local_replay_must_return_to_same_simulation_time(self):
        text = '\n'.join(self.rows()) + '\n[LocalInputRollback] ACTIVE mode=5 D=2 epoch=1\n'
        self.assertFalse(verify_local(text, 5, minimum=1)['passed'])
        text += '[LocalInputRollback] END mode=5 target=102 world=200 expectedWorld=200'
        self.assertTrue(verify_local(text, 5, minimum=1)['passed'])
        self.assertFalse(verify_local(text.replace('expectedWorld=200', 'expectedWorld=201'), 5, minimum=1)['passed'])
        self.assertFalse(verify_local(text, 1, minimum=1)['passed'])

    def test_missing_confirmed_correction_fails(self):
        rows = [r for r in self.rows() if not ('CORRECTED' in r and 'player=2' in r)]
        result = verify('\n'.join(rows), 1)
        self.assertFalse(result['passed'])
        self.assertEqual(result['missing_count'], 1)

    def test_unbacked_or_wrong_address_correction_fails(self):
        rows = '\n'.join(self.rows())
        self.assertFalse(verify(rows.replace('CORRECTED frame=100 player=2 address=00555F17',
                                             'CORRECTED frame=100 player=2 address=00555F18'), 1)['passed'])
        self.assertFalse(verify(rows.replace('equal=1', 'equal=0'), 1)['passed'])
        self.assertFalse(verify(rows.replace('CORRECTED frame=100 player=1 address=0055541B raw=00040010 direction=4',
                                             'CORRECTED frame=100 player=1 address=0055541B raw=00040010 direction=6'), 1)['passed'])


if __name__ == '__main__':
    unittest.main()
