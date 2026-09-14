"""独立キャラセレの確定値・ロード後の値・実更新間隔を確認する。"""
import json
import argparse
import re
import statistics
import sys
from pathlib import Path


def validate_load_sequence(text):
    current = None
    loaded = False
    errors = []
    for kind, *values in re.findall(
            r"\[Select\] (COMMIT|LOADED) p1=(\d+)/(\d+)/(\d+) p2=(\d+)/(\d+)/(\d+) stage=(\d+)", text):
        if kind == 'COMMIT':
            if current is not None and not loaded:
                errors.append('前の確定選択をロードしていない')
            current, loaded = values, False
        else:
            if current != values:
                errors.append(dict(expected=current, loaded=values))
            loaded = True
    if current is None or not loaded:
        errors.append('最後の確定選択のロードがない')
    return errors


def analyze(directory, random_stage=False):
    sides = []
    for role in (1, 2):
        text = (directory / f"game_{role}.log").read_text(encoding="utf-8", errors="replace")
        local = re.findall(r"\[Select\] LOCAL epoch=(\d+) char=(\d+) moon=(\d+) color=(\d+)", text)
        commit = re.findall(r"\[Select\] COMMIT p1=(\d+)/(\d+)/(\d+) p2=(\d+)/(\d+)/(\d+) stage=(\d+)", text)
        loaded = re.findall(r"\[Select\] LOADED p1=(\d+)/(\d+)/(\d+) p2=(\d+)/(\d+)/(\d+) stage=(\d+)", text)
        resolved = re.findall(r"\[Select\] RANDOM resolved=(\d+)", text)
        ticks = [tuple(map(int, v)) for v in re.findall(
            r"\[SelectPace\] f=(\d+) WT=(\d+) interval=(\d+) local=(\d+) peer=(\d+)", text)]
        intervals = sorted(t[2] for t in ticks if t[2] > 0)
        steady = sorted(t[2] for t in ticks if t[0] % 65536 > 120 and t[2] > 0)
        display = sorted(int(dt) for f, dt in re.findall(r"\[DisplayPace\] f=(\d+) interval=(\d+)", text)
                         if int(f) // 65536 == 1 and int(f) % 65536 > 120 and int(dt) > 0)
        discontinuities = sum(b[0] != a[0] + 1 or b[1] != a[1] + 1
                              for a, b in zip(ticks, ticks[1:]) if b[0] // 65536 == a[0] // 65536)
        sides.append(dict(local=local, commit=commit, loaded=loaded, load_errors=validate_load_sequence(text),
                          random_resolved=resolved, samples=len(intervals),
                          discontinuities=discontinuities,
                          median_us=statistics.median(intervals) if intervals else None,
                          p99_us=intervals[min(len(intervals)-1, int(len(intervals)*.99))] if intervals else None,
                          max_us=max(intervals, default=None),
                          after_first_120=dict(samples=len(steady), max_us=max(steady, default=None),
                              p99_us=steady[min(len(steady)-1, int(len(steady)*.99))] if steady else None,
                              display_samples=len(display), display_max_us=max(display, default=None),
                              display_p99_us=display[min(len(display)-1, int(len(display)*.99))] if display else None),
                          over_20ms=sum(v > 20000 for v in intervals)))
    a, b = sides
    passed = bool(a["commit"] and a["local"] and b["local"] and a["samples"] and b["samples"])
    # ONCEやラウンド遷移は再選択せず、同じ確定値を繰り返しロードする。
    # 単なる重複除去では後の異常値を隠すため、時系列で直前のCOMMITと照合する。
    passed &= a["commit"] == b["commit"] and a["loaded"] == b["loaded"]
    passed &= not a['load_errors'] and not b['load_errors']
    passed &= len(a["commit"]) == len(a["local"]) == len(b["local"])
    if passed:
        passed &= all(c[:3] == x[1:] and c[3:6] == y[1:]
                      for c, x, y in zip(a["commit"], a["local"], b["local"]))
    passed &= not a["discontinuities"] and not b["discontinuities"]
    if random_stage:
        passed &= bool(a["random_resolved"]) and not b["random_resolved"]
        passed &= a["random_resolved"] == [c[-1] for c in a["commit"]]
        passed &= all(0 < int(stage) < 100 for stage in a["random_resolved"])
    return dict(sides=sides, passed=bool(passed))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--random-stage", action="store_true")
    args = parser.parse_args()
    result = analyze(args.directory, args.random_stage)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    sys.exit(0 if result["passed"] else 1)
