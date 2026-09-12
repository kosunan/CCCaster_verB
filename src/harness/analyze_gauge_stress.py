"""ゲージ切替のフレームと処理・更新・提示・音の実測を照合する。"""
import argparse
import json
import math
import re
from pathlib import Path


def stats(values):
    values=sorted(values)
    if not values: return {'count':0}
    return dict(count=len(values),median=values[len(values)//2],p99=values[math.ceil(len(values)*.99)-1],maximum=values[-1])


def analyze(path):
    tags={}
    for line in path.read_text(encoding='utf-8').splitlines():
        m=re.match(r'\[(\w+)\]',line)
        if m:
            row={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',line)}
            tags.setdefault(m[1],[]).append(row)
    all_rows=tags.get('GaugeStress',[])
    rows=[r for r in all_rows if not r['replay']]
    by_frame={r['f']:r for r in rows}
    sounds={r['f']:r['ticks']/60 for r in tags.get('SoundProbe',[]) if not r['replay']}
    display={r['f']:r['interval'] for r in tags.get('DisplayPace',[])}
    events=[]
    for a,b in zip(rows,rows[1:]):
        if b['f']==a['f']+1 and a['requested']!=b['requested']:
            window=[by_frame[f] for f in (b['f']-1,b['f'],b['f']+1,b['f']+2) if f in by_frame]
            events.append(dict(frame=b['f'],to=b['requested'],work_us=b['ticks']/60,
                sound_us=sounds.get(b['f']),next_interval_us=by_frame.get(b['f']+1,{}).get('interval',0)/60,
                window_work_max_us=max(r['ticks']/60 for r in window),
                window_interval_max_us=max(r['interval']/60 for r in window),
                display_samples=sum(r['f'] in display for r in window),
                window_display_max_us=max((display.get(r['f'],0) for r in window),default=0),
                window_display_error_us=max((abs(display[r['f']]-1_000_000/60) for r in window if r['f'] in display),default=0)))
    summary=dict(samples=len(rows),replay_samples=len(all_rows)-len(rows),
        gauge_values=sorted({r['requested'] for r in rows}),
        actual_meter1=sorted({r['meter1'] for r in rows}),actual_meter2=sorted({r['meter2'] for r in rows}),
        health_values=sorted({r[k] for r in rows for k in ('hp1','hp2')}),
        timer_values=sorted({r['timer'] for r in rows}),
        work_us=stats([r['ticks']/60 for r in rows]),
        interval_us=stats([r['interval']/60 for r in rows if r['interval']]),
        rising=stats([e['work_us'] for e in events if e['to']]),
        falling=stats([e['work_us'] for e in events if not e['to']]),events=events,
        spikes=[dict(frame=r['f'],requested=r['requested'],work_us=r['ticks']/60,interval_us=r['interval']/60,
                     sound_us=sounds.get(r['f'])) for r in rows if r['ticks']>60000 or r['interval']>1060000])
    summary['freeze_passed']=bool(rows) and summary['health_values']==[11400] and all(t in (4751,4752) for t in summary['timer_values'])
    return summary


def failures(result, control=False, maximum_error_us=1000, maximum_work_us=None):
    errors=[]
    if not result['freeze_passed']: errors.append('体力・タイマー固定の不成立')
    if result['samples']<600: errors.append('通常フレーム600件未満')
    if control:
        if result['gauge_values']!=[0] or result['events']: errors.append('固定0%の対照条件不成立')
    else:
        if result['gauge_values']!=[0,30000]: errors.append('0% / 300%の両方を採取していない')
        if any(30000 not in result[k] for k in ('actual_meter1','actual_meter2')): errors.append('両者の実ゲージ300%を確認できない')
        for value in (0,30000):
            if sum(e['to']==value for e in result['events'])<3: errors.append(f'{value}への切替が3回未満')
        for event in result['events']:
            if maximum_work_us is not None and event['window_work_max_us']>maximum_work_us:
                errors.append(f"{event['frame']}: 更新・描画処理 {event['window_work_max_us']:.1f}us")
            if event['display_samples']!=4: errors.append(f"{event['frame']}: 提示間隔の採取不足")
            if event['window_display_error_us']>maximum_error_us:
                errors.append(f"{event['frame']}: 提示間隔誤差 {event['window_display_error_us']:.1f}us")
    return errors


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder',type=Path)
    parser.add_argument('--control',action='store_true',help='固定0%%の対照条件を判定')
    parser.add_argument('--check',action='store_true',help='採取不足・固定失敗・提示スパイクで失敗終了')
    parser.add_argument('--maximum-error-us',type=float,default=1000)
    parser.add_argument('--maximum-work-us',type=float,help='提示余裕では隠せない更新・描画処理の上限')
    args=parser.parse_args()
    if args.maximum_error_us<=0: parser.error('maximum-error-us は正数')
    if args.maximum_work_us is not None and (not math.isfinite(args.maximum_work_us) or args.maximum_work_us<=0): parser.error('maximum-work-us は有限の正数')
    result={str(s):analyze(args.folder/f'game_{s}.log') for s in (1,2)}
    for r in result.values(): r['failures']=failures(r,args.control,args.maximum_error_us,args.maximum_work_us)
    (args.folder/'gauge_analysis.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({s:{k:v for k,v in r.items() if k not in ('events','spikes')} for s,r in result.items()},ensure_ascii=False,indent=2))
    raise SystemExit(0 if all(not r['failures'] if args.check else r['freeze_passed'] for r in result.values()) else 1)
