"""D0固定の描画前入力訂正。再計算回数と、前進したゲーム内時刻を区別する。"""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
from analyze_stock_input import analyze, controller_times


def add_late_evidence(rows, corrections, result, depth, neutral=False):
    errors=result['errors']
    by_sample={c['trial']:c for c in corrections}
    if len(by_sample)!=len(corrections) or set(by_sample)!={s['sample'] for s in result['samples']}:
        errors.append('入力標本と訂正記録の欠落・重複')
    markers={r['sample']:r for r in rows if r['phase']==6}
    old_draws=0
    for sample in result['samples']:
        sid=sample['sample'];c=by_sample.get(sid);marker=markers[sid]
        if c is None: continue
        def require(ok, message):
            if not ok: errors.append(f'標本{sid}: '+message)
        require(c['depth']==depth and c['status']==4 and not c['errors'],'訂正処理が未完了')
        require(c['worldBefore']==c['worldAfter']==marker['world'],'再計算でゲーム内時刻が前進した')
        require(c['replayCount']==depth,'再計算回数の不一致')
        require(c['rawBefore']==0 and c['rawLatest']!=0,'入力発生条件が不一致')
        require(bool(c.get('neutral',False))==neutral,'入力維持の対照条件が不一致')
        delta=sample['endpoints']['present']['world']-marker['world']
        sample['present_world_delta']=delta
        require(delta==(2 if neutral else 2-depth),'反応画像までの前進Fが不一致')
        sample['recalculated_updates']=depth
        raw_loop=sample['endpoints']['raw']['loop']
        before=next((r for r in rows if r['loop']==raw_loop and r['phase']==10),None)
        after=next((r for r in rows if r['loop']==raw_loop and r['phase']==11),None)
        motion={'left':11,'right':10,'A':1}[sample['stimulus']]
        require(before is not None and after is not None,'状態更新前／予約後の観測がない')
        if before and after:
            require(before['deviceIndex']==65535 and before['sequence']==0,'状態更新前に予約が存在した')
            require(after['deviceIndex']==motion and after['sequence']==0,'標準処理で次動作が予約されていない')
            sample['reserved_motion']=after['deviceIndex']
        require(c['motionAfter']==(motion if depth==2 and not neutral else 0),'訂正完了時の動作が不一致')
        if depth==2:
            old=next((r for r in rows if r['loop']==marker['loop']-1 and r['phase']==8),None)
            replay=next((r for r in rows if r['loop']==marker['loop']+1 and r['phase']==8),None)
            fields=('world','sequence','state','renderX','renderY','renderAnimation')
            equal=old is not None and replay is not None and all(old[k]==replay[k] for k in fields)
            sample['previous_actor_draw_identical']=equal
            require(equal,'訂正前後の直前更新でキャラ描画データが変わった')
            old_draws+=int(equal)
    result['late_input']=dict(depth=depth,neutral_control=neutral,
        present_world_delta_counts=dict(Counter(s['present_world_delta'] for s in result['samples'] if 'present_world_delta' in s)),
        previous_actor_draw_identical_count=old_draws,
        previous_full_image_identical_count=sum(c['pastImageEqual'] for c in corrections) if depth==2 else None,
        previous_pixels_different=[c['pastPixelsDifferent'] for c in corrections] if depth==2 else [],
        neutral_saved_state_identical_count=sum(c.get('stateBytesDifferent',-1)==0 for c in corrections) if neutral else None,
        neutral_state_different_bytes=[c.get('stateBytesDifferent') for c in corrections] if neutral else [],
        elapsed_time_is_normal_latency_measurement=False)
    result['measurement_valid']=result['passed'] and not errors
    result.pop('passed')
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path)
    folder=parser.parse_args().directory
    read=lambda name:json.loads((folder/name).read_text(encoding='utf-8'))
    manifest=read('manifest.json');status=read('status.json');execution=read('result.json')
    corrections=read('corrections.json')
    with (folder/'frames.csv').open() as stream:
        rows=[{k:int(v) for k,v in row.items()} for row in csv.DictReader(stream)]
    result=analyze(rows,status['frequency'],tool=True)
    if manifest['delay']!=0 or manifest['source']!='controller' or manifest.get('runahead'):
        result['errors'].append('D0固定・実コントローラ・先行表示なしの条件が不一致')
    if execution['errors'] or not execution['protected_unchanged'] or execution.get('completed')!=30:
        result['errors'].append('実ゲーム未完了または保全違反')
    if status['version']!=2 or status['status']!=1 or status['completed']!=30:
        result['errors'].append('観測器が不正')
    log=(folder/'cccaster_hook_log.txt').read_text(encoding='utf-8',errors='replace')
    if '[TrainingDelay] ACTIVE D=0' not in log or '[InputRunahead] restored=' in log:
        result['errors'].append('実行時のD0・先行表示無効の証拠が不一致')
    controller_times(result,read('events.json'),status['frequency'])
    add_late_evidence(rows,corrections,result,manifest['late_depth'],manifest.get('late_neutral',False))
    result['conditions']=dict(delay=0,real_api_virtual_controller=True,forced_before_present_gate=True,
        gpu_readback=True,normal_latency_benchmark=False,sha256=manifest['sha256'])
    (folder/'late_analysis.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in result.items() if k in ('measurement_valid','errors','count','late_input')},ensure_ascii=False,indent=2))
    return not result['measurement_valid']


if __name__=='__main__': raise SystemExit(main())
