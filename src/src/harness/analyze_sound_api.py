"""効果音更新ごとのAPI内訳。ネスト時間は和集合で集計し、通常と再計算を分離。"""
import json
import re
import sys
from collections import defaultdict
from pathlib import Path
from analyze_spin_etw import union_length

def fields(line):
    return {k: int(v) if re.fullmatch(r"-?\d+", v) else v
            for k, v in re.findall(r"(\w+)=([^\s]+)", line)}

def stats(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return dict(count=len(values), median=values[len(values)//2],
                p99=values[min(len(values)-1, int(len(values)*.99))], maximum=values[-1], total=sum(values))

def analyze(path):
    parents, children = {}, defaultdict(list)
    seen_play = set()
    first_combat = None
    drops = 0
    setup = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("[SoundApi]"):
            row = fields(line)
            if row["api"] == "Play":
                row["first_observed"] = row["buffer"] not in seen_play
                seen_play.add(row["buffer"])
            children[row["seq"]].append(row)
        elif line.startswith("[SoundProbe]"):
            row = fields(line)
            if "seq" in row:
                if row["seq"] in parents:
                    raise ValueError("duplicate invocation serial")
                parents[row["seq"]] = row
        elif line.startswith("[CombatStress]"):
            first_combat = first_combat or fields(line)["f"]
        elif line.startswith("[SoundApiDrop]") or line.startswith("[SoundProbeDrop]"):
            drops += fields(line)["count"]
        elif line.startswith("[SoundApiHook]") or line.startswith("[SoundApiReady]") or line.startswith("[SoundApiWaitHook]"):
            setup.append(line)
    selected = {s: p for s, p in parents.items() if not p.get("warmup") and
                first_combat is not None and p["f"] >= first_combat}
    errors, reports = [], []
    for seq, parent in selected.items():
        rows = sorted(children[seq], key=lambda r: r["begin"])
        for row in rows:
            if not (parent["begin"] <= row["begin"] <= row["end"] <= parent["end"]):
                errors.append(dict(seq=seq, reason="API outside parent", api=row["api"]))
        union = union_length((r["begin"], r["end"]) for r in rows)
        reports.append(dict(parent, api_union_us=union/60, residual_us=(parent["ticks"]-union)/60,
                            apis=[dict(r, us=(r["end"]-r["begin"])/60) for r in rows]))
    summary = {}
    for replay, name in [(0, "normal"), (1, "replay")]:
        ps = [p for p in reports if p["replay"] == replay]
        apis = defaultdict(list)
        for p in ps:
            for r in p["apis"]:
                apis[r["api"]].append(r)
        summary[name] = dict(updates=len(ps), parent_us=stats([p["ticks"]/60 for p in ps]),
            first_play_us=stats([r["us"] for r in apis.get("Play", []) if r["first_observed"]]),
            repeat_play_us=stats([r["us"] for r in apis.get("Play", []) if not r["first_observed"]]),
            residual_us=stats([p["residual_us"] for p in ps]),
            api={name: dict(us=stats([r["us"] for r in rows]),
                           failures=sum(bool(r["result"] & 0x80000000) for r in rows),
                           nonzero_results=sorted(set(r["result"] for r in rows if r["result"])),
                           callers=sorted(set(r["caller"] for r in rows)),
                           arguments=sorted(set((r["a"], r["b"], r["c"]) for r in rows)) if name != "GetStatus" else [],
                           statuses=sorted(set(r["status"] for r in rows)) if name == "GetStatus" else [])
                 for name, rows in apis.items()},
            top_updates=sorted(ps, key=lambda p: p["ticks"], reverse=True)[:8])
    wait_requested = any(s.startswith("[SoundApiWaitHook]") for s in setup)
    wait_captured = any(r["api"] == "WaitForMultipleObjects" for rs in children.values() for r in rs)
    return dict(source=str(path), first_combat=first_combat, drops=drops, errors=errors, setup=setup,
                wait_capture_missing=wait_requested and not wait_captured, summary=summary)

if __name__ == "__main__":
    directory = Path(sys.argv[1])
    report = {str(s): analyze(directory / f"game_{s}.log") for s in (1, 2)}
    output = directory / "sound_api_analysis.json"
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({s: dict(drops=r["drops"], errors=r["errors"],
        normal={api: data["us"] for api, data in r["summary"]["normal"]["api"].items()}) for s, r in report.items()}, indent=2))
