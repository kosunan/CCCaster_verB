"""final起動比較12試行だけを検証し、到達時間のJSONと表を出力する。"""
import argparse
import json
from pathlib import Path
from statistics import median
import sys


METRICS = {
    'worker_ready_ms': 'worker→入力/表示完了',
    'game_launch_ready_ms': 'game launch→完了',
    'negotiation_ms': '接続→交渉確定',
    'menu_to_detect_ms': 'mode25→選択検出',
}


def statistics(values):
    return {'count': len(values), 'median': median(values), 'min': min(values), 'max': max(values)}


def summarize(root):
    trials, errors = [], []
    # globや再帰探索で旧比較・途中実験を取り込まない。
    for mode in ('training', 'versus'):
        for variant in ('baseline', 'fast'):
            for repeat in range(1, 4):
                name = f'final_{mode}_{variant}_{repeat}'
                path = root / name / 'result.json'
                try:
                    result = json.loads(path.read_text(encoding='utf-8-sig'))
                    count = 1 if mode == 'training' else 2
                    if result.get('status') != 'ready':
                        raise ValueError(f"status={result.get('status')!r}")
                    if result.get('error') or result.get('cleanupErrors'):
                        raise ValueError('実行またはcleanupエラーあり')
                    if (result.get('mode'), result.get('variant'), result.get('instances')) != (mode, variant, count):
                        raise ValueError('mode/variant/instancesがディレクトリ名と不一致')
                    samples, launches = result['samples'], result['launchQpcUs']
                    if len(samples) != count or len(launches) != count:
                        raise ValueError('side数またはlaunchQpcUs数が不正')
                    if sorted(s['side'] for s in samples) != list(range(1, count + 1)):
                        raise ValueError('side番号が欠落または重複')
                    sides = []
                    for sample in samples:
                        side = sample['side']
                        if sample.get('validation', {}).get('ready') is not True:
                            raise ValueError(f'side{side}: validation.readyではない')
                        events = {}
                        for event in sample['events']:
                            key = event['event']
                            # 同一イベントの再記録がある場合は最初の到達を使う。
                            if key not in events or event['qpcUs'] < events[key]['qpcUs']:
                                events[key] = event
                        required = ['launch', 'chara_input', 'chara_present', 'mode_25', 'chara_detect']
                        if mode == 'versus':
                            required += ['negotiation_connected', 'negotiation_locked']
                        for key in required:
                            if key not in events:
                                raise ValueError(f'side{side}: event={key}なし')
                            event = events[key]
                            if type(event.get('qpcUs')) is not int or type(event.get('fromLaunchUs')) is not int:
                                raise ValueError(f'side{side}: event={key}時刻が整数ではない')
                            if event['qpcUs'] - launches[side - 1] != event['fromLaunchUs']:
                                raise ValueError(f'side{side}: event={key}のworker起点が不一致')
                        ready = max(events['chara_input']['qpcUs'], events['chara_present']['qpcUs'])
                        metrics = {
                            'worker_ready_ms': max(events['chara_input']['fromLaunchUs'],
                                                   events['chara_present']['fromLaunchUs']) / 1000,
                            'game_launch_ready_ms': (ready - events['launch']['qpcUs']) / 1000,
                            'menu_to_detect_ms': (events['chara_detect']['qpcUs'] - events['mode_25']['qpcUs']) / 1000,
                            'negotiation_ms': ((events['negotiation_locked']['qpcUs'] -
                                                events['negotiation_connected']['qpcUs']) / 1000
                                               if mode == 'versus' else None),
                        }
                        if any(value is not None and value < 0 for value in metrics.values()):
                            raise ValueError(f'side{side}: イベントの時系列が逆転')
                        sides.append({'side': side, 'ready_qpc_us': ready, **metrics})
                    trial = {'name': name, 'mode': mode, 'variant': variant, 'repeat': repeat,
                             'source': str(path.resolve()), 'sides': sorted(sides, key=lambda s: s['side'])}
                    if mode == 'versus':
                        trial['pair_ready_ms'] = (max(s['ready_qpc_us'] for s in sides) - min(launches)) / 1000
                    trials.append(trial)
                except (OSError, ValueError, KeyError, TypeError, IndexError) as exc:
                    errors.append({'trial': name, 'error': str(exc)})
    groups = []
    for mode in ('training', 'versus'):
        for variant in ('baseline', 'fast'):
            selected = [t for t in trials if t['mode'] == mode and t['variant'] == variant]
            sides = [s for t in selected for s in t['sides']]
            group = {'mode': mode, 'variant': variant, 'trial_count': len(selected),
                     'side_count': len(sides), 'metrics': {}, 'by_side': {}}
            for key in METRICS:
                values = [s[key] for s in sides if s[key] is not None]
                if values:
                    group['metrics'][key] = statistics(values)
            for side in (1,) if mode == 'training' else (1, 2):
                group['by_side'][str(side)] = {}
                for key in METRICS:
                    values = [s[key] for s in sides if s['side'] == side and s[key] is not None]
                    if values:
                        group['by_side'][str(side)][key] = statistics(values)
            if mode == 'versus' and selected:
                group['pair_ready_ms'] = statistics([t['pair_ready_ms'] for t in selected])
            groups.append(group)
    return {'complete': not errors and len(trials) == 12, 'expected_trials': 12,
            'valid_trials': len(trials), 'units': 'ms', 'groups': groups, 'trials': trials, 'errors': errors,
            'definitions': {'ready': 'max(chara_input.qpcUs, chara_present.qpcUs)',
                            'pair_ready_ms': '(max(両side ready) - min(launchQpcUs))/1000',
                            'group': 'Training各variant=3 side、Versus各variant=6 side。pairは各3試行。',
                            'note': 'API表示要求・入力経路・モード検証まで。物理表示や手入力応答ではない。'}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path, help='既定は入力ディレクトリ/startup_summary.json')
    args = parser.parse_args()
    result = summarize(args.directory)
    output = args.output or args.directory / 'startup_summary.json'
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(f"{'完了' if result['complete'] else '未完・不合格'}: {result['valid_trials']}/12試行  JSON: {output}")
    print('|モード|条件|指標|n|中央値 ms|min–max ms|')
    print('|---|---|---|---:|---:|---:|')
    for group in result['groups']:
        rows = [(METRICS[key], value) for key, value in group['metrics'].items()]
        if 'pair_ready_ms' in group:
            rows.append(('両側完了・最初のworker起点', group['pair_ready_ms']))
        for label, value in rows:
            print(f"|{group['mode']}|{group['variant']}|{label}|{value['count']}|{value['median']:.3f}|"
                  f"{value['min']:.3f}–{value['max']:.3f}|")
    for error in result['errors']:
        print(f"{error['trial']}: {error['error']}", file=sys.stderr)
    return 0 if result['complete'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
