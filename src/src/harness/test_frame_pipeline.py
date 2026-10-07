import unittest
from verify_frame_pipeline import verify


def evidence():
    lines = []
    # A 1ms preparation spike must appear in native entry/rendering, not be
    # subtracted from elapsed time or hidden by claiming the old native boundary.
    for frame in range(58, 63):
        tick = frame * 1_000_000
        work = 60_000 if frame == 60 else 600
        lines.extend([
            f'[FrameStart] f={frame} actual={tick} due={tick} readyLate=-12000',
            f'[UpdateCadence] f={frame} prev={frame-1} ticks={tick} interval=1000000 play=1 consecutive={int(frame>58)} dropped=0',
            f'[FramePipeline] f={frame} start={tick} input={tick+work} saved={tick+work+60} prepared={tick+work+120} native={tick+work+180} present={tick+work+6000}'
        ])
    return '\n'.join(lines)


class FramePipelineTest(unittest.TestCase):
    def test_spike_is_in_work_not_start_period(self):
        report = verify(evidence(),minimum=4,require_saved=True,spike_us=1000)
        self.assertTrue(report['passed'],report)
        self.assertEqual(report['stats']['start_interval_us']['minimum'],1_000_000/60)
        self.assertGreater(report['stats']['native_interval_us']['maximum'],17000)
        self.assertGreater(report['stats']['preparation_us']['maximum'],1000)

    def test_save_before_start_is_rejected(self):
        text = evidence().replace('saved=60060060','saved=59999999')
        self.assertFalse(verify(text,minimum=4,require_saved=True)['passed'])

    def test_missing_native_and_fake_period_are_rejected(self):
        for text in (evidence().replace('native=60060180','native=0'),
                     evidence().replace('interval=1000000','interval=999999'), ''):
            self.assertFalse(verify(text,minimum=4)['passed'])

    def test_replay_cannot_count_as_another_normal_frame(self):
        text = evidence() + '\n' + evidence().splitlines()[5]
        self.assertFalse(verify(text,minimum=4)['passed'])


if __name__ == '__main__':
    unittest.main()
