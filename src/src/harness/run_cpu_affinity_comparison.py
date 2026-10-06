"""同じバイナリの通常対戦で、1CPU自動固定と固定解除を順序交代して比較する。"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re

from analyze_deadline_diagnostics import analyze, distribution, read, release_metrics
from run_deadline_diagnostics import run


ORDER = ('pinned', 'unpinned', 'unpinned', 'pinned', 'pinned', 'unpinned')
COMMON_SAMPLES = 2500


def affinity(text, policy):
    guards = [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines()
              if line.startswith('[GameCpuGuard]')]
    if len(guards) != 1:
        raise ValueError('ゲームスレッドのCPU設定が一意でない')
    guard = guards[0]
    mask = int(guard['chosen'], 16)
    expected = int(guard['before'], 16) & int(guard['process'], 16) & ~int(guard['exclude'], 16)
    if guard['disabled'] != '0' or guard['error'] != '0' or not mask or mask & ~expected:
        raise ValueError(f'CPU除外設定が不正: {guard}')
    pins = [dict(re.findall(r'(\w+)=(\S+)', line)) for line in text.splitlines()
            if line.startswith('[GameCpuPin]')]
    if policy == 'pinned':
        if (mask.bit_count() != 1 or len(pins) != 1 or pins[0]['enabled'] != '1' or
                pins[0]['auto'] != '1' or pins[0]['error'] != '0'):
            raise ValueError('現行の1CPU自動固定が適用されていない')
    elif mask != expected or mask.bit_count() <= 1 or pins:
        raise ValueError('固定解除が適用されていない')
    if not re.search(r'\[TimingThread\] role=game tid=\d+ MMCSS=1 high=1', text):
        raise ValueError('MMCSSの高優先度設定が適用されていない')
    return dict(mask=f'{mask:x}', allowed_cpus=[i for i in range(32) if mask & (1 << i)],
                mmcss=True, high=True)


def frame_metrics(path):
    tables = read(path)
    releases = {r['f']: r for r in tables['ReleaseGate']}
    values = {}
    for u in tables['UpdateCadence']:
        if not u['consecutive'] or not u['play']:
            continue
        r = releases[u['f']]
        p = releases[u['prev']]
        current, previous = release_metrics(r), release_metrics(p)
        valid = not current['deadline_was_clamped'] and not previous['deadline_was_clamped']
        values[u['f']] = dict(interval_us=u['interval']/60,
            abs_error_us=abs(u['error'])/60, late_us=current['release_late_us'],
            ready_margin_us=current['ready_margin_us'], ready_late_audio_us=current['ready_late_audio_us'],
            schedule_error_us=(r['due']-p['due']-1_000_000)/60 if valid else None)
    return values


def summarize(values):
    rows = list(values)
    result = {key: distribution([r[key] for r in rows]) for key in rows[0]}
    result.update(samples=len(rows), outside_3us=sum(r['abs_error_us'] > 3 for r in rows),
                  late_over_3us=sum(r['late_us'] is not None and r['late_us'] > 3 for r in rows),
                  ready_after_deadline=sum(r['ready_margin_us'] < 0 for r in rows))
    result['outside_3us_percent'] = result['outside_3us']*100/len(rows)
    return result


def confirmed_metrics(values, confirmed):
    return {f: row for f, row in values.items() if f <= int(confirmed.get(str(f//65536), 0))}


def collect(output):
    reports = []
    metrics = []
    hashes = None
    for index, policy in enumerate(ORDER, 1):
        folder = output/f'{index:02d}_{policy}'
        result = json.loads((folder/'result.json').read_text(encoding='utf-8'))
        if not all(result.get(k) for k in ('passed', 'protected_unchanged', 'binaries_unchanged')):
            raise ValueError(f'実対戦が不合格: {folder}')
        if hashes is None:
            hashes = result['binaries']
        if result['binaries'] != hashes or result['cpu_policy'] != policy or result['fixed_stage'] != 59:
            raise ValueError('比較条件・バイナリが一致しない')
        for side in (1, 2):
            path = folder/f'game_{side}.log'
            a = analyze(path)
            if a['dropped']:
                raise ValueError('更新計測の欠落あり')
            (folder/f'deadline_{side}.json').write_text(json.dumps(a, indent=2), encoding='utf-8')
            settings = affinity(path.read_text(encoding='utf-8'), policy)
            raw_metric = frame_metrics(path)
            metric = confirmed_metrics(raw_metric, result['sync']['confirmed_by_epoch'])
            metrics.append(metric)
            reports.append(dict(run=index, policy=policy, side=side, affinity=settings,
                                all_samples=summarize(metric.values()),
                                raw_samples=summarize(raw_metric.values()),
                                unconfirmed_tail=[dict(frame=f, **row) for f, row in raw_metric.items() if f not in metric]))
    common = sorted(set.intersection(*(set(m) for m in metrics)))
    if len(common) < COMMON_SAMPLES:
        raise ValueError(f'共通の通常更新が不足: {len(common)} < {COMMON_SAMPLES}')
    selected = common[:COMMON_SAMPLES]
    pooled = defaultdict(list)
    all_pooled = defaultdict(list)
    for report, metric in zip(reports, metrics):
        values = [metric[f] for f in selected]
        report['common_samples'] = summarize(values)
        pooled[report['policy']].extend(values)
        all_pooled[report['policy']].extend(metric.values())
    return dict(passed=True, binaries=hashes, order=ORDER, common_available=len(common),
                measurement_scope='双方確定済み・イントロ除外・同世代の連続通常更新。終了後の片側未確定フレームは別記。',
                selected_frames=selected, runs=reports,
                pooled={k: summarize(v) for k, v in pooled.items()},
                all_pooled={k: summarize(v) for k, v in all_pooled.items()})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    if not args.analyze_only:
        output.mkdir(parents=True, exist_ok=False)
        root = Path(__file__).resolve().parents[3]
        paths = [p for p in (root/'src/src/core_dll').rglob('*') if p.is_file() and p.suffix in ('.cpp', '.hpp', '.h')]
        hashes = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
        (output/'product_sources.json').write_text(json.dumps(hashes, indent=2), encoding='utf-8')
        (output/'plan.json').write_text(json.dumps(dict(order=ORDER, seconds=80, fixed_stage=59,
            common_samples=COMMON_SAMPLES, etw=False, detailed_probe=False), indent=2), encoding='utf-8')
        for index, policy in enumerate(ORDER, 1):
            print(f'比較 {index}/{len(ORDER)}: {policy}', flush=True)
            if run(output/f'{index:02d}_{policy}', 80, cpu_policy=policy, fixed_stage=59):
                raise SystemExit('実対戦が不合格。ログを保持して停止')
        if any(hashlib.sha256((root/p).read_bytes()).hexdigest() != value for p, value in hashes.items()):
            raise SystemExit('比較中に製品ソースが変更された')
    report = collect(output)
    (output/'comparison.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report['pooled'], ensure_ascii=False, indent=2), flush=True)
