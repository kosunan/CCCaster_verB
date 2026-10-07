"""先行表示を同時刻の訂正と誤認せず、予約と過去描画の変化を検出する。"""
import unittest
from analyze_late_input import add_late_evidence


def fixture():
    draw=dict(phase=8,world=19,sequence=0,state=0,renderX=100,renderY=0,renderAnimation=200)
    rows=[dict(phase=6,sample=1,loop=10,world=21),dict(draw,loop=9),dict(draw,loop=11),
        dict(phase=10,loop=11,deviceIndex=65535,sequence=0),dict(phase=11,loop=11,deviceIndex=11,sequence=0)]
    correction=dict(trial=1,depth=2,status=4,errors=0,worldBefore=21,worldAfter=21,
        replayCount=2,rawBefore=0,rawLatest=4<<16,motionAfter=11,pastImageEqual=0,pastPixelsDifferent=123)
    result=dict(passed=True,errors=[],samples=[dict(sample=1,stimulus='left',
        endpoints=dict(raw=dict(loop=11),present=dict(loop=12,world=21)))])
    return rows,[correction],result


class LateInputTest(unittest.TestCase):
    def test_replayed_updates_are_not_elapsed_frames(self):
        rows,cs,r=fixture();add_late_evidence(rows,cs,r,2)
        self.assertTrue(r['measurement_valid'],r['errors'])
        self.assertEqual(r['late_input']['present_world_delta_counts'],{0:1})
        self.assertEqual(r['late_input']['previous_full_image_identical_count'],0)
        self.assertFalse(r['late_input']['elapsed_time_is_normal_latency_measurement'])
    def test_future_world_rejected(self):
        rows,cs,r=fixture();cs[0]['worldAfter']+=1;add_late_evidence(rows,cs,r,2)
        self.assertIn('ゲーム内時刻が前進',r['errors'][0])
    def test_earlier_actor_image_change_rejected(self):
        rows,cs,r=fixture();rows[2]['renderX']+=1;add_late_evidence(rows,cs,r,2)
        self.assertIn('キャラ描画データ',r['errors'][0])
    def test_derived_value_without_native_reservation_rejected(self):
        rows,cs,r=fixture();rows[-1]['deviceIndex']=65535;add_late_evidence(rows,cs,r,2)
        self.assertIn('標準処理で次動作',r['errors'][0])
    def test_input_missing_not_silently_dropped(self):
        rows,cs,r=fixture();add_late_evidence(rows,[],r,2)
        self.assertIn('欠落・重複',r['errors'][0])


if __name__=='__main__': unittest.main()
