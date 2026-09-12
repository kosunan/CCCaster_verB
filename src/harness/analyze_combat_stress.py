"""CombatStress/SoundProbe の通常更新と再計算を分ける。ticks は 60 MHz。"""
import json
import re
import sys
from pathlib import Path

def distribution(values):
    values = sorted(values)
    if not values:
        return {"n": 0}
    return {"n": len(values), "median_us": values[len(values)//2],
            "p99_us": values[min(len(values)-1, int(len(values)*.99))], "max_us": values[-1]}

def analyze(path):
    sound = {0: [], 1: []}
    combat = {0: [], 1: []}
    latest = None
    drops = 0
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        fields = dict(re.findall(r"(\w+)=(-?\d+)", line))
        fields = {k: int(v) for k, v in fields.items()}
        if line.startswith("[SoundProbe]"):
            latest = fields
            sound[fields["replay"]].append(fields)
        elif line.startswith("[CombatStress]") and latest and latest["f"] == fields["f"]:
            combat[latest["replay"]].append(fields)
        elif line.startswith("[SoundProbeDrop]"):
            drops += fields["count"]
    result = {"drops": drops}
    for replay, name in [(0, "normal"), (1, "replay")]:
        frames = {r["f"] for r in combat[replay]}
        rows = [r for r in sound[replay] if r["f"] in frames]
        result[name] = {"frames": len(combat[replay]),
            "damaged_frames": sum(r["hp1"] < 11400 or r["hp2"] < 11400 for r in combat[replay]),
            "sound_requests": sum(r["calls"] for r in rows),
            "suppressed": sum(r["suppressed"] for r in rows),
            "sound_update": distribution([r["ticks"]/60 for r in rows]),
            "with_requests": distribution([r["ticks"]/60 for r in rows if r["calls"]]),
            "max_hit_budget": max((max(r["hits1"], r["hits2"]) for r in combat[replay]), default=0)}
    return result

if __name__ == "__main__":
    directory = Path(sys.argv[1])
    result = {f"side_{s}": analyze(directory / f"game_{s}.log") for s in (1, 2)}
    text = json.dumps(result, ensure_ascii=False, indent=2)
    (directory / "combat_stress_analysis.json").write_text(text, encoding="utf-8")
    print(text)
