"""完成画像の提示とゲーム進行を分けて集計する。物理パネルの表示計測ではない。"""
import argparse
import json
import re
import statistics
from pathlib import Path


def percentile(values, fraction):
    return sorted(values)[round((len(values) - 1) * fraction)] if values else None


def analyze(text, minimum_seconds=5):
    errors, rows, groups, group = [], [], [], []
    hz = None
    for line in text.splitlines():
        rate = re.search(r'\[MonitorRefresh\] active=1 hz=(\d+)/(\d+)', line)
        if rate:
            hz = int(rate[1]) / int(rate[2])
            if group:
                groups.append(group)
                group = []
        if line.startswith('[MonitorPresent] failed'):
            errors.append(line)
        if not line.startswith('[MonitorPresent] seq='):
            continue
        row = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
        required = {'seq', 'image', 'frame', 'qpc', 'cost', 'repeat', 'missed', 'mode', 'intro', 'world'}
        if set(row) != required or hz is None:
            errors.append('表示記録・モニター周期が不足')
            continue
        if rows:
            previous = rows[-1]
            if row['seq'] != previous['seq'] + 1 or row['qpc'] <= previous['qpc']:
                errors.append('表示記録の欠落・時計逆行')
            if row['repeat'] != int(row['image'] == previous['image']):
                errors.append('画像の反復判定が不一致')
        row['hz'] = hz
        rows.append(row)
        battle = row['mode'] == 1 and row['intro'] == 0
        if group and (not battle or row['world'] < group[-1]['world'] or
                      row['frame'] // 65536 != group[-1]['frame'] // 65536):
            groups.append(group)
            group = []
        if battle:
            group.append(row)
    if group:
        groups.append(group)
    result = dict(passed=False, errors=errors, presents=len(rows),
                  repeats=sum(r['repeat'] for r in rows), measurement='D3D9 Present request, not physical scanout')
    if not groups:
        errors.append('通常戦闘の表示記録なし')
        return result
    # 通常戦闘の最長連続区間。初めの1秒は開始合意・追いつき直後なので別枠にする。
    longest = max(groups, key=lambda g: g[-1]['qpc'] - g[0]['qpc'])
    steady = [r for r in longest if r['qpc'] >= longest[0]['qpc'] + 60000000]
    if len(steady) < 2:
        errors.append('連続表示区間不足')
        return result
    seconds = (steady[-1]['qpc'] - steady[0]['qpc']) / 60000000
    intervals = [(b['qpc'] - a['qpc']) / 60 for a, b in zip(steady, steady[1:])]
    error_us = [abs(i - 1000000 / steady[0]['hz']) for i in intervals]
    fps = (len(steady) - 1) / seconds
    game_hz = (steady[-1]['world'] - steady[0]['world']) / seconds
    result['steady'] = dict(seconds=seconds, presents=len(steady), requested_hz=steady[0]['hz'],
                            present_fps=fps, world_hz=game_hz,
                            repeated=sum(r['repeat'] for r in steady),
                            missed=steady[-1]['missed'] - steady[0]['missed'],
                            interval_median_us=statistics.median(intervals),
                            interval_max_us=max(intervals), error_p99_us=percentile(error_us, .99),
                            present_cost_p99_us=percentile([r['cost'] / 60 for r in steady], .99))
    if seconds < minimum_seconds:
        errors.append('測定時間不足')
    if abs(fps / steady[0]['hz'] - 1) > .03:
        errors.append('平均表示回数が指定Hzの±3%外')
    if abs(game_hz / 60 - 1) > .03:
        errors.append('ゲーム進行が60Hzの±3%外')
    result['passed'] = not errors
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    args = parser.parse_args()
    result = {p.name: analyze(p.read_text(encoding='utf-8')) for p in sorted(args.folder.glob('game_*.log'))}
    output = args.folder / 'monitor_timing.json'
    output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result and all(x['passed'] for x in result.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
