"""仮想DS4提出、標準入力変換、キャラ状態、描画投入、Present入口を分けて集計。"""
import argparse
import csv
import json
from pathlib import Path
import statistics


def analyze(path):
    with (path/'frames.csv').open() as stream:
        rows=[{k:int(v) for k,v in r.items()} for r in csv.DictReader(stream)]
    events=json.loads((path/'events.json').read_text())
    status=json.loads((path/'status.json').read_text())
    present_phase=4 if any(r['phase']==4 for r in rows) else 3
    samples=[]
    errors=[]
    # 最後のmeasureが発行した30押下・30解放だけを使い、設定メニューの操作を除外。
    for index,event in enumerate(events[-60:]):
        if event['command'] not in ('left','right'): continue
        direction=4 if event['command']=='left' else 6
        if event['observation']['last'][4:6]!=[1,0]:
            errors.append(f'{index}: 対戦操作可能状態でない');continue
        candidates=[r for r in rows if event['end']<=r['qpc']<event['end']+status['frequency']*.16 and r['mode']==1 and r['intro']==0]
        raw=next((r for r in candidates if r['phase']==1 and r['rawDirection']==direction),None)
        if not raw:
            errors.append(f'{index}: 生入力を捕捉できない');continue
        logic=next((r for r in candidates if r['qpc']>=raw['qpc'] and r['phase']==2 and r['actorDirection']==direction),None)
        if not logic:
            errors.append(f'{index}: キャラ入力の反映がない');continue
        present=next((r for r in candidates if r['phase']==present_phase and r['loop']==logic['loop']),None)
        movement=next((r for r in candidates if r['phase']==2 and r['qpc']>=logic['qpc'] and r['x']!=event['observation']['last'][10]),None)
        if not present or not movement:
            errors.append(f'{index}: 表示要求または移動を捕捉できない');continue
        move_present=next((r for r in candidates if r['phase']==present_phase and r['loop']==movement['loop']),None)
        if not move_present: errors.append(f'{index}: 移動の表示要求がない');continue
        ms=lambda r: round((r['qpc']-event['end'])*1000/status['frequency'],4)
        samples.append(dict(command=event['command'],raw_ms=ms(raw),logic_ms=ms(logic),present_ms=ms(present),
                            movement_ms=ms(movement),movement_present_ms=ms(move_present),
                            raw_to_logic_frames=logic['loop']-raw['loop'],raw_to_movement_frames=movement['loop']-raw['loop']))
    summary={}
    for key in ('raw_ms','logic_ms','present_ms','movement_ms','movement_present_ms','raw_to_logic_frames','raw_to_movement_frames'):
        values=[s[key] for s in samples]
        if values: summary[key]=dict(min=min(values),median=round(statistics.median(values),4),max=max(values))
    result=dict(passed=len(samples)==30 and not errors,samples=len(samples),errors=errors,summary=summary,details=samples,present_phase=present_phase,
                scope='仮想DS4更新完了からCPU上の入力反映・描画投入・ゲームのPresent呼出し直前（phase4、旧ログphase3は内部待機前）。GPU表示完了・物理表示は含まない')
    (path/'analysis.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths',type=Path,nargs='+')
    args=parser.parse_args()
    passed=True
    for path in args.paths:
        result=analyze(path)
        passed=passed and result['passed']
        print(json.dumps(dict(path=str(path),samples=result['samples'],errors=result['errors'],summary=result['summary']),ensure_ascii=False,indent=2))
    raise SystemExit(0 if passed else 1)
