"""実更新の締切・周期・準備余裕を分離し、最終解放の遅れをETWへ対応づける。"""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import re
import statistics

from analyze_spin_etw import Trace


def distribution(values):
    values = sorted(v for v in values if v is not None)
    if not values:
        return dict(count=0)
    return dict(count=len(values), minimum=values[0], median=statistics.median(values),
                p99=values[math.ceil(len(values)*.99)-1], maximum=values[-1])


def read(path):
    tables = defaultdict(list)
    for line in path.read_text(encoding='utf-8').splitlines():
        match = re.match(r'\[(\w+)\]', line)
        if match:
            row = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)(?=\s|$)', line)}
            tables[match[1]].append(row)
    return tables


def release_metrics(row):
    # DeadlineTicksはtargetを過ぎるとnowを返す。due==readyを「遅着なし」と
    # 誤認しない。元の締切が失われたQPC区間は推測で再構成しない。
    ready_late = row.get('readyLate', row['ready']-row['due'])
    clamped = ready_late > 0 and row['due'] == row['ready']
    gate_late = (row['actual']-row['due'])/60
    return dict(ready_late_audio_us=ready_late/60, ready_margin_us=-ready_late/60,
                deadline_was_clamped=clamped, gate_exit_late_us=gate_late,
                release_late_us=None if clamped else gate_late)


def analyze(path, trace=None):
    tables = read(path)
    releases = {s['f']: s for s in tables['ReleaseGate']}
    updates = [s for s in tables['UpdateCadence'] if s['consecutive'] and s['play']]
    frames = {s['f'] for s in updates}
    rows = []
    for s in updates:
        if s['f'] not in releases or s['prev'] not in releases:
            raise ValueError(f'解放境界ログ不足: {path} f={s["f"]}')
        current, previous = releases[s['f']], releases[s['prev']]
        actual = current['actual'] - previous['actual']
        scheduled = current['due'] - previous['due']
        late_change = (current['actual']-current['due']) - (previous['actual']-previous['due'])
        if actual != s['interval'] or actual != scheduled + late_change:
            raise ValueError(f'異なる計測境界が混在: {path} f={s["f"]}')
        metrics = release_metrics(current)
        valid_projection = not metrics['deadline_was_clamped'] and not release_metrics(previous)['deadline_was_clamped']
        rows.append(dict(frame=s['f'], interval_us=actual/60, error_us=(actual-1_000_000)/60,
                         scheduled_us=scheduled/60 if valid_projection else None,
                         late_change_us=late_change/60 if valid_projection else None,
                         **metrics,
                         ready=current['ready'], due=current['due'], actual=current['actual']))
    result = dict(source=str(path.resolve()), samples=len(rows),
                  ready_margin_clock='WASAPI projection at arrival; legacy logs fall back to QPC',
                  release_late_scope='unclamped QPC deadlines only; late arrivals are recorded separately',
                  dropped=max((s['dropped'] for s in tables['UpdateCadence']), default=0),
                  stats={key: distribution([s[key] for s in rows]) for key in
                         ('interval_us', 'error_us', 'scheduled_us', 'late_change_us',
                          'release_late_us', 'ready_margin_us', 'ready_late_audio_us', 'gate_exit_late_us')},
                  interval_outside_3us=sum(abs(s['error_us']) > 3 for s in rows),
                  ready_after_deadline=sum(s['ready_margin_us'] < 0 for s in rows),
                  clamped_deadlines=sum(s['deadline_was_clamped'] for s in rows),
                  work_us=distribution([s['work'] for s in tables['Pace'] if s['f'] in frames]),
                  snapshot_us=distribution([s['save'] for s in tables['SnapshotStage'] if s['f'] in frames]),
                  sound_us=distribution([s['ticks']/60 for s in tables['SoundProbe']
                                         if s['f'] in frames and not s['replay']]),
                  top_late=sorted(rows, key=lambda s:max(s['release_late_us'] or 0, s['ready_late_audio_us']), reverse=True)[:10],
                  top_error=sorted(rows, key=lambda s:abs(s['error_us']), reverse=True)[:10])
    if trace:
        ids = {(s['pid'], s['tid']) for s in tables['SpinOS']}
        if len(ids) != 1:
            raise ValueError(f'ゲームスレッドのPID/TIDが一意でない: {ids}')
        pid, tid = ids.pop()
        result.update(pid=pid, tid=tid)
        windows = []
        # 準備スピンと別の、native LeaveCriticalSection直後の最終200µs待機を照合。
        for s in rows:
            if s['deadline_was_clamped']:
                # 本来のQPC締切は記録されていない。到着後だけを帰属し、遅着を隠さない。
                windows.append(trace.attribute(dict(name='late_arrival_tail', frame=s['frame'], pid=pid, tid=tid,
                    begin=s['ready'], end=s['actual'], ready_late_audio_us=s['ready_late_audio_us'])))
                continue
            if s['release_late_us'] < 1 and s['ready_margin_us'] >= 0:
                continue
            for name, begin in (('release_late', s['due']), ('release_gate', min(s['due'], s['ready']))):
                window = dict(name=name, frame=s['frame'], pid=pid, tid=tid, begin=begin, end=s['actual'])
                windows.append(trace.attribute(window))
        result['etw_windows'] = windows
    return result


def load_trace(path, logs):
    tids = {s['tid'] for log in logs for s in read(log)['SpinOS']}
    if not tids:
        raise ValueError('ETW照合にはSpinOSのPID/TID記録が必要です')
    records = []
    with path.open(encoding='utf-8-sig') as stream:
        for line in stream:
            row = json.loads(line)
            kind = row['type']
            # 全PCのstack/CSwitchを保持するとGB単位になる。割込みは全CPUを保持し、
            # 後段で対象スレッドが実際に動いたCPUだけを突き合わせる。
            if (kind in ('meta', 'header', 'dpc', 'isr') or
                (kind == 'cswitch' and (row['old_tid'] in tids or row['new_tid'] in tids)) or
                (kind == 'thread' and row['tid'] in tids) or
                (kind == 'image' and row['pid'] in (0, 4))):
                records.append(row)
    return Trace(records)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--etw', type=Path)
    args = parser.parse_args()
    trace = None
    if args.etw:
        trace = load_trace(args.etw, [args.folder/f'game_{side}.log' for side in (1, 2)])
    report = dict(etw_metadata=trace.metadata if trace else None,
                  sides={str(side): analyze(args.folder/f'game_{side}.log', trace) for side in (1, 2)})
    target = args.folder / 'deadline_analysis.json'
    target.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(output=str(target), samples={k:v['samples'] for k,v in report['sides'].items()})))
