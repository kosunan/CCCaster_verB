from pathlib import Path
import json,sys
p=Path(sys.argv[1]); names=['mode','intro','state','WT','RT','roundTimer','menuCtr','rng0','rng1','p1seq','p2seq','p1hp','p2hp','roundCnt','p1win','p2win']
def get(role,prefix):
 d={};dups=[]
 for l in (p/f'game_{role}.log').read_text(encoding='utf-8',errors='replace').splitlines():
  if l.startswith(prefix):
   a=list(map(int,l[len(prefix):].split()))
   if a[0] in d:dups.append(a[0])
   d[a[0]]=a[1:]
 return d,dups
result={}
for prefix in ['[REC] ','[FRAME] ','[STATE] ','[MEM] ']:
 h,hd=get(1,prefix);c,cd=get(2,prefix);common=sorted(h.keys()&c.keys())
 diffs={}
 if prefix=='[MEM] ':
  for i,n in enumerate(names):
   f=[k for k in common if h[k][i]!=c[k][i]]
   diffs[n]={'count':len(f),'first': f[:3]}
 else:
  f=[k for k in common if h[k]!=c[k]];diffs['all']={'count':len(f),'first':f[:3]}
 result[prefix.strip()]={'common':len(common),'host':len(h),'client':len(c),'duplicates':len(hd)+len(cd),'diffs':diffs}
# 絶対WTとメニュー内部カウンタは比較結果を残すが合否条件にはしない。
essential=[n for n in names if n not in ('WT','menuCtr')]
passed=all(result[k]['common']>=1000 and result[k]['duplicates']==0 and
           result[k]['diffs']['all']['count']==0 for k in ('[REC]','[FRAME]','[STATE]'))
passed &= all(result['[MEM]']['diffs'][n]['count']==0 for n in essential)
passed &= all(abs(result[k]['host']-result[k]['client'])<=8 for k in result)
# 人為的に両窓を停止した末尾の先行分だけを許容し、途中の欠落を検査する。
for role in (1,2):
    frames,_=get(role,'[REC] ')
    for epoch in {f//65536 for f in frames}:
        block=sorted(f for f in frames if f//65536==epoch)
        passed &= block==list(range(epoch*65536+1,max(block)+1))
result['passed']=bool(passed)
result['note']='Absolute WT and menu counter are reported separately; relative progression is compared in FRAME. Up to 8 tail frames may differ when stopping both processes.'
(p/'comparison.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(result,ensure_ascii=False,indent=2))
sys.exit(0 if passed else 1)
