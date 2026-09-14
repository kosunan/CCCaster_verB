"""確定入力の選択時刻と、両端の実ゲームの遷移先を検査する。"""
import json,re,sys
from pathlib import Path
folder=Path(sys.argv[1]);scenario=int(sys.argv[2]);out={'scenario':scenario,'sides':[]}
for side in (1,2):
 text=(folder/f'game_{side}.log').read_text(encoding='utf-8',errors='replace')
 resolves=list(re.finditer(r'\[Rematch\] RESOLVED frame=(\d+) p1=(-?\d+) p2=(-?\d+) target=(\d+)',text))
 failures=[line for line in text.splitlines() if 'FAILED' in line]
 ok=bool(resolves) and not failures
 evidence=[]
 for m in resolves:
  frame,p1,p2,target=map(int,m.groups());tail=text[m.end():]
  transition=re.search(r'\[SceneRunner\] Phase change: 5 -> (\d+)',tail)
  expected_target=0 if scenario==0 else 1
  expected_next=3 if scenario==0 else 2
  offset=frame%65536
  valid=(target==expected_target and transition is not None and int(transition[1])==expected_next)
  if scenario==0:valid=valid and p1==0 and p2==0 and offset==300
  else:valid=valid and (p1,p2)==((1,-1) if scenario==1 else (-1,1)) and offset==64
  ok=ok and valid
  evidence.append({'frame':frame,'offset':offset,'p1':p1,'p2':p2,'target':target,'next_phase':int(transition[1]) if transition else None,'passed':valid})
 out['sides'].append({'side':side,'resolutions':evidence,'failures':failures,'passed':ok})
out['passed']=all(s['passed'] for s in out['sides'])
(folder/'rematch_comparison.json').write_text(json.dumps(out,indent=2),encoding='utf-8')
print(json.dumps(out,indent=2));sys.exit(0 if out['passed'] else 1)
