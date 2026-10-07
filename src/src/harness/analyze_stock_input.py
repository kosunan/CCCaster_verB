"""標準DI受取り→実描画読出し→同じ画像のPresent要求を結び付けて集計する。"""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
from statistics import median

NAMES={1:'left',2:'right',3:'A'}


def stats(values):
    return dict(min=min(values),median=median(values),max=max(values))


def controller_times(result,events,frequency):
    """取得位置を動かしただけの短縮を区別するため、外部提出QPCからも集計。"""
    if len(events)!=len(result['samples']):
        result['errors'].append('外部入力と受取り標本の数が不一致');return
    for sample,event in zip(result['samples'],events):
        if sample['sample']!=event['sample'] or sample['stimulus']!=NAMES[event['stimulus']]:
            result['errors'].append('外部入力と受取り標本の対応が不一致');return
        if sample['input_qpc']<event['start']:
            result['errors'].append('外部入力発生前の受取りを誤採用');return
        offset=(sample['input_qpc']-event['end'])*1000/frequency
        sample['controller_to_receive_ms']=offset
        for endpoint in sample['endpoints'].values(): endpoint['controller_ms']=offset+endpoint['ms']
    result['controller_summary']={name:stats([s['endpoints'][name]['controller_ms'] for s in result['samples']])
        for name in result['samples'][0]['endpoints']}
    result['controller_summary']['receive']=stats([s['controller_to_receive_ms'] for s in result['samples']])


def runahead_evidence(rows,result,frequency):
    """省略された確定画像を採用せず、提示後に1F戻した画像であることを観測する。"""
    loops={r['loop']:r for r in rows if r['phase']==0}
    accepted=0
    for sample in result['samples']:
        endpoint=sample['endpoints']['present'];after=loops.get(endpoint['loop']+1)
        if not after or (endpoint['world']-after['world']) & 0xffffffff != 1:
            result['errors'].append(f"標本{sample['sample']}: 先行画像の提示後の1F復元がない")
        else: accepted+=1
    # 追加更新を60Hzの前進と誤算しない。直前の先行更新から同じWTへ戻った入口だけを採取。
    normal=[r for loop,r in loops.items() if loop-1 in loops and r['world']==loops[loop-1]['world']]
    intervals=[(b['qpc']-a['qpc'])*1000/frequency for a,b in zip(normal,normal[1:])
        if b['loop']==a['loop']+2 and (b['world']-a['world']) & 0xffffffff == 1]
    result['runahead']=dict(presented_samples=accepted,restored_before_next_input=accepted==len(result['samples']))
    if intervals:
        result['runahead']['normal_update_intervals_ms']=dict(**stats(intervals),mean=sum(intervals)/len(intervals),count=len(intervals))


def analyze(rows,frequency,expected=30,tool=False):
    markers=[r for r in rows if r['phase']==6]
    errors=[];samples=[]
    if len(markers)!=expected or len({r['sample'] for r in markers})!=expected:
        errors.append(f'入力標本の欠落・重複: {len(markers)}/{expected}')
    for start in markers:
        sid=start['sample'];kind=start['stimulus']
        run=[r for r in rows if r['sample']==sid and r['qpc']>=start['qpc'] and r['loop']<start['loop']+30]
        def first(predicate,description):
            value=next((r for r in run if predicate(r)),None)
            if value is None: raise ValueError(description+'がない')
            return value
        try:
            if kind not in NAMES: raise ValueError('未知の入力')
            if any(r['mode']!=1 or r['intro']!=0 for r in run): raise ValueError('対戦操作可能区間の外')
            if start['result'] & 0x80000000 or not start['buffer']: raise ValueError('GetDeviceStateの受取り失敗')
            if start['pov']!={1:27000,2:9000,3:0xffffffff}[kind] or start['buttons']!=(0x80 if kind==3 else 0):
                raise ValueError('DIJOYSTATE2への入力値が不一致')
            if start['sequence']!=0 or start['actorDirection'] or start['actorButtons']:
                raise ValueError('静止・入力解放からの試行ではない')
            # 製品のAはDirectInputHookの通常マッピングでCONFIRMも併記される。
            direction={1:4,2:6,3:0}[kind];buttons=(0x410 if tool else 0x10) if kind==3 else 0
            raw=first(lambda r:r['phase']==1 and r['rawDirection']==direction and r['rawButtons']==buttons,'標準生入力')
            logic=first(lambda r:r['phase']==7 and (r['actorButtons']!=start['actorButtons'] if kind==3
                else r['actorDirection']==direction),'キャラの入力受理')
            # 入力欄の変化だけでは描画反応とみなさない。実読出しで動作とリソースが変わること。
            draw=first(lambda r:r['phase']==8 and r['sequence']!=start['sequence']
                       and r['renderAnimation']!=start['animation']
                       and (kind==3 or r['renderX']!=start['x']),'反応の描画データ読出し')
            if (draw['renderX'],draw['renderY'],draw['renderAnimation'])!=(draw['x'],draw['y'],draw['animation']):
                raise ValueError('実読出しレジスタとキャラ描画データが不一致')
            submit=first(lambda r:r['phase']==9 and r['loop']==draw['loop'] and r['qpc']>=draw['qpc'],'同じ更新のスプライト描画引数')
            if (submit['renderX'],submit['renderY'],submit['sequence'])!=(draw['renderX'],draw['renderY'],draw['sequence']):
                raise ValueError('スプライトへ渡る描画データが不一致')
            present=first(lambda r:r['phase']==4 and r['loop']==draw['loop'] and r['qpc']>=submit['qpc'],'同じ描画更新のPresent要求')
            if not start['qpc']<=raw['qpc']<=logic['qpc']<=draw['qpc']<=submit['qpc']<=present['qpc']:
                raise ValueError('入力から提示までの順序が不整合')
            endpoints={name:dict(loop=value['loop'],world=value['world'],loop_delta=value['loop']-start['loop'],
                ms=(value['qpc']-start['qpc'])*1000/frequency,
                elapsed_frames_60hz=(value['qpc']-start['qpc'])*60/frequency)
                for name,value in [('raw',raw),('logic',logic),('draw_read',draw),('sprite_submit',submit),('present',present)]}
            samples.append(dict(sample=sid,stimulus=NAMES[kind],input_loop=start['loop'],input_world=start['world'],
                input_qpc=start['qpc'],sequence_before=start['sequence'],sequence_after=draw['sequence'],
                actor_buttons=logic['actorButtons'],x_before=start['x'],x_draw=draw['renderX'],
                animation_before=start['animation'],animation_draw=draw['renderAnimation'],endpoints=endpoints))
        except ValueError as e: errors.append(f'標本{sid}: {e}')
    summary={}
    if samples:
        for name in samples[0]['endpoints']:
            values=[s['endpoints'][name] for s in samples]
            summary[name]=dict(ms=stats([v['ms'] for v in values]),
                elapsed_frames_60hz=stats([v['elapsed_frames_60hz'] for v in values]),
                loop_delta_counts=dict(Counter(v['loop_delta'] for v in values)))
    return dict(passed=not errors and len(samples)==expected,errors=errors,count=len(samples),
                stimuli=dict(Counter(s['stimulus'] for s in samples)),summary=summary,samples=samples)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('directory',type=Path)
    args=parser.parse_args();folder=args.directory
    with (folder/'frames.csv').open() as f:
        rows=[{k:int(v) for k,v in row.items()} for row in csv.DictReader(f)]
    header=json.loads((folder/'status.json').read_text())
    if header['version']!=2: raise ValueError('最終解析はactorButtonsとA割当を持つv2のみ')
    tool=header['reserved']==1
    result=analyze(rows,header['frequency'],tool=tool)
    manifest=json.loads((folder/'manifest.json').read_text())
    delay=manifest.get('delay',0)
    result['mode']=f'tool_d{delay}' if tool else 'stock'
    evidence=json.loads((folder/'result.json').read_text())
    if tool:
        audit=json.loads((folder/'runtime_audit.json').read_text())
        sites={'0x41f098':2,'0x41f0a0':3,'0x4a024e':2,'0x4a027f':3,'0x4a0291':3,
               '0x4a02a2':3,'0x4a02b4':3,'0x4a02e9':2,'0x4a02f2':3}
        if not all(audit['code'][site]=='90'*size for site,size in sites.items()):
            result['errors'].append('製品入力最適化の維持が不正')
        log=(folder/'cccaster_hook_log.txt').read_text(encoding='utf-8',errors='replace')
        if delay!=2 and f'[TrainingDelay] ACTIVE D={delay}' not in log:
            result['errors'].append('ディレイの適用証拠がない')
    else:
        for name in ('runtime_audit_before.json','runtime_audit_after.json'):
            audit=json.loads((folder/name).read_text())
            if not audit['passed']: result['errors'].append(name+': 標準コード不一致')
    if evidence['errors'] or not evidence['protected_unchanged'] or header['completed']!=30 or header['status']!=1:
        result['errors'].append('実行完了・設定保全・観測器状態が不正')
    if (folder/'events.json').exists():
        events=json.loads((folder/'events.json').read_text())
        if events: controller_times(result,events,header['frequency'])
    active=[r for r in rows if r['phase']==0 and r['mode']==1 and r['intro']==0 and r['sample']]
    intervals=[(b['qpc']-a['qpc'])*1000/header['frequency'] for a,b in zip(active,active[1:]) if b['loop']==a['loop']+1]
    if intervals:
        result['update_intervals_ms']=dict(**stats(intervals),mean=sum(intervals)/len(intervals),count=len(intervals))
    if manifest.get('runahead'):
        runahead_evidence(rows,result,header['frequency'])
        result['native_loop_intervals_ms']=result.pop('update_intervals_ms',{})
    result['passed']=result['passed'] and not result['errors']
    (folder/'analysis.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    columns=['sample','stimulus','phase','loop','world','qpc','pov','buttons','rawDirection','rawButtons',
             'actorDirection','actorButtons','sequence','state','x','renderX','animation','renderAnimation']
    marker_loops={r['sample']:r['loop'] for r in rows if r['phase']==6}
    with (folder/'input_to_render_trace.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=columns,extrasaction='ignore');writer.writeheader()
        writer.writerows(r for r in rows if r['sample'] in marker_loops and 0<=r['loop']-marker_loops[r['sample']]<=delay+2)
    print(json.dumps({k:v for k,v in result.items() if k!='samples'},ensure_ascii=False,indent=2))
    return not result['passed']


if __name__=='__main__': raise SystemExit(main())
