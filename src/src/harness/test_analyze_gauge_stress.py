import tempfile
import unittest
import subprocess
import sys
from pathlib import Path
from analyze_gauge_stress import analyze, failures


class GaugeAnalysis(unittest.TestCase):
    def test_cli_help_is_valid(self):
        result=subprocess.run([sys.executable,str(Path(__file__).with_name('analyze_gauge_stress.py')),'--help'],capture_output=True)
        self.assertEqual(result.returncode,0,result.stderr)

    def sample(self, spike=False, hp=11400, missing=False):
        lines=[]
        for f in range(600,1260):
            request=30000 if (f-600)//60%2 else 0
            lines.append(f'[GaugeStress] f={f} requested={request} meter1={request} meter2={request} heat1=0 heat2=0 hp1={hp} hp2=11400 timer=4751 ticks=30000 interval=1000000 replay=0')
            if not (missing and f==661):
                interval=20000 if spike and f==660 else 16667
                lines.append(f'[DisplayPace] f={f} interval={interval}')
        # 再計算は通常更新間隔や満タンイベント数を増やさない。
        lines.append('[GaugeStress] f=660 requested=30000 meter1=0 meter2=0 heat1=825 heat2=825 hp1=11400 hp2=11400 timer=4751 ticks=600000 interval=0 replay=1')
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'game.log';path.write_text('\n'.join(lines))
            return analyze(path)

    def test_complete_run_and_replay_separation(self):
        r=self.sample()
        self.assertEqual(failures(r),[])
        self.assertEqual(r['samples'],660)
        self.assertEqual(r['replay_samples'],1)
        self.assertEqual(r['work_us']['maximum'],500)

    def test_transition_spike_fails_even_with_good_median(self):
        r=self.sample(spike=True)
        self.assertTrue(any('提示間隔誤差' in e for e in failures(r)))

    def test_work_limit_rejects_cost_hidden_by_present_budget(self):
        r=self.sample()
        r['events'][0]['window_work_max_us']=4000
        self.assertEqual(failures(r),[])
        self.assertTrue(any('更新・描画処理' in e for e in failures(r,maximum_work_us=1200)))

    def test_missing_display_sample_cannot_pass(self):
        self.assertTrue(any('採取不足' in e for e in failures(self.sample(missing=True))))

    def test_health_drift_and_wrong_control_fail(self):
        self.assertTrue(any('固定' in e for e in failures(self.sample(hp=11000))))
        self.assertTrue(failures(self.sample(),control=True))

    def test_empty_trace_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'empty.log';path.write_text('')
            self.assertTrue(failures(analyze(path)))


if __name__=='__main__': unittest.main()
