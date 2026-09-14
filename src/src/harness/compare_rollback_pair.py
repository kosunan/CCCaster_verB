"""確定済み戦闘を比較する。観測対象は入力・代表状態であり全メモリではない。"""
import argparse
import json
import re
import statistics
from pathlib import Path

FIELDS = {
    '[REC]': 'p1_direction p1_buttons p2_direction p2_buttons'.split(),
    '[FRAME]': 'phase intro relative_world_timer real_timer'.split(),
    '[STATE]': 'rng_hash p1_x p1_y p2_x p2_y'.split(),
    '[MEM]': 'mode intro state absolute_WT real_timer round_timer menu_counter rng0 rng1 p1_sequence p2_sequence p1_hp p2_hp round_count p1_wins p2_wins'.split(),
}
EXCLUDED = {'[MEM]': {3, 6}}


def read(path):
    data = {tag: {} for tag in FIELDS}
    confirmed, depths, times, failures = {}, [], [], []
    repeats = predictions = sounds = 0
    for number, line in enumerate(path.read_text(encoding='utf-8', errors='strict').splitlines(), 1):
        x = line.split()
        if not x:
            continue
        try:
            if x[0] in data:
                if len(x) != len(FIELDS[x[0]]) + 2:
                    raise ValueError('列数不正')
                f = int(x[1])
                tuple(map(int, x[2:]))
                if f < 1 or f > 0xffffffff or f % 65536 == 0:
                    raise ValueError('フレーム番号不正')
                repeats += f in data[x[0]]
                data[x[0]][f] = x[2:]
            if x[0] == '[CONFIRMED]':
                f = int(x[1])
                if len(x) != 2 or f < 0 or f > 0xffffffff:
                    raise ValueError('確定番号不正')
                # 世代先頭は「まだ確定なし」のセンチネル。実フレームとして数えない。
                if f % 65536:
                    confirmed[f // 65536] = max(f, confirmed.get(f // 65536, 0))
            if '[Rollback] BEGIN' in line:
                depths.append(int(re.search(r'depth=(\d+)', line)[1]))
            if '[Rollback] END' in line:
                times.append(int(re.search(r'elapsedUs=(\d+)', line)[1]))
            predictions += '[Rollback] PREDICT' in line
            if '[Rollback] SFX suppressed=' in line:
                sounds += int(line.split('=')[-1])
            if 'FAILED' in line or re.search(r'\[\w*Drop(?:ped)?\]', line):
                failures.append(line)
        except (ValueError, TypeError, IndexError) as exc:
            failures.append(f'{path.name}:{number}: {exc}: {line}')
    return data, confirmed, dict(rollbacks=len(depths), predictions=predictions,
        max_depth=max(depths, default=0), mean_depth=statistics.mean(depths) if depths else 0,
        rollup_median_us=statistics.median(times) if times else 0,
        rollup_p99_us=sorted(times)[min(len(times)-1, int(len(times)*.99))] if times else 0,
        rollup_max_us=max(times, default=0), repeated_trace_rows=repeats,
        suppressed_sounds=sounds, failures=failures)


def compare(folder, min_frames=1000, require_rollback=True):
    a, ac, astats = read(folder / 'game_1.log')
    b, bc, bstats = read(folder / 'game_2.log')
    unmatched = sorted(ac.keys() ^ bc.keys())
    confirmed = {e: min(ac[e], bc[e]) for e in ac.keys() & bc.keys()}
    result = dict(host=astats, client=bstats, comparisons={},
                  excluded={'absolute_WT': 0, 'menu_counter': 0},
                  unmatched_confirmed_epochs=unmatched, confirmed_by_epoch=confirmed,
                  min_frames=min_frames, require_rollback=require_rollback,
                  scope='確定済み戦闘の入力・代表状態。保存領域の完全性は未検証')
    passed = bool(confirmed) and not (unmatched or astats['failures'] or bstats['failures'])
    passed &= not require_rollback or astats['rollbacks'] + bstats['rollbacks'] > 0
    for tag in FIELDS:
        common = sorted(f for f in a[tag].keys() & b[tag].keys() if f <= confirmed.get(f // 65536, 0))
        missing = [f for e, last in confirmed.items() for f in range(e*65536+1, last+1)
                   if f not in a[tag] or f not in b[tag]]
        differences, details = [], []
        for f in common:
            x, y = a[tag][f], b[tag][f]
            for i in EXCLUDED.get(tag, set()):
                result['excluded'][FIELDS[tag][i]] += x[i] != y[i]
            changed = [i for i in range(len(x)) if i not in EXCLUDED.get(tag, set()) and x[i] != y[i]]
            if changed:
                differences.append(f)
                if len(details) < 8:
                    details.append(dict(frame=f, values=[dict(field=FIELDS[tag][i], host=x[i], client=y[i]) for i in changed]))
        result['comparisons'][tag] = dict(common_confirmed=len(common), different=len(differences),
            first_differences=differences[:8], details=details, missing=missing[:8], missing_count=len(missing))
        passed &= len(common) >= min_frames and not differences and not missing
    result['passed'] = bool(passed)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--min-frames', type=int, default=1000)
    parser.add_argument('--allow-no-rollback', action='store_true')
    args = parser.parse_args()
    if args.min_frames < 1:
        parser.error('--min-frames は1以上')
    try:
        result = compare(args.folder, args.min_frames, not args.allow_no_rollback)
    except (OSError, UnicodeError) as exc:
        result = dict(passed=False, error=str(exc))
    output = json.dumps(result, ensure_ascii=False, indent=2)
    (args.folder / 'rollback_comparison.json').write_text(output, encoding='utf-8')
    print(output)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
