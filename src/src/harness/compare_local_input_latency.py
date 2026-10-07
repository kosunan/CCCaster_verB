"""同じ外部入力・観測器の修正前後比較とD2維持を判定する。"""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path


def compare(before,after,delayed):
    read=lambda p:json.loads(p.read_text(encoding='utf-8'))
    logs=[before,after,delayed];data=[read(p/'analysis.json') for p in logs]
    errors=[]
    for folder,result in zip(logs,data):
        if not result['passed'] or result['count']!=30: errors.append(str(folder)+': 30標本未達')
    manifests=[read(p/'manifest.json') for p in logs]
    hashes=[{Path(k).name:v for k,v in m['sha256'].items()} for m in manifests]
    for name in ('MBAA.exe','stock_input_probe.dll','stock_input_probe.cpp','run_tool_input_boundary.py'):
        if len({h[name] for h in hashes})!=1: errors.append(name+': 測定条件が異なる')
    games=[read(p/'runtime_audit.json')['game'] for p in logs]
    for key in ('p1character','p2character','stage','mode'):
        if len({g[key] for g in games})!=1: errors.append(key+': 対戦条件不一致')
    a,b,d=data
    if a.get('mode')!='tool_d0' or b.get('mode')!='tool_d0' or d.get('mode')!='tool_d2': errors.append('D0/D2条件不一致')
    poll_counts=[]
    for folder in logs:
        rows=list(csv.DictReader((folder/'frames.csv').open()))
        last_loop=max(int(r['loop']) for r in rows)
        loops={int(r['loop']) for r in rows if r['phase']=='0' and r['mode']=='1' and r['intro']=='0'
               and int(r['sample'])>0 and int(r['loop'])<last_loop}
        polls=Counter(int(r['loop']) for r in rows if r['phase']=='5' and int(r['loop']) in loops)
        counts=dict(Counter(polls.values()))
        if any(polls[loop]!=1 for loop in loops) or len(loops)<1000: errors.append(str(folder)+': 1更新1採取の条件不一致')
        poll_counts.append(dict(directory=str(folder),updates=len(polls),polls_per_update=counts))
    receive_gain=a['summary']['present']['ms']['median']-b['summary']['present']['ms']['median']
    controller_gain=a['controller_summary']['present']['median']-b['controller_summary']['present']['median']
    delay_extra=d['summary']['present']['ms']['median']-b['summary']['present']['ms']['median']
    if not 14<receive_gain<18: errors.append('受取り起点で約1Fの改善がない')
    if not 12<controller_gain<20: errors.append('外部入力発生起点で改善がない')
    if not 32<delay_extra<35: errors.append('D2の追加2Fが維持されていない')
    return dict(passed=not errors,errors=errors,before=str(before),after=str(after),delayed=str(delayed),
        receive_to_present_median_ms=[x['summary']['present']['ms']['median'] for x in data],
        controller_to_present_median_ms=[x['controller_summary']['present']['median'] for x in (a,b)],
        receive_origin_gain_ms=receive_gain,controller_origin_gain_ms=controller_gain,D2_extra_ms=delay_extra,
        update_intervals_ms=[x['update_intervals_ms'] for x in data],single_poll_per_update=poll_counts,
        product_hashes=hashes)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('before','after','delayed'): parser.add_argument(name,type=Path)
    args=parser.parse_args();result=compare(args.before.resolve(),args.after.resolve(),args.delayed.resolve())
    (args.after/'improvement.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2));raise SystemExit(not result['passed'])
