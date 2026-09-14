"""起動CALLサイトの両端QPCを対応付ける。親子の時間は重複するため合算しない。"""
import argparse
import json
from pathlib import Path
import re
from statistics import median


def analyze(path):
    text = path.read_text(encoding='utf-8', errors='replace')
    sites = {int(i): {'site': a, 'target': b} for i, a, b in
             re.findall(r'\[BootSite\] id=(\d+) site=(\w+) target=(\w+)', text)}
    stacks, durations, errors = {}, {}, []
    for line in text.splitlines():
        match = re.search(r'\[BootCall\] (id|site)=(\w+) edge=([01]) qpcUs=(\d+)', line)
        if not match:
            continue
        kind, raw, edge, qpc = match.groups()
        # 初期診断ログはid*2をsiteという名前で16進出力していた。
        key = int(raw) if kind == 'id' else int(raw, 16) // 2
        stack = stacks.setdefault(key, [])
        if edge == '0':
            stack.append(int(qpc))
        elif not stack:
            errors.append(f'id={key}: 開始なし')
        else:
            elapsed = int(qpc) - stack.pop()
            if elapsed < 0:
                errors.append(f'id={key}: 負の時間')
            durations.setdefault(key, []).append(elapsed / 1000)
    errors += [f'id={key}: 終了なし' for key, stack in stacks.items() if stack]
    rows = [{'id': key, **sites[key], 'count': len(values), 'totalMs': sum(values),
             'medianMs': median(values), 'maxMs': max(values)}
            for key, values in durations.items() if key in sites]
    return {'source': str(path), 'calls': sorted(rows, key=lambda row: -row['totalMs']),
            'errors': errors, 'note': '親子の時間は重複。情報収集を省略した場合はそのCALL行はない。'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = analyze(args.log)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    for row in result['calls'][:15]:
        print(f"{row['site']} → {row['target']}: {row['totalMs']:.3f} ms ({row['count']}回)")
    return 1 if result['errors'] or not result['calls'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
