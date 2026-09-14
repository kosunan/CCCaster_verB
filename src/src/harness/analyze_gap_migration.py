import bisect
import collections
import json
from pathlib import Path
import statistics
import sys

analysis_path, events_path, output_path = map(Path, sys.argv[1:])
data = json.loads(analysis_path.read_text(encoding='utf-8'))
windows = [v for v in data['intervals'] if v['name'] == 'maxGap']
by_tid = collections.defaultdict(list)
for window in sorted(windows, key=lambda v: v['begin']):
    by_tid[window['tid']].append(window)
starts = {tid: [v['begin'] for v in vs] for tid, vs in by_tid.items()}
needles = ['"' + role + '_tid":' + str(tid) + ','
           for tid in by_tid for role in ('old', 'new')]
pending = {}
switches = []
hz = data['metadata']['qpc_hz']
for line in events_path.open(encoding='utf-8'):
    if '"type":"cswitch"' not in line or not any(n in line for n in needles):
        continue
    row = json.loads(line)
    tick = row['qpc'] // hz * 60000000 + row['qpc'] % hz * 60000000 // hz
    tid = row['new_tid']
    if tid in pending:
        old, window = pending.pop(tid)
        if row.get('new_pid') != window['pid']:
            continue
        switches.append(dict(source=window['source'], frame=window['frame'],
            tid=tid, gap_us=window['duration_us'], exit_late_us=window['exit_late_us'],
            status=window['status'], cpu_before=old['cpu'], cpu_after=row['cpu'],
            migrated=old['cpu'] != row['cpu'], wait_reason=old['wait_reason'],
            state=old['state'], new_tid=old['new_tid'], new_priority=old['new_priority'],
            old_priority=old['old_priority'], off_cpu_us=(row['qpc']-old['qpc'])*1000000/hz,
            switch_out_qpc=old['qpc'], switch_in_qpc=row['qpc']))
    tid = row['old_tid']
    if tid in by_tid:
        index = bisect.bisect_right(starts[tid], tick) - 1
        if index >= 0:
            window = by_tid[tid][index]
            if tick < window['end'] and row.get('old_pid') == window['pid']:
                pending[tid] = (row, window)

def stats(values):
    return dict(n=len(values), median=statistics.median(values), maximum=max(values)) if values else dict(n=0)

def summarize(selected, rows):
    return dict(max_gap_count=len(selected), status=dict(collections.Counter(v['status'] for v in selected)),
        windows_with_off_cpu=sum(v['off_cpu_us'] > 0 for v in selected),
        windows_with_interrupt=sum(v['interrupt_us'] > 0 for v in selected),
        matched_switches=len(rows), migrations=sum(v['migrated'] for v in rows),
        windows_with_migration=len({(v['source'],v['frame']) for v in rows if v['migrated']}),
        wait_reasons=dict(collections.Counter(v['wait_reason'] for v in rows)),
        switch_to_idle=sum(v['new_tid']==0 for v in rows),
        off_cpu_us=stats([v['off_cpu_us'] for v in rows]),
        migrated_gap_us=stats([v['gap_us'] for v in rows if v['migrated']]),
        migrated_exit_late_us=stats([v['exit_late_us'] for v in rows if v['migrated']]),
        interrupt_modules=dict(collections.Counter(k for v in selected for k in v['drivers_us'])))

summary = {}
for source in ['all'] + sorted({v['source'] for v in windows}):
    sw = [v for v in windows if source == 'all' or v['source'] == source]
    sr = [v for v in switches if source == 'all' or v['source'] == source]
    summary[source] = {}
    for label, low, high in [('all',0,float('inf')),('ge_10us',10,float('inf')),('10_to_40us',10,40)]:
        summary[source][label] = summarize([v for v in sw if low<=v['duration_us']<=high],
            [v for v in sr if low<=v['gap_us']<=high])
result = dict(analysis=str(analysis_path), events=str(events_path), metadata=data['metadata'],
    note='maxGap windows selected by existing ETW analyzer; one window may contain multiple switches. Reason 30 is WrQuantumEnd in local mingw32/include/ddk/wdm.h. CPU movement is observed; residual on-CPU cost is not individually attributed.',
    summary=summary, switches=switches)
with output_path.open('x', encoding='utf-8') as output:
    json.dump(result, output, ensure_ascii=False, indent=2)
print(json.dumps(summary, ensure_ascii=False, indent=2))
