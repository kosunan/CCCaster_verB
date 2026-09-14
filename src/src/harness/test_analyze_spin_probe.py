import tempfile
import unittest
from pathlib import Path
from analyze_spin_probe import analyze


class SpinDecompositionTest(unittest.TestCase):
    def test_scheduled_change_and_late_tail_are_separate(self):
        lines = ['[Rollback] BEGIN frame=99 target=100']
        # ticks: 第2Fは予定+600、終了遅れ+120、末尾処理+60、時計換算差+30。
        for frame, due, spin, late, tail in [(100, 1000000, 2000000, 6, 60),
                                            (101, 2000600, 3000750, 126, 120)]:
            lines.append(f'[SpinProbe] f={frame} play=1 due={due} ready={due-180000} reads=1 gap=0 read=6 between=0 gapLate=0 exitLate={late}')
            lines.append(f'[SpinGap] f={frame} gapRead=0 gapBetween=0 exitGap=0 exitRead=6 exitBetween=0')
            parts = ' '.join(f'{k}={spin}' for k in ('spin','bounded','wait','begin','input','trace','commit','step'))
            lines.append(f'[SpinTail] f={frame} {parts} game={spin+tail}')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'game.log'
            path.write_text('\n'.join(lines), encoding='utf-8')
            row = analyze(path)['intervals'][0]
        self.assertEqual(row['schedule_adjustment'], 10)
        self.assertEqual(row['exit_late_change'], 2)
        self.assertEqual(row['tail_change'], 1)
        self.assertEqual(row['clock_mapping_change'], .5)
        self.assertEqual(row['signed_error'], 13.5)
        self.assertEqual(row['abs_residual'], 3.5)

if __name__ == '__main__':
    unittest.main()
