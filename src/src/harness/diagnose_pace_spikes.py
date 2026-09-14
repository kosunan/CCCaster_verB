"""既存PACEログの通常戦闘間隔を分解する。ゲームや原本ログは変更しない。"""
import argparse
import json
import math
import re
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return {"count": len(values), "min": values[0],
            "p99": values[math.ceil(len(values) * .99) - 1], "max": values[-1]}


def analyze(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    tags = {}
    for line in text.splitlines():
        tag = re.match(r"\[(\w+)\]", line)
        if tag:
            row = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)}
            tags.setdefault(tag[1], []).append(row)
    recovery = re.search(r"\[Rollback\] BEGIN frame=\d+ target=(\d+)", text)
    if not recovery:
        return {"error": "初回ロールバックなし。対象区間を定義できない"}
    first = int(recovery[1])
    epoch = first // 65536 * 65536
    delay = re.search(rf"EPOCH base={epoch} .*lookahead=(\d+)", text)
    delay = int(delay[1]) if delay else None
    spins = {r["f"]: r for r in tags.get("SpinStage", [])}
    captures = {r["f"]: r for r in tags.get("CaptureStage", [])}
    snapshots = {r["f"]: r for r in tags.get("SnapshotStage", [])}
    shifts = {}
    for row in tags.get("PhasePace", []):
        shifts[row["f"]] = shifts.get(row["f"], 0) + row["shift"]
    samples = []
    for a, b in zip(tags.get("Pace", []), tags.get("Pace", [])[1:]):
        if not (b["f"] == a["f"] + 1 and b["play"] and
                b["f"] >= first and b["f"] // 65536 == first // 65536):
            continue
        interval = b["qpc"] - a["qpc"]
        due = b["due"] - a["due"]
        late_delta = (b["audio"] - b["due"]) - (a["audio"] - a["due"])
        clock_delta = (b["audio"] - b["qpc"]) - (a["audio"] - a["qpc"])
        # 同じログ値の代数恒等式。因果や独立した計測精度の証明ではない。
        assert interval == due + late_delta - clock_delta
        spin = spins.get(b["f"], {})
        capture = captures.get(b["f"] + delay, {}) if delay is not None else {}
        row = dict(frame=b["f"], interval=interval,
                   error=round(interval - 1000000 / 60, 3), due_interval=due,
                   phase_shift=shifts.get(b["f"] + delay, 0) if delay is not None else None,
                   late_previous=a["audio"] - a["due"], late=b["audio"] - b["due"],
                   late_delta=late_delta, clock_delta=clock_delta,
                   ready_late=b["ready"] - b["due"], spin_late=spin.get("late"),
                   post_spin_us=b["qpc"] - spin["end"] if spin.get("end") else None,
                   publication_to_update_us=b["qpc"] - capture["published"] if capture else None,
                   save_us=snapshots.get(b["f"], {}).get("save"),
                   previous_play=bool(a["play"]))
        samples.append(row)
    return {"source": str(path.resolve()), "first_recovery": first, "delay": delay,
            "selection": "初回ロールバック以後・同世代・連続番号・後端play=1。外れ値除外なし",
            "previous_not_playing": sum(not r["previous_play"] for r in samples),
            "interval": distribution([r["interval"] for r in samples]),
            "abs_error": distribution([abs(r["error"]) for r in samples]),
            "components": {key: distribution([r[key] for r in samples if r[key] is not None])
                           for key in ("due_interval", "phase_shift", "late", "late_delta",
                                       "clock_delta", "ready_late", "spin_late", "post_spin_us", "save_us")},
            "late_preparation_count": sum(r["ready_late"] > 0 for r in samples),
            "top_absolute_error": sorted(samples, key=lambda r: abs(r["error"]), reverse=True)[:12],
            "top_post_spin": sorted([r for r in samples if r["post_spin_us"] is not None],
                                    key=lambda r: r["post_spin_us"], reverse=True)[:8]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    print(json.dumps({str(side): analyze(args.root / f"game_{side}.log") for side in (1, 2)},
                     ensure_ascii=False, indent=2))
