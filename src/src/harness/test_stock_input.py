"""入力欄だけの早い変化や別画像のPresentを遅延結果へ誤採用しない回帰試験。"""
import unittest
from analyze_stock_input import analyze,controller_times,runahead_evidence


def records():
    base=dict(qpc=1000,phase=6,loop=10,world=10,sample=1,stimulus=1,mode=1,intro=0,
        result=0,buffer=0x123000,pov=27000,buttons=0,rawDirection=0,rawButtons=0,
        actorDirection=0,actorButtons=0,sequence=0,animation=100,x=500,y=0,
        renderX=500,renderY=0,renderAnimation=100)
    def row(**values): return {**base,**values}
    changed=dict(loop=11,world=11,actorDirection=4,sequence=11,x=400,renderX=400,animation=200,renderAnimation=200)
    return [row(),row(phase=1,qpc=1010,rawDirection=4),row(phase=7,qpc=1020,actorDirection=4),
        row(phase=8,qpc=1030,actorDirection=4),row(phase=4,qpc=1990,actorDirection=4),
        row(phase=8,qpc=2010,**changed),row(phase=9,qpc=2020,**changed),row(phase=4,qpc=2990,**changed)]


class StockInputTest(unittest.TestCase):
    def test_runahead_requires_restore_after_reactive_image(self):
        rows=records();result=analyze(rows,60000,1)
        # 確定画像の省略要求を誤採用した場合、次の入口でWTは戻らない。
        rows.append({**rows[-1], 'phase':0, 'loop':12, 'qpc':3000})
        runahead_evidence(rows,result,60000)
        self.assertIn('1F復元',result['errors'][0])
        rows[-1]['world']-=1
        result=analyze(rows,60000,1)
        runahead_evidence(rows,result,60000)
        self.assertFalse(result['errors'])
        self.assertEqual(result['runahead']['presented_samples'],1)
    def test_input_acceptance_does_not_count_as_draw_response(self):
        result=analyze(records(),60000,1)
        self.assertTrue(result['passed'],result['errors'])
        endpoints=result['samples'][0]['endpoints']
        self.assertEqual(endpoints['logic']['loop_delta'],0)
        self.assertEqual(endpoints['draw_read']['loop_delta'],1)
        self.assertEqual(endpoints['present']['loop_delta'],1)
        self.assertAlmostEqual(endpoints['present']['ms'],1990/60)
    def test_present_from_another_frame_is_rejected(self):
        rows=records();rows[-1]['loop']=12
        result=analyze(rows,60000,1)
        self.assertFalse(result['passed'])
        self.assertIn('同じ描画更新のPresent要求',result['errors'][0])
    def test_actual_render_registers_must_match(self):
        rows=records();rows[5]['renderY']=9
        self.assertIn('実読出しレジスタ',analyze(rows,60000,1)['errors'][0])
    def test_only_changed_direction_has_no_visible_response(self):
        rows=records()[:5]
        self.assertIn('描画データ読出し',analyze(rows,60000,1)['errors'][0])
    def test_wrong_native_button_mapping_fails(self):
        rows=records()
        for r in rows: r['stimulus']=3
        rows[0].update(pov=0xffffffff,buttons=0x80)
        rows[1].update(rawDirection=0,rawButtons=4) # DをAと誤認しない
        self.assertIn('標準生入力',analyze(rows,60000,1)['errors'][0])
    def test_product_A_contains_confirm_without_becoming_D(self):
        rows=records()
        for r in rows: r['stimulus']=3
        rows[0].update(pov=0xffffffff,buttons=0x80)
        rows[1].update(rawDirection=0,rawButtons=0x410)
        rows[2].update(actorDirection=0,actorButtons=0x1001)
        self.assertTrue(analyze(rows,60000,1,tool=True)['passed'])
        self.assertFalse(analyze(rows,60000,1)['passed'])
    def test_external_origin_keeps_time_before_sampling(self):
        result=analyze(records(),60000,1)
        controller_times(result,[dict(sample=1,stimulus=1,start=300,end=400)],60000)
        self.assertAlmostEqual(result['controller_summary']['present']['median'],2590/60)
        self.assertAlmostEqual(result['controller_summary']['receive']['median'],10)
    def test_external_origin_rejects_already_received_input(self):
        result=analyze(records(),60000,1)
        controller_times(result,[dict(sample=1,stimulus=1,start=1100,end=1200)],60000)
        self.assertIn('発生前',result['errors'][0])


if __name__=='__main__': unittest.main()
