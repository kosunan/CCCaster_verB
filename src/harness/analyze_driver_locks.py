"""ユーザーモードの長いロック取得を集計。カーネル内スピンロックは対象外。"""
import argparse
import json
import re
from collections import Counter
from pathlib import Path
from analyze_gauge_stress import analyze as analyze_gauge, failures as gauge_failures


def analyze(path):
    modules, events, windows, hooks = [], [], [], []
    dropped = 0
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        row = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
        if line.startswith('[DriverLockModule]'):
            row['path'] = line.split('path=', 1)[1]
            modules.append(row)
        elif line.startswith('[DriverLockHook]'):
            row['name'] = re.search(r'name=(\w+)', line)[1]
            hooks.append(row)
        elif line.startswith('[DriverLock]'):
            events.append(row)
        elif line.startswith(('[DrawWork]', '[TextureTransfer]')):
            windows.append(row)
        elif line.startswith('[DriverLockDropped]'):
            dropped += row['count']
    for event in events:
        event['acquire_us'] = (event['end'] - event['begin']) / 60
        module = next((m for m in modules if m['base'] <= event['caller'] < m['base'] + m['size']), None)
        event['module'] = module['path'] if module else None
        event['rva'] = event['caller'] - module['base'] if module else None
        event['other_owner_observed'] = event['kind'] == 1 and event['owner'] not in (0, event['tid'])
        # 時間帯だけで別スレッドの待ちを同じ描画に帰属させない。
        event['draw_frames'] = sorted({w['f'] for w in windows if w['tid'] == event['tid']
            and w['begin'] <= event['begin'] and event['end'] <= w['end']})
    counts = Counter((e['module'], e['rva'], e['kind']) for e in events)
    groups = []
    for (module, rva, kind), count in counts.items():
        rows = [e for e in events if (e['module'], e['rva'], e['kind']) == (module, rva, kind)]
        groups.append(dict(module=module, rva=rva, kind=kind, count=count,
            maximum_us=max(e['acquire_us'] for e in rows),
            other_owner_observed=sum(e['other_owner_observed'] for e in rows)))
    return dict(hooks=hooks, hooks_ok=len(hooks) == 3 and all(h['enable'] == 0 for h in hooks),
        dropped=dropped, modules=modules, events=events,
        groups=sorted(groups, key=lambda x: x['maximum_us'], reverse=True),
        limits='取得APIの実QPC経過。CPU実行時間・所有者の保持時間ではない。CS所有者は取得前の瞬間値。SRW所有者、独自スピンロック、カーネル内部は未観測。モジュールは設置時のスナップショット。')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--check', action='store_true', help='設置失敗・欠落を失敗終了にする')
    args = parser.parse_args()
    result = {str(side): analyze(args.folder / f'game_{side}.log') for side in (1, 2)}
    for side, report in result.items():
        # 診断中は計測負荷を含むため速度合否を付けない。実験条件は検査する。
        report['gauge_failures'] = gauge_failures(analyze_gauge(args.folder / f'game_{side}.log'), maximum_error_us=float('inf'))
    (args.folder / 'driver_locks.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    for side, report in result.items():
        print(f"{side}: hooks={report['hooks_ok']} events={len(report['events'])} dropped={report['dropped']}")
        for group in report['groups'][:8]:
            print(group)
    if args.check and any(not r['hooks_ok'] or r['dropped'] or r['gauge_failures'] for r in result.values()):
        raise SystemExit(1)


if __name__ == '__main__':
    main()
