import csv
import json
from pathlib import Path
import tempfile
import unittest

from analyze_replay_display import analyze, correlate_dwm, HEADER, RECORD


class ReplayDisplayTests(unittest.TestCase):
    def make(self, directory, dropped=(), missing=()):
        # 一覧1F、イントロ1F、通常2F、次ラウンド1F、結果1F。
        rows = [(1000,26,0,0), (2000,1,0,1), (3000,1,1,0),
                (4000,1,2,0), (5000,1,0,0), (6000,5,0,0)]
        raw = HEADER.pack(0x42434343,1,len(rows),100,60000,1,48).ljust(4096,b'\0')
        for i,(tick,mode,real,intro) in enumerate(rows):
            raw += RECORD.pack(tick,tick+1,i,mode,real,10,0,intro,i,123)
        (directory/'frames.bin').write_bytes(raw)
        capture = dict(game_pid=123,replay='test.rep',replay_sha256='test',capture_complete=True,
                       protected_changes=[], foreground=[dict(tick=t,game=True) for t,*_ in rows])
        (directory/'capture.json').write_text(json.dumps(capture))
        with (directory/'presents.csv').open('w',newline='') as file:
            writer=csv.DictWriter(file,fieldnames=['ProcessID','QPCTime','Dropped','msBetweenPresents',
                'msBetweenDisplayChange','msInPresentAPI','PresentMode','SwapChainAddress'])
            writer.writeheader()
            for i,(tick,*_) in enumerate(rows):
                if i in missing:
                    continue
                writer.writerow(dict(ProcessID=123,QPCTime=tick-100,Dropped=int(i in dropped),
                    msBetweenPresents=1000 if i==1 else 16.667,
                    msBetweenDisplayChange=0 if i in dropped else 16.667,
                    msInPresentAPI=.1,PresentMode='Composed',SwapChainAddress='1'))

    def test_no_drop_and_round_reset(self):
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp); self.make(directory)
            result=analyze(directory)
            self.assertEqual(result['battle']['game_frames'],4)
            self.assertEqual(result['battle']['presents'],4)
            self.assertEqual(result['battle']['world_discontinuities'],0)
            self.assertEqual(len(result['rounds']),2)
            self.assertLess(result['battle']['present_ms']['maximum'],17)

    def test_intro_drop_is_separate_from_active_battle(self):
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp); self.make(directory,dropped=(1,))
            result=analyze(directory)
            self.assertEqual(result['battle']['dropped'],1)
            self.assertEqual(result['active_battle']['dropped'],0)

    def test_missing_present_is_not_counted_as_displayed(self):
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp); self.make(directory,missing=(2,))
            result=analyze(directory)
            self.assertEqual(result['battle']['no_present_frames'],1)
            self.assertEqual(result['battle']['displayed'],3)

    def test_overflow_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp); self.make(directory)
            path=directory/'frames.bin'; data=bytearray(path.read_bytes())
            data[24:28]=(3).to_bytes(4,'little'); path.write_bytes(data)
            with self.assertRaises(ValueError):
                analyze(directory)

    def test_absent_game_trace_is_not_drop_free_evidence(self):
        with tempfile.TemporaryDirectory() as temp:
            directory=Path(temp); self.make(directory,missing=range(6))
            result=analyze(directory)
            self.assertFalse(result['game_present_trace_available'])
            self.assertFalse(result['valid_for_drop_assessment'])
            self.assertEqual(result['display_timing_scope'],'unavailable_game_trace')
            self.assertEqual(result['battle']['no_present_frames'],4)

    def test_dwm_output_switch_is_not_single_monitor_jitter(self):
        def row(tick, latency, chain):
            return dict(QPCTime=str(tick), msUntilDisplayed=str(latency),
                        Dropped='0', SwapChainAddress=chain, msBetweenDisplayChange='8.333')
        # 同じETW表示時刻を、開始時刻の異なるアプリ/DWMイベントから復元する。
        app = [row(1000,10,'game'), row(2000,10,'game'), row(3000,10,'game')]
        dwm = [row(1500,5,'120hz'), row(2500,5,'144hz'), row(3500,5,'120hz')]
        result = correlate_dwm(app,dwm,100000)
        self.assertTrue(result['mixed_outputs'])
        self.assertEqual(result['matched_by_output'], {'120hz':2, '144hz':1})
        self.assertEqual(result['output_switches'],2)

    def test_dwm_ambiguous_and_unmatched_are_not_guessed(self):
        def row(tick, chain):
            return dict(QPCTime=str(tick), msUntilDisplayed='1', Dropped='0',
                        SwapChainAddress=chain, msBetweenDisplayChange='8.333')
        result = correlate_dwm([row(1000,'game'), row(2000,'game')],
                               [row(1000,'a'), row(1000,'b')],100000)
        self.assertEqual(result['ambiguous'],1)
        self.assertEqual(result['unmatched'],1)
        self.assertFalse(result['mixed_outputs'])


if __name__ == '__main__':
    unittest.main()
