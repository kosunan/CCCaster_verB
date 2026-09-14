"""機器指定の32bit DirectSound処理時間試験を集計。実ゲームや波形試験とは区別する。"""
import csv
import json
import sys
from pathlib import Path

def stats(rows, key):
    values=sorted(float(r[key]) for r in rows)
    return dict(n=len(values), median=values[len(values)//2],
                p99=values[min(len(values)-1,int(len(values)*.99))], maximum=values[-1])

directory=Path(sys.argv[1])
report={}
for path in sorted(directory.glob('device_*.csv')):
    rows=list(csv.DictReader(path.open(encoding='utf-8')))
    groups={}
    for rate in sorted({r['rate'] for r in rows},key=int):
        by_rate=[r for r in rows if r['rate']==rate]
        groups[rate]={}
        for prepared in ('0','1'):
            subset=[r for r in by_rate if r['prepared']==prepared]
            groups[rate][prepared]=dict(play_us=stats(subset,'Play_us'),
                prepare_us=stats(subset,'prepare_us'),
                failures=sum(int(r['failures']) for r in subset),
                preparation_failed=sum(r['prepare_result']!='1' for r in subset) if prepared=='1' else 0)
    report[path.stem]=groups
text=json.dumps(report,ensure_ascii=False,indent=2)
(directory/'analysis.json').write_text(text,encoding='utf-8')
print(text)
