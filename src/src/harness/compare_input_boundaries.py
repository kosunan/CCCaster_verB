"""既存標準測定と補完した製品測定を比較し、旧D0ログの再利用範囲も残す。"""
import argparse
import json
from pathlib import Path
from statistics import median


def compare(stock,tool,previous):
    read=lambda p:json.loads(p.read_text(encoding='utf-8'))
    a=read(stock/'analysis.json');b=read(tool/'analysis.json');old=read(previous/'analysis.json')
    if not all(x['passed'] for x in (a,b,old)): raise ValueError('合格していない測定')
    sg=read(stock/'runtime_audit_before.json')['game'];tg=read(tool/'runtime_audit.json')['game']
    if any(sg[k]!=tg[k] for k in ['p1character','p2character','stage','mode']): raise ValueError('対戦条件不一致')
    if read(tool/'manifest.json')['delay']!=0: raise ValueError('製品側がD0ではない')
    result=dict(stock=str(stock),tool=str(tool),conditions=dict(characters=[sg['p1character'],sg['p2character']],
        stage=sg['stage'],mode='Training',delay=0,samples_each=30),stages={})
    for label,data in [('stock',a),('tool_D0',b)]:
        result['stages'][label]={}
        for name,start,end in [('receive_to_raw',None,'raw'),('raw_to_draw','raw','draw_read'),
                ('draw_to_present','draw_read','present'),('raw_to_present','raw','present'),('receive_to_present',None,'present')]:
            values=[s['endpoints'][end]['ms']-(s['endpoints'][start]['ms'] if start else 0) for s in data['samples']]
            result['stages'][label][name]=dict(min_ms=min(values),median_ms=median(values),max_ms=max(values))
    values=[s['movement_present_ms']-s['raw_ms'] for s in old['details']]
    result['previous_D0_reuse']=dict(directory=str(previous),count=old['samples'],raw_to_movement_present_ms=
        dict(min=min(values),median=median(values),max=max(values)),raw_to_movement_updates=old['summary']['raw_to_movement_frames'],
        limitation='旧ログに取得API直後のQPCと実描画レジスタはない。不足を新測定で補完。製品バイナリは異なるため、再利用は対応区間の整合確認に限る。')
    result['present_median_difference_ms_stock_minus_tool']=a['summary']['present']['ms']['median']-b['summary']['present']['ms']['median']
    result['present_median_difference_frames_60hz']=result['present_median_difference_ms_stock_minus_tool']*.06
    sb=read(stock/'protected_before.json');tb=read(tool/'protected_before.json');common=set(sb)&set(tb)
    result['common_protected_count']=len(common)
    result['common_protected_files_equal']=bool(common) and all(sb[k]==tb[k] for k in common)
    result['passed']=result['common_protected_files_equal']
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('stock','tool','previous'): parser.add_argument(name,type=Path)
    args=parser.parse_args();result=compare(args.stock.resolve(),args.tool.resolve(),args.previous.resolve())
    (args.tool/'comparison.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
    raise SystemExit(not result['passed'])
