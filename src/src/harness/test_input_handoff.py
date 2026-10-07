import unittest
from analyze_input_handoff import analyze


def evidence():
    return '\n'.join([
        '[InputCaptureSend] f=131073 begin=999950 end=1000000 published=1000060',
        '[InputHandoff] f=131073 source=131073 capture=131073 captureDue=2000000 simulationDue=2030000 phaseUs=500',
        '[FramePipeline] f=131073 start=1030000 input=1030060 saved=1033000 prepared=1033100 native=1033400 present=1100000',
        '[FrameStart] f=131073 actual=1030000 due=1030000 readyLate=-12000',
        '[UpdateCadence] f=131073 ticks=1030000 interval=1000000 consecutive=1 play=1 dropped=0',
        '[MonitorPresent] frame=131073 mode=1 intro=0 repeat=0'])


class InputHandoffTest(unittest.TestCase):
    def test_same_sample_reaches_game(self):
        r=analyze(evidence(),{'2':131073},minimum=1)
        self.assertTrue(r['passed'],r)
        self.assertEqual(r['phase_us'],[500])
        self.assertEqual(r['repeat_presents'],0)
        self.assertEqual(r['stats']['publish_marker_to_inject_us']['median'],(1033100-1000060)/60)

    def test_cannot_claim_success_without_samples(self):
        self.assertFalse(analyze('',{},minimum=1)['passed'])

    def test_wrong_frame_saved_order_or_drift_is_rejected(self):
        for old,new in [('source=131073','source=131074'),('saved=1033000','saved=999999'),
                        ('simulationDue=2030000','simulationDue=2030001'),('dropped=0','dropped=1')]:
            self.assertFalse(analyze(evidence().replace(old,new),{'2':131073},minimum=1)['passed'])

    def test_unconfirmed_tail_not_a_combat_sample(self):
        self.assertFalse(analyze(evidence(),{'2':131072},minimum=1)['passed'])

    def test_final_spin_interruption_is_not_hidden_by_early_arrival(self):
        r=analyze(evidence().replace('due=1030000','due=850000'),{'2':131073},minimum=1)
        self.assertTrue(r['passed'],r)
        self.assertEqual(r['arrival_after_deadline'],0)
        self.assertEqual(r['final_wait_overrun_3us'],1)
        self.assertEqual(r['stats']['final_wait_overrun_us']['maximum'],3000)


if __name__=='__main__':unittest.main()
