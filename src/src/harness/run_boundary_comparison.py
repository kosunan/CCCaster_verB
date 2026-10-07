"""境界時計の現行・専用1本・専用4本を同じバイナリ/固定CPUで実対戦比較する。"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re

from analyze_deadline_diagnostics import read, distribution
from run_deadline_diagnostics import run

ROOT = Path(__file__).resolve().parents[3]
ORDER = [(0, 1), (1, 1), (4, 1), (4, 2), (1, 2), (1, 1), (4, 1), (0, 2)]
COMMON = 2500


def environment(workers, side, stall=False):
    peer = 3 - side
    values = {f'CCCASTER_TEST_CPU_PIN_{side}': '2', f'CCCASTER_TEST_CPU_PIN_{peer}': '4',
              f'CCCASTER_BOUNDARY_WORKERS_{side}': str(workers),
              f'CCCASTER_BOUNDARY_WORKERS_{peer}': '0',
              f'CCCASTER_TEST_BOUNDARY_STALL_{side}': '1' if stall else '0',
              f'CCCASTER_TEST_BOUNDARY_STALL_{peer}': '0'}
    if workers:
        values[f'CCCASTER_BOUNDARY_CPUS_{side}'] = '6' if workers == 1 else '6,8,10,12'
    return values


def metric(current, previous, race, previous_race):
    result = dict(game_interval_error_us=abs(current['actual'] - previous['actual'] - 1_000_000)/60)
    if race is None:
        return result
    for row in (race, previous_race):
        if not row or not row['due'] <= row['boundary'] <= row['game'] <= row['received']:
            raise ValueError('境界時刻/受取時刻の順序が不正')
        if not 0 <= row['covered'] <= row['valid'] <= row['requested']:
            raise ValueError('境界の有効本数が不正')
        if row['workers'] != row['requested']:
            raise ValueError('要求した物理CPUの計時スレッドが起動していない')
        if row['winner'] >= 0 and row[f"s{row['winner']}"] != row['boundary']:
            raise ValueError('採用時刻と採時スロットが一致しない')
    if current['actual'] != race['received']:
        raise ValueError('実更新指標を補助の採時へ置き換えている')
    result.update(boundary_late_us=(race['boundary']-race['due'])/60,
                  boundary_interval_error_us=abs(race['boundary']-previous_race['boundary']-1_000_000)/60,
                  schedule_interval_error_us=abs(race['due']-previous_race['due']-1_000_000)/60,
                  handoff_us=(race['received']-race['boundary'])/60,
                  arm_margin_us=(race['due']-race['armed'])/60,
                  helpers_valid=race['valid'], helpers_covered=race['covered'],
                  helper_won=int(race['winner'] >= 0), late_armed=int(race['armed'] >= race['due']))
    return result


def boundary_rows(tables):
    # 新形式は256byteのログ上限に収まるようスロットを別行にする。旧ログも読める。
    slots = {r['f']: r for r in tables['BoundarySlots']}
    return {r['f']: dict(r, **{k: v for k, v in slots.get(r['f'], {}).items() if k != 'f'})
            for r in tables['BoundaryRace']}


def samples(folder, side, workers, confirmed):
    tables = read(folder/f'game_{side}.log')
    if any(row.get('dropped', 0) for row in tables['UpdateCadence']):
        raise ValueError('通常更新ログの欠落')
    settings = tables['BoundaryWorker']
    priority = re.findall(r'\[TimingThread\] role=boundary tid=\d+ MMCSS=1 high=1',
                         (folder/f'game_{side}.log').read_text(encoding='utf-8'))
    if len(priority) != workers:
        raise ValueError('専用時計のMMCSS高優先度が成立していない')
    if workers and (len(settings) != workers or any(row['active'] != 1 for row in settings) or
                    sorted(row['cpu'] for row in settings) != ([6] if workers == 1 else [6, 8, 10, 12])):
        raise ValueError('計時CPUの配置が比較条件と異なる')
    pins = tables['GameCpuPin']
    if len(pins) != 1 or pins[0]['cpu'] != 2 or pins[0]['enabled'] != 1:
        raise ValueError('測定ゲームCPUが2に固定されていない')
    releases = {r['f']: r for r in tables['FrameStart']}
    races = boundary_rows(tables)
    pipelines = {r['f']: r for r in tables['FramePipeline']}
    values, tail = {}, {}
    for update in tables['UpdateCadence']:
        if not update['consecutive'] or not update['play']:
            continue
        f, prev = update['f'], update['prev']
        row = metric(releases[f], releases[prev], races.get(f), races.get(prev))
        if workers and f not in races:
            raise ValueError('境界時計の採取がない')
        if (update['interval'] != releases[f]['actual']-releases[prev]['actual'] or
                row['game_interval_error_us'] != abs(update['error'])/60):
            raise ValueError('ゲーム実更新の差分が不一致')
        if f in pipelines and prev in pipelines and pipelines[f]['native'] and pipelines[prev]['native']:
            row['native_interval_error_us'] = abs(pipelines[f]['native']-pipelines[prev]['native']-1_000_000)/60
        if f <= int(confirmed.get(str(f//65536), 0)):
            values[f] = row
        else:
            tail[f] = row
    stalls = tables['BoundaryStall']
    if any(not s['entered'] < s['due'] < s['resumed'] for s in stalls):
        raise ValueError('故障注入が締切を跨いでいない')
    stall_due = {s['due'] for s in stalls}
    affected = [dict(frame=f, **row) for f, row in races.items()
                if row['due'] in stall_due and f in values]
    return values, dict(unconfirmed_tail=tail, stalls=stalls, affected=affected, settings=settings)


def summarize(rows):
    rows = list(rows)
    if not rows:
        raise ValueError('比較標本がない')
    keys = set().union(*(row.keys() for row in rows))
    result = dict(samples=len(rows), stats={key: distribution([r.get(key) for r in rows]) for key in sorted(keys)})
    result['over_3us'] = {key: sum(r.get(key, 0) > 3 for r in rows) for key in keys if key.endswith('_us') and
                         ('error' in key or key == 'boundary_late_us')}
    result['late_armed'] = sum(r.get('late_armed', 0) for r in rows)
    result['helper_wins'] = sum(r.get('helper_won', 0) for r in rows)
    return result


def collect(output, plan):
    measured, reports = [], []
    hashes = None
    for index, (workers, side) in enumerate(plan['order'], 1):
        folder = output/f'{index:02d}_workers{workers}_side{side}'
        result = json.loads((folder/'result.json').read_text(encoding='utf-8'))
        if not all(result.get(k) for k in ('passed', 'protected_unchanged', 'binaries_unchanged')):
            raise ValueError(f'実ゲーム不合格: {folder}')
        if hashes is not None and result['binaries'] != hashes:
            raise ValueError('比較中にバイナリが変わった')
        hashes = result['binaries']
        values, extra = samples(folder, side, workers, result['sync']['confirmed_by_epoch'])
        if not plan.get('stall') and extra['stalls']:
            raise ValueError('通常比較に故障注入が混入した')
        measured.append(values)
        reports.append(dict(run=index, workers=workers, side=side, all_samples=summarize(values.values()), **extra))
    common = sorted(set.intersection(*(set(m) for m in measured)))
    required = plan['common_samples']
    if len(common) < required:
        raise ValueError(f'共通標本不足: {len(common)} < {required}')
    selected = common[:required]
    pooled, all_pooled = defaultdict(list), defaultdict(list)
    for report, values in zip(reports, measured):
        rows = [values[f] for f in selected]
        report['common_samples'] = summarize(rows)
        pooled[str(report['workers'])].extend(rows)
        all_pooled[str(report['workers'])].extend(values.values())
    return dict(passed=True, meaning='採取条件・同期・保全の成立。全F精度達成や改善の判定とは別。',
                binaries=hashes, common_available=len(common), selected_frames=selected, runs=reports,
                pooled={key:summarize(rows) for key,rows in pooled.items()},
                all_pooled={key:summarize(rows) for key,rows in all_pooled.items()})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--plan', choices=('compare', 'sync', 'fault'), default='compare')
    parser.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    if args.analyze_only:
        plan = json.loads((output/'plan.json').read_text(encoding='utf-8'))
    else:
        output.mkdir(parents=True, exist_ok=False)
        plan = dict(order=ORDER if args.plan == 'compare' else [(4, 1)] if args.plan == 'sync' else [(1, 1), (4, 1)],
                    seconds=80 if args.plan == 'compare' else 55, common_samples=COMMON if args.plan == 'compare' else 1000,
                    network='15,25,5' if args.plan == 'sync' else None, stall=args.plan == 'fault',
                    fixed_stage=59, measured_cpu=2, peer_cpu=4, helper_cpus=[6,8,10,12],
                    spin_guard_us=2000, etw=False, cpu_sampling=False, detailed_probe=False)
        (output/'plan.json').write_text(json.dumps(plan, indent=2), encoding='utf-8')
        paths = [p for p in (ROOT/'src').rglob('*') if p.is_file() and p.suffix in ('.cpp','.hpp','.h','.py','.ps1') and
                 '__pycache__' not in str(p)]
        sources = {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}
        (output/'sources.json').write_text(json.dumps(sources, indent=2), encoding='utf-8')
        for index, (workers, side) in enumerate(plan['order'], 1):
            print(f'境界時計 {index}/{len(plan["order"])}: 専用{workers}本、測定側{side}', flush=True)
            if run(output/f'{index:02d}_workers{workers}_side{side}', plan['seconds'], fixed_stage=59,
                   extra_env=environment(workers, side, plan['stall']), network=plan['network']):
                raise SystemExit('実ゲーム不合格。結果を保持して停止')
        if any(hashlib.sha256((ROOT/p).read_bytes()).hexdigest() != h for p,h in sources.items()):
            raise SystemExit('試験中に製品/判定器が変更された')
    report = collect(output, plan)
    (output/'comparison.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(report['pooled'], ensure_ascii=False, indent=2), flush=True)


if __name__ == '__main__':
    main()
