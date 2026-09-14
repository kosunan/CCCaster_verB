"""共通観測器の区間選択・累積誤差・破損拒否を検査する。"""
import unittest
from bench_legacy_real import HEADER, RECORD, MAP_SIZE, analyze


def recording(rows):
    raw = bytearray(MAP_SIZE)
    HEADER.pack_into(raw, 0, 0x42434343, 1, len(rows), 262144, 60000000, 1, RECORD.size)
    for i, (tick, world, mode, skip, intro) in enumerate(rows):
        RECORD.pack_into(raw, 4096+i*RECORD.size, tick, tick+6, world, mode,
                         world, 99, skip, intro, i, 1)
    return raw


class LegacyBenchmarkAnalysis(unittest.TestCase):
    def test_fixed_window_uses_first_eligible_run_and_keeps_spike(self):
        rows = [(i*1000000, i, 1, 0, 0) for i in range(100)]
        rows.append((100000000, 100, 20, 0, 0))
        # 毎F +1us、採用区間中に一度だけ+1ms。後の良好区間を選ばない。
        for i in range(1700):
            rows.append((200000000+i*1000060+(60000 if i>=300 else 0), i, 1, 0, 0))
        rows.append((2000000000, 0, 8, 0, 0))
        rows.extend((3000000000+i*1000000, i, 1, 0, 0) for i in range(1700))
        fixed = analyze(recording(rows))['fixed_battle']
        self.assertEqual(fixed['first_ordinal'], 221)
        self.assertEqual(fixed['intervals'], 1500)
        self.assertAlmostEqual(fixed['phase_end_us'], 2500)
        self.assertEqual(fixed['over_1ms'], 1)

    def test_loading_intro_skip_and_world_discontinuity_split_runs(self):
        rows = [(i*1000000, i, 1, 0, 0) for i in range(4000)]
        rows[1000] = (1000000000, 1000, 8, 0, 0)
        rows[2000] = (2000000000, 2000, 1, 0, 2)
        rows[3000] = (3000000000, 3000, 1, 1, 0)
        self.assertNotIn('fixed_battle', analyze(recording(rows)))
        rows = [(i*1000000, i+(5 if i>=1000 else 0), 1, 0, 0) for i in range(2000)]
        self.assertNotIn('fixed_battle', analyze(recording(rows)))

    def test_reject_corrupt_header_and_truncated_record(self):
        raw = recording([(0, 0, 1, 0, 0)])
        with self.assertRaises(RuntimeError):
            analyze(raw[:4097])
        raw[24:28] = (3).to_bytes(4, 'little')
        with self.assertRaises(RuntimeError):
            analyze(raw)


if __name__ == '__main__':
    unittest.main()
