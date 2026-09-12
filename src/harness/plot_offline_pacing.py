"""修正前後の3000区間を、外れ値を除かず可視化する。"""
import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from bench_legacy_real import RECORD

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'docs/benchmarks'
data = json.loads((OUT/'2026-09-13_offline_pacing.json').read_text(encoding='utf-8'))
plt.rcParams.update({'font.family':['Yu Gothic','Meiryo','sans-serif'], 'font.size':11,
    'axes.spines.top':False,'axes.spines.right':False,'svg.fonttype':'none'})
fig, axes=plt.subplots(1,2,figsize=(11,4.6),layout='constrained')
for index,(kind,label,color) in enumerate([('before','Previous v10','#64748b'),('after','Updated v10','#007f8b')]):
    value=data['aggregate'][kind]['abs_error_us']['p99']
    axes[0].bar(label,value,color=color,width=.55)
    axes[0].annotate(f'{value:.2f}µs',(index,value),xytext=(0,6),textcoords='offset points',ha='center')
    errors=[]
    for trial in data['trials']:
        if trial['kind']!=kind: continue
        folder=ROOT/trial['folder']
        raw=(folder/'frames.bin').read_bytes()
        freq=json.loads((folder/'result.json').read_text(encoding='utf-8'))['frequency']
        fixed=trial['fixed_battle']
        ticks=[RECORD.unpack_from(raw,4096+i*RECORD.size)[0] for i in range(fixed['first_ordinal'],fixed['last_ordinal']+1)]
        errors.extend(abs((b-a)*1e6/freq-1e6/60) for a,b in zip(ticks,ticks[1:]))
    errors.sort()
    axes[1].plot(errors,[100*(i+1)/len(errors) for i in range(len(errors))],label=label,color=color,lw=1.8)
axes[0].set(title='Frame-interval absolute error: p99',ylabel='Microseconds (lower is better)')
axes[0].set_ylim(0,max(v['abs_error_us']['p99'] for v in data['aggregate'].values())*1.17)
axes[1].set(xscale='log',xlabel='Absolute error (microseconds, log scale)',ylabel='Intervals within this error (%)',title='All measured intervals',ylim=(0,101))
axes[1].axvline(3,color='#d97706',ls=':',lw=1,label='3µs')
axes[1].legend(frameon=False,loc='lower right')
for axis in axes:
    axis.grid(axis='y',alpha=.15); axis.set_axisbelow(True)
fig.suptitle('Offline pacing update | Same binary, 2 x 1,500 intervals each | Sep 13, 2026',fontsize=14)
fig.savefig(OUT/'2026-09-13_offline_pacing.png',dpi=160)
fig.savefig(OUT/'2026-09-13_offline_pacing.svg')
