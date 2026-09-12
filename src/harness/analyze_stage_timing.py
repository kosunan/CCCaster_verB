"""PACE_TRACEの区間計測。実戦開始後のデータを残し、外れ値も除去しない。"""
from pathlib import Path
import json, math, re, sys

root = Path(sys.argv[1])
def stats(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return {"count": len(values), "median": values[len(values)//2],
            "p99": values[math.ceil(len(values)*.99)-1], "max": values[-1]}

report = {}
for side in (1, 2):
    text = (root / f"game_{side}.log").read_text(encoding="utf-8", errors="replace")
    first = re.search(r"\[Rollback\] BEGIN frame=\d+ target=(\d+)", text)
    if not first:
        report[str(side)] = {"error": "ロールバック実戦区間なし"}
        continue
    first = int(first[1])
    rows = {}
    for line in text.splitlines():
        match = re.match(r"\[(\w+)\] ", line)
        if match:
            row = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", line)}
            if row:
                rows.setdefault(match[1], []).append(row)
    summary = {}
    for tag in ("CaptureStage", "SnapshotStage", "RestoreStage", "SpinStage", "InputSpin", "TimelineLock"):
        selected = [r for r in rows.get(tag, []) if r.get("f", 0) >= first and r.get("f", 0) // 65536 == first // 65536]
        fields = set().union(*(r.keys() for r in selected)) if selected else set()
        summary[tag] = {k: stats([r[k] for r in selected if k in r]) for k in fields - {"f", "begin", "end", "published"}}
        if selected and "begin" in fields:
            summary[tag]["spin_elapsed"] = stats([r["end"]-r["begin"] for r in selected])
    # 入力公開時刻と通常更新直前の実QPCを対応させる。Dは当該世代の設定から読む。
    epoch = first // 65536 * 65536
    delay = re.search(rf"EPOCH base={epoch} .*lookahead=(\d+)", text)
    delay = int(delay[1]) if delay else 0
    captures = {r["f"]: r for r in rows.get("CaptureStage", [])}
    pace = [r for r in rows.get("Pace", []) if r.get("play") and r["f"] >= first and r["f"] // 65536 == first // 65536]
    summary["publication_to_update_us"] = stats([r["qpc"] - captures[r["f"] + delay]["published"] for r in pace if r["f"] + delay in captures])
    summary["late_preparation_examples"] = [{"frame": r["f"], "late": r["ready"]-r["due"]} for r in pace if r["ready"]-r["due"] > 100]
    summary["clock"] = [l for l in text.splitlines() if l.startswith("[Clock]")]
    summary["receive_over100_all_phases"] = rows.get("ReceiveStage", [])
    summary["audio_over100_all_phases"] = rows.get("AudioWork", [])
    report[str(side)] = summary
output = json.dumps(report, ensure_ascii=False, indent=2)
(root / "stage_analysis.json").write_text(output, encoding="utf-8")
print(output)
