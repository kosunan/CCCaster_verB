"""入力公開・採取の試作を同じDLL・CPU・確定Fで比較する。旧3箇所のログ解析にも対応。"""
import argparse
import json
from pathlib import Path
from analyze_deadline_diagnostics import read, distribution
from run_boundary_comparison import environment, boundary_rows
from run_deadline_diagnostics import run

NORMAL = ['baseline', 'capture', 'publication', 'all', 'baseline']


def trial_environment(mode, fault=False):
    if mode not in NORMAL:
        raise ValueError('Present試作は撤去済み。旧ログは--analyze-onlyで解析する')
    env = environment(4, 1)
    env['CCCASTER_DISABLE_MONITOR_PRESENT'] = '1'  # Present直前の既存待機を実際に通す。
    for name in ('publication', 'capture'):
        env[f'CCCASTER_SPIN_{name.upper()}_1'] = '1' if mode in (name, 'all') else '0'
        env[f'CCCASTER_SPIN_{name.upper()}_2'] = '0'
    env['CCCASTER_TEST_CAPTURE_STALL_1'] = '1' if fault else '0'
    env['CCCASTER_TEST_CAPTURE_STALL_2'] = '0'
    return env


def measure(tables, confirmed, mode, fault, present_removed=False):
    if not fault and (tables['CaptureStall'] or tables['BoundaryStall']):
        raise ValueError('通常比較に故障注入が混入した')
    workers = tables['BoundaryWorker']
    if len(workers) != 4 or sorted(r['cpu'] for r in workers) != [6, 8, 10, 12] or any(not r['active'] for r in workers):
        raise ValueError('比較用4物理コアの配置が不成立')
    if any(r.get('dropped', 0) for r in tables['UpdateCadence']) or tables['DeferredTraceDropped']:
        raise ValueError('計測ログの欠落')
    capture, display = ({r['f']: r for r in tables[key]} for key in ('CaptureStage', 'DisplayPace'))
    boundaries = boundary_rows(tables)
    rows = {}
    for update in tables['UpdateCadence']:
        f = update['f']
        if not update['play'] or not update['consecutive'] or f > int(confirmed.get(str(f // 65536), 0)):
            continue
        if f not in capture or f not in display or f not in boundaries:
            continue
        b = boundaries[f]
        if not b['due'] <= b['boundary'] <= b['game'] <= b['received']:
            raise ValueError('境界時刻と実処理時刻が逆転')
        rows[f] = dict(capture_late_us=capture[f]['late'], capture_lock_us=capture[f]['lock'],
                       present_late_us=display[f]['late'], actual_interval_error_us=abs(update['error']) / 60,
                       boundary_late_us=(b['boundary'] - b['due']) / 60)
    extras = {}
    for name in ('Present', 'Publication'):
        data = [r for r in tables[f'{name}Race'] if r['f'] in rows]
        expected = mode in (name.lower(), 'all') and not (name == 'Present' and present_removed)
        if expected and len(data) < 500:
            raise ValueError(f'{name}の実通過が不足: {len(data)}')
        if not expected and tables[f'{name}Race']:
            raise ValueError(f'{name}の無効化が不成立')
        if any(r['observed'] > r['actual'] or r['workers'] != 4 for r in data):
            raise ValueError('公開/Presentの採時または配置が不正')
        extras[name.lower()] = dict(samples=len(data), helper_wins=sum(r['winner'] >= 0 for r in data),
            observed_to_actual_us=distribution([(r['actual'] - r['observed']) / 60 for r in data]))
    captures = [r for r in tables['CaptureRace'] if r['f'] in rows]
    if mode in ('capture', 'all'):
        if len(captures) < 1000 or len({r['f'] for r in captures}) != len(captures):
            raise ValueError('補助採取の欠落/二重採取')
        if any(r['captured'] < r['due'] for r in captures):
            raise ValueError('入力を締切前に採取した')
    extras['capture'] = dict(samples=len(captures), helper_wins=sum(r['worker'] >= 0 for r in captures),
                            late_us=distribution([(r['captured'] - r['due']) / 60 for r in captures]))
    affected = [r['f'] for r in tables['CaptureStall'] if r['f'] in rows]
    if fault and len(affected) < 15:
        raise ValueError(f'故障注入の確定戦闘標本が不足: {len(affected)}')
    extras['fault'] = dict(samples=len(affected), capture_late_us=distribution([rows[f]['capture_late_us'] for f in affected]))
    return rows, extras


def summarize(rows):
    return {key: distribution([r[key] for r in rows]) for key in rows[0]}


def collect(output, plan):
    reports, samples, hashes = [], [], None
    for index, mode in enumerate(plan['order'], 1):
        folder = output / f'{index:02d}_{mode}'
        result = json.loads((folder / 'result.json').read_text(encoding='utf-8'))
        if not all(result.get(k) for k in ('passed', 'protected_unchanged', 'binaries_unchanged')):
            raise ValueError(f'実ゲーム不合格: {folder}')
        if hashes is not None and hashes != result['binaries']:
            raise ValueError('比較途中でバイナリが変化')
        hashes = result['binaries']
        rows, extras = measure(read(folder / 'game_1.log'), result['sync']['confirmed_by_epoch'], mode, plan['fault'],
                               present_removed=plan.get('present_removed', False))
        samples.append(rows)
        reports.append(dict(mode=mode, folder=str(folder), confirmed=result['sync']['confirmed_by_epoch'], **extras))
    common = sorted(set.intersection(*(set(s) for s in samples)))
    if len(common) < plan['common_samples']:
        raise ValueError(f'共通確定標本の不足: {len(common)}')
    selected = common[:plan['common_samples']]
    for report, rows in zip(reports, samples):
        report['same_frames'] = summarize([rows[f] for f in selected])
        report['all_frames'] = summarize(list(rows.values()))
    result = dict(passed=True, plan=plan, selected_frames=selected, binaries=hashes, trials=reports)
    (output / 'comparison.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--fault', action='store_true')
    parser.add_argument('--analyze-only', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    if args.analyze_only:
        plan = json.loads((output / 'plan.json').read_text(encoding='utf-8'))
    else:
        output.mkdir(parents=True, exist_ok=False)
        plan = dict(order=['baseline', 'all'] if args.fault else NORMAL, seconds=60, common_samples=1000, present_removed=True,
                    fault=args.fault, measured_side=1, game_cpu=2, peer_game_cpu=4, helper_cpus=[6, 8, 10, 12])
        (output / 'plan.json').write_text(json.dumps(plan, indent=2), encoding='utf-8')
        for index, mode in enumerate(plan['order'], 1):
            if run(output / f'{index:02d}_{mode}', plan['seconds'], extra_env=trial_environment(mode, args.fault)):
                return 1
    result = collect(output, plan)
    print(json.dumps(dict(passed=result['passed'], trials=len(result['trials']), logs=str(output)), ensure_ascii=False))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
