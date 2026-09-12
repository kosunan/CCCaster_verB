"""比較資料用の標準Matplotlib図。JSONと生QPC列から生成する。"""
import json
from pathlib import Path
import struct
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT/'docs/benchmarks'
data = json.loads((OUT/'2026-09-13_legacy_comparison.json').read_text(encoding='utf-8'))
plt.rcParams.update({'font.family': ['Yu Gothic', 'Meiryo', 'sans-serif'], 'font.size': 11,
                     'axes.spines.top': False, 'axes.spines.right': False, 'svg.fonttype': 'none'})
colors = ['#64748b', '#007f8b']
labels = ['旧CCCaster', 'v10']
fig, axes = plt.subplots(1, 3, figsize=(14, 4.5), layout='constrained')
startup = [data['startup'][v]['median'] for v in ('old','new')]
bars = axes[0].bar(labels, startup, color=colors, width=.55)
for bar, value in zip(bars, startup):
    axes[0].text(bar.get_x()+bar.get_width()/2, value+.15, f'{value:.3f}秒', ha='center')
axes[0].set(title='トレーニング起動・各3回の中央値', ylabel='秒（短いほど速い）', ylim=(0,10))
for variant, label, color in zip(('old','new'), labels, colors):
    c = data['cadence'][variant]
    m = c['measurement']
    raw = (ROOT/c['folder']/'frames.bin').read_bytes()
    record = struct.Struct('<qq8I')
    ticks = [record.unpack_from(raw,4096+i*record.size)[0]
             for i in range(m['first_ordinal'],m['last_ordinal']+1)]
    phase = [((tick-ticks[0])*1e6/c['frequency']-i*1e6/60)/1000 for i,tick in enumerate(ticks)]
    axes[1].plot([i/60 for i in range(len(ticks))],phase,label=label,color=color,lw=1.4)
axes[1].set(title='1/60秒からの累積ずれ',xlabel='基準の経過秒数',ylabel='ms',xlim=(0,25))
axes[1].legend(frameon=False)
p99 = [data['cadence'][v]['measurement']['abs_error_us']['p99'] for v in ('old','new')]
bars = axes[2].bar(labels,p99,color=colors,width=.55)
for bar,value in zip(bars,p99):
    axes[2].text(bar.get_x()+bar.get_width()/2,value+8,f'{value:.2f}µs',ha='center')
axes[2].set(title='単一フレーム間隔の絶対誤差・p99',ylabel='µs（小さいほど一定）',ylim=(0,max(p99)*1.2))
for axis in axes:
    axis.grid(axis='y',alpha=.15)
    axis.set_axisbelow(True)
fig.suptitle('旧版とv10の共通観測による比較  |  Ryzen 7 5800X3D  |  2026-09-13',fontsize=15)
fig.savefig(OUT/'2026-09-13_legacy_comparison.png',dpi=160)
fig.savefig(OUT/'2026-09-13_legacy_comparison.svg')
print('比較図をPNG/SVGで保存しました')
