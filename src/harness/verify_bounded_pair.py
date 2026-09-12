from pathlib import Path
import sys,json
p=Path(sys.argv[1])
def records(role,prefix):
 d={}; duplicates=[]
 for line in (p/f'{role}.log').read_text(encoding='utf-8',errors='replace').splitlines():
  if line.startswith(prefix):
   a=list(map(int,line[len(prefix):].split()))
   if a[0] in d: duplicates.append(a[0])
   d[a[0]]=a[1:]
 return d,duplicates
report={}; ok=True
for prefix in ['[REC] ','[FRAME] ']:
 h,hd=records('host',prefix);c,cd=records('client',prefix)
 common=sorted(h.keys()&c.keys())
 mismatches=[f for f in common if h[f]!=c[f]]
 missing=sorted(h.keys()^c.keys())
 epochs=sorted({f//65536 for f in common})
 holes=[]
 for e in epochs:
  frames=[f for f in common if f//65536==e]
  if frames != list(range(e*65536+1,max(frames)+1)):holes.append(e)
 report[prefix.strip()]={'host':len(h),'client':len(c),'common':len(common),'mismatches':len(mismatches),'first':mismatches[:5],'missing':len(missing),'duplicates':len(hd)+len(cd),'epochs':epochs,'holes':holes}
 ok &= len(common)>1000 and not(mismatches or missing or hd or cd or holes)
report['passed']=bool(ok)
(p/'result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
print(json.dumps(report,ensure_ascii=False,indent=2))
sys.exit(0 if ok else 1)
