"""軽量スピン計測を解析。原本ticks=1/60µsを保ち、報告だけµsに変換する。"""
import json
import math
import re
import sys
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return dict(count=len(values), median=round(values[len(values)//2], 3),
                p99=round(values[math.ceil(len(values)*.99)-1], 3), maximum=round(values[-1], 3))


def analyze(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    tables = {tag: {} for tag in ('SpinProbe', 'SpinTail', 'SpinGap', 'SpinReturn', 'SpinClock')}
    for line in text.splitlines():
        for tag in tables:
            if line.startswith(f'[{tag}]'):
                row = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
                tables[tag][row['f']] = row
    first = re.search(r'\[Rollback\] BEGIN frame=\d+ target=(\d+)', text)
    if not first:
        return {"error": "初回ロールバックなし"}
    first = int(first[1])
    samples = []
    missing = []
    boundaries = ('spin', 'bounded', 'wait', 'begin', 'input', 'trace', 'commit', 'step', 'game')
    previous = None
    previous_probe = None
    for f, p in sorted(tables['SpinProbe'].items()):
        if not p['play'] or f < first or f//65536 != first//65536:
            continue
        t, g = tables['SpinTail'].get(f), tables['SpinGap'].get(f)
        if not t or not g or any(not t.get(k) for k in boundaries):
            missing.append(f)
            continue
        s = dict(frame=f, reads=p['reads'], ready_margin=(p['due']-p['ready'])/60)
        for k in ('gap', 'read', 'between', 'gapLate', 'exitLate'):
            s[k] = p[k]/60
        for k in ('gapRead', 'gapBetween', 'exitGap', 'exitRead', 'exitBetween'):
            s[k] = g[k]/60
        if p['reads'] > 1:
            assert p['gap'] == g['gapRead'] + g['gapBetween']
            assert g['exitGap'] == g['exitRead'] + g['exitBetween']
        for a, b in zip(boundaries, boundaries[1:]):
            assert t[b] >= t[a], (f, a, b)
            s[f'{a}_to_{b}'] = (t[b]-t[a])/60
        s['post_spin'] = (t['game']-t['spin'])/60
        if tables['SpinClock']:
            c = tables['SpinClock'].get(f)
            if not c:
                missing.append(f)
            else:
                for k in ('innerGap', 'audioGap', 'prevAge', 'age', 'maxAge', 'ageLate', 'shift', 'clamp'):
                    s[k] = c[k]/60
                s['ppm'] = c['ppm']
                if p['reads'] > 1:
                    assert c['innerGap'] == g['exitGap'] + c['prevAge'] - c['age'], f
        if tables['SpinReturn']:
            r = tables['SpinReturn'].get(f)
            extra = ('entry', 'observeBegin', 'observeEnd', 'callbackEnd')
            if not r or any(not r.get(k) for k in extra):
                missing.append(f)
            else:
                points = dict(t, **r)
                order = ('step',) + extra + ('game',)
                for a, b in zip(order, order[1:]):
                    assert points[b] >= points[a], (f, a, b)
                    s[f'{a}_to_{b}'] = (points[b]-points[a])/60
        if previous and f == previous['f'] + 1:
            s['interval'] = (t['game']-previous['game'])/60
            delta = t['game'] - previous['game']
            scheduled = p['due'] - previous_probe['due']
            late_change = p['exitLate'] - previous_probe['exitLate']
            tail_change = (t['game']-t['spin']) - (previous['game']-previous['spin'])
            mapping_change = (t['spin']-previous['spin']) - scheduled - late_change
            assert delta-1000000 == scheduled-1000000+late_change+tail_change+mapping_change
            s['signed_error'] = (delta-1000000)/60
            s['abs_error'] = abs(s['signed_error'])
            s['schedule_adjustment'] = (scheduled-1000000)/60
            s['residual'] = (delta-scheduled)/60
            s['abs_residual'] = abs(s['residual'])
            s['exit_late_change'] = late_change/60
            s['tail_change'] = tail_change/60
            # QPCと音声時計の速度差、および最終採取から外側QPCまでの時間差。
            s['clock_mapping_change'] = mapping_change/60
        samples.append(s)
        previous = t
        previous_probe = p
    keys = sorted(set().union(*(r.keys() for r in samples)) - {'frame'}) if samples else []
    top = lambda key: sorted(samples, key=lambda r:r[key], reverse=True)[:6]
    return dict(source=str(path.resolve()), first_recovery=first, samples=len(samples),
                incomplete=missing, deferred_drop='[DeferredTraceDropped]' in text,
                stats={key:distribution([s[key] for s in samples if key in s]) for key in keys},
                intervals=[s for s in samples if 'interval' in s],
                top_gap=top('gap'), top_exit_late=top('exitLate'), top_post_spin=top('post_spin'),
                hidden_gap=[s for s in samples if s['exitLate'] > s['exitGap'] + .2])


if __name__ == '__main__':
    root = Path(sys.argv[1])
    result = {str(side):analyze(root/f'game_{side}.log') for side in (1,2)}
    output = json.dumps(result, ensure_ascii=False, indent=2)
    (root/'spin_analysis.json').write_text(output, encoding='utf-8')
    print(output)
