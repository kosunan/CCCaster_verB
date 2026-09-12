"""イントロの確定状態と通常更新周期を、intro値ごとに分離して検査する。"""
import argparse
import collections
import json
import re
from pathlib import Path
from compare_rollback_pair import compare


def read(path):
    states, rollbacks, cadence = {}, collections.Counter(), collections.defaultdict(list)
    previous = None
    current = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        if line.startswith('[INTRO] '):
            values = list(map(int, line.split()[1:]))
            states[values[0]] = values[1:]
        elif line.startswith('[FRAME] '):
            values = list(map(int, line.split()[1:]))
            current[values[0]] = values[2]
        elif '[Rollback] BEGIN ' in line:
            values = dict(re.findall(r'(\w+)=(\d+)', line))
            if 'targetIntro' in values:
                rollbacks[values['intro'] + '->' + values['targetIntro']] += 1
        elif line.startswith('[UpdateCadence] '):
            values = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', line)}
            f, n, tick = values['f'], values['n'], values['ticks']
            intro = current.get(f)
            if intro == 0 and f in states:
                intro = '0_active' if states[f][-2] else '0_locked'
            if previous:
                pf, pn, pt, pi = previous
                if f == pf + 1 and f // 65536 == pf // 65536 and n == pn + 1:
                    key = str(intro) if intro == pi else f'{pi}->{intro}'
                    cadence[key].append((tick - pt - 1000000) / 60)
            previous = f, n, tick, intro
    metrics = {key: dict(intervals=len(v), max_abs_error_us=max(map(abs, v)),
                        over_3us=sum(abs(x) > 3 for x in v)) for key, v in cadence.items()}
    return states, dict(rollbacks), metrics


def analyze(folder, require_boundaries=False):
    sync = compare(folder)
    a, ar, at = read(folder / 'game_1.log')
    b, br, bt = read(folder / 'game_2.log')
    confirmed = sync['confirmed_by_epoch']
    frames = [f for e, last in confirmed.items() for f in range(e * 65536 + 1, last + 1)]
    missing = [f for f in frames if f not in a or f not in b]
    differences = [f for f in frames if f in a and f in b and a[f] != b[f]]
    counts = collections.Counter(str(a[f][0]) for f in frames if f in a)
    substates = collections.Counter(f'{a[f][0]}/{a[f][1]}' for f in frames if f in a)
    locked = sum(a[f][0] == 0 and a[f][-2] == 0 for f in frames if f in a)
    covered = all(x in ar and x in br for x in ('2->2', '1->1', '2->1', '1->0'))
    return dict(passed=sync['passed'] and not missing and not differences and
                (covered or not require_boundaries) and all(counts[str(i)] for i in (2, 1, 0)),
                sync_passed=sync['passed'], confirmed_states=dict(counts), substates=dict(substates),
                intro0_input_locked=locked, missing=missing[:8], different=differences[:8],
                missing_count=len(missing), different_count=len(differences),
                boundaries_covered_both=covered, host_rollbacks=ar, client_rollbacks=br,
                host_cadence=at, client_cadence=bt,
                timing_scope='通常前進のみ。再計算を除外、未確定Fを含む。60MHz実時刻の差を60で除したµs。')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--require-boundaries', action='store_true')
    args = parser.parse_args()
    result = analyze(args.folder, args.require_boundaries)
    output = json.dumps(result, ensure_ascii=False, indent=2)
    (args.folder / 'intro_comparison.json').write_text(output, encoding='utf-8')
    print(output)
    raise SystemExit(0 if result['passed'] else 1)
