import struct
import tempfile
import unittest
from pathlib import Path
from analyze_update_cadence import analyze_text, read_evidence


def log(errors=(0, 0), evidence=0):
    ticks = 1000000
    lines = [f'[UpdateCadence] n=1 f=65537 prev=0 ticks={ticks} interval=0 error=0 consecutive=0 spike=0 dropped=0 evidence={evidence} play=1']
    for n, error in enumerate(errors, 2):
        ticks += 1000000 + error
        lines.append(f'[UpdateCadence] n={n} f={65536+n} prev={65535+n} ticks={ticks} interval={1000000+error} error={error} consecutive=1 spike={int(abs(error)>180)} dropped=0 evidence={evidence} play=1')
    return '\n'.join(lines)


class UpdateCadenceTests(unittest.TestCase):
    def test_no_rollback_required_and_maximum_not_p99(self):
        self.assertTrue(analyze_text(log([0]*999 + [180]), 1000)['passed'])
        r = analyze_text(log([0]*999 + [181]), 1000)
        self.assertFalse(r['passed'])
        self.assertEqual(len(r['spikes']), 1)
        self.assertAlmostEqual(r['maximum_abs_error_us'], 181/60)

    def test_negative_short_interval_also_fails(self):
        self.assertFalse(analyze_text(log([-181]), 1)['passed'])

    def test_missing_and_corrupt_samples_fail(self):
        for text in ['', log().replace('n=2', 'n=8'), log().replace('dropped=0', 'dropped=1'),
                     log().replace('error=0 consecutive=1', 'error=1 consecutive=1'),
                     log() + '\n[FRAME] 65540 4 0 8 9', log() + '\n[DeferredTraceDropped] count=1']:
            self.assertFalse(analyze_text(text, 1)['passed'], text)

    def test_early_spike_is_not_hidden_by_rollback_cutoff(self):
        r = analyze_text(log([181, 0]) + '\n[Rollback] BEGIN frame=65539 target=65539', 2)
        self.assertEqual(r['spikes'][0]['frame'], 65538)

    def test_evidence_missing_fails(self):
        r = analyze_text(log([181], evidence=1), 1)
        self.assertTrue(any('証拠保存不備' in f for f in r['failures']))

    def test_cannot_hide_combat_with_play_flag(self):
        text = log().replace('play=1', 'play=0').replace('consecutive=1', 'consecutive=0').replace('interval=1000000', 'interval=0')
        text += '\n[FRAME] 65538 4 0 8 9'
        self.assertTrue(any('非戦闘として除外' in f for f in analyze_text(text, 1)['failures']))

    def test_binary_evidence_and_truncation(self):
        data = struct.pack('<12I3q', 0x53504343, 1, 65550, 65549, 14, 123, 10, 28, 65549, 65551, 1, 180, 9900, 1000181, 181)
        for frame in range(65540, 65550):
            data += struct.pack('<8I', frame, 1, 4, 1, 2, 1, 1, 2) + bytes(28) + b'ABCD'
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / 'state.bin'
            p.write_bytes(data)
            r = read_evidence(p)
            self.assertTrue(r['complete'])
            self.assertEqual([x['frame'] for x in r['rows']], list(range(65540, 65550)))
            for bad in [data[:-1], data + b'X', data[:30]]:
                p.write_bytes(bad)
                with self.assertRaises(ValueError):
                    read_evidence(p)


if __name__ == '__main__':
    unittest.main()
