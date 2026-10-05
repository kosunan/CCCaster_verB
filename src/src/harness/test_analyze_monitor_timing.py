import unittest
from analyze_monitor_timing import analyze


def sample(display=240, game=60):
    text = [f'[MonitorRefresh] active=1 hz={display}/1 monitor=0']
    last = -1
    for i in range(display * 8):
        image = i * game // display
        text.append(f'[MonitorPresent] seq={i+1} image={image} frame={131073+image} '
                    f'qpc={i*60000000//display} cost=3000 repeat={int(image==last)} '
                    f'missed=0 mode=1 intro=0 world={image}')
        last = image
    return '\n'.join(text)


class MonitorTimingTest(unittest.TestCase):
    def test_independent_rates(self):
        for hz in (60, 120, 144, 240, 360):
            result = analyze(sample(hz))
            self.assertTrue(result['passed'], result)
            self.assertAlmostEqual(result['steady']['present_fps'], hz, places=3)

    def test_acceleration_fails(self):
        self.assertFalse(analyze(sample(game=240))['passed'])

    def test_missing_or_empty_trace_fails(self):
        self.assertFalse(analyze('')['passed'])
        self.assertFalse(analyze(sample().replace('seq=10 ', 'seq=11 '))['passed'])

    def test_repeat_mismatch_fails(self):
        self.assertFalse(analyze(sample().replace('repeat=1', 'repeat=0', 1))['passed'])

    def test_actual_rate_not_label(self):
        text = sample(120).replace('hz=120/1', 'hz=240/1')
        self.assertFalse(analyze(text)['passed'])

    def test_device_failure_fails(self):
        self.assertFalse(analyze(sample() + '\n[MonitorPresent] failed hr=88760868')['passed'])


if __name__ == '__main__':
    unittest.main()
