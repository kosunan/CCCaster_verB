import unittest
from analyze_triple_network import analyze_text


class TripleNetworkTest(unittest.TestCase):
    log = ('[NetworkTest] receive delay=150..200 ms loss=20%\n'
           '[NetworkTest] delayed=1000 dropped=251 requestedUs=175000000 observedUs=175300000\n'
           '[NetplaySession] wh=600 tick=16667us correctionParts=0 RTT=350000us peerF=590\n')

    def test_real_hold_and_rtt(self):
        result = analyze_text(self.log)
        self.assertTrue(result['passed'])
        self.assertEqual(result['observed_hold_mean_ms'], 175.3)

    def test_configuration_only_is_not_evidence(self):
        self.assertFalse(analyze_text(self.log.splitlines()[0])['passed'])

    def test_undelayed_callback_fails(self):
        self.assertFalse(analyze_text(self.log.replace('observedUs=175300000', 'observedUs=20000'))['passed'])

    def test_fake_rtt_without_hold_fails(self):
        self.assertFalse(analyze_text('\n'.join([self.log.splitlines()[0], self.log.splitlines()[2]]))['passed'])

    def test_unaffected_rtt_fails(self):
        self.assertFalse(analyze_text(self.log.replace('RTT=350000', 'RTT=2000'))['passed'])

    def test_other_delay_range_fails(self):
        self.assertFalse(analyze_text(self.log.replace('150..200', '50..90'))['passed'])

    ping120_log = ('[NetworkTest] receive delay=60..60 ms loss=0%\n'
                   '[NetworkTest] delayed=1000 dropped=0 requestedUs=60000000 observedUs=60300000\n'
                   '[NetplaySession] wh=600 tick=16667us correctionParts=0 RTT=121000us peerF=590\n')

    def test_ping120_real_hold_and_rtt(self):
        self.assertTrue(analyze_text(self.ping120_log, 0, 60, 60)['passed'])

    def test_ping120_rejects_old_profile(self):
        self.assertFalse(analyze_text(self.log, 0, 60, 60)['passed'])

    def test_ping120_requires_real_hold(self):
        self.assertFalse(analyze_text(self.ping120_log.replace('observedUs=60300000', 'observedUs=20000'), 0, 60, 60)['passed'])

    def test_ping120_requires_delayed_rtt(self):
        self.assertFalse(analyze_text(self.ping120_log.replace('RTT=121000', 'RTT=2000'), 0, 60, 60)['passed'])

    def test_ping120_requires_zero_simulated_loss(self):
        self.assertFalse(analyze_text(self.ping120_log.replace('dropped=0', 'dropped=10'), 0, 60, 60)['passed'])


if __name__ == '__main__':
    unittest.main()
