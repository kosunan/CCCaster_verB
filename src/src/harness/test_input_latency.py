import csv
import json
from pathlib import Path
import tempfile
import unittest
from analyze_input_latency import analyze


class InputLatencyTest(unittest.TestCase):
    def fixture(self,path,rows):
        fields=['qpc','phase','loop','world','mode','intro','rawDirection','rawButtons','converted','actorDirection','x','y','pattern']
        with (path/'frames.csv').open('w',newline='') as f:
            writer=csv.writer(f);writer.writerow(fields)
            for tick,phase,loop,x in rows:
                writer.writerow([tick,phase,loop,loop,1,0,4,0,4,4,x,0,0])
        (path/'events.json').write_text(json.dumps([dict(command='left',end=100,observation=dict(last=[0,2,0,0,1,0,0,0,0,0,100,0,0]))]))
        (path/'status.json').write_text(json.dumps(dict(frequency=1000)))

    def test_separates_state_and_movement_frame(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)
            self.fixture(path,[(110,1,5,100),(111,2,5,100),(125,3,5,100),(127,1,6,100),(128,2,6,90),(142,3,6,90)])
            result=analyze(path)
            self.assertEqual(result['errors'],[])
            row=result['details'][0]
            self.assertEqual(row['raw_to_logic_frames'],0)
            self.assertEqual(row['raw_to_movement_frames'],1)
            self.assertEqual(row['present_ms'],25)
            self.assertEqual(row['movement_present_ms'],42)

    def test_missing_present_is_not_a_success(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)
            self.fixture(path,[(110,1,5,100),(111,2,5,100),(128,2,6,90)])
            result=analyze(path)
            self.assertEqual(result['samples'],0)
            self.assertTrue(result['errors'])

    def test_uses_present_call_after_internal_wait(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)
            self.fixture(path,[(110,1,5,100),(111,2,5,100),(112,3,5,100),(125,4,5,100),
                               (127,1,6,100),(128,2,6,90),(129,3,6,90),(142,4,6,90)])
            result=analyze(path)
            self.assertEqual(result['present_phase'],4)
            self.assertEqual(result['details'][0]['present_ms'],25)
            self.assertEqual(result['details'][0]['movement_present_ms'],42)

    def test_intro_input_is_excluded(self):
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)
            self.fixture(path,[])
            events=json.loads((path/'events.json').read_text());events[0]['observation']['last'][5]=2
            (path/'events.json').write_text(json.dumps(events))
            result=analyze(path)
            self.assertEqual(result['samples'],0)
            self.assertIn('対戦操作可能状態でない',result['errors'][0])


if __name__=='__main__': unittest.main()
