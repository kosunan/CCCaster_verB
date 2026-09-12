"""CCCASTER_PACE_TRACE: 締切・時計・待機の寄与を分解（単位µs）。"""
import math
import json
import re
import sys
from pathlib import Path


def analyze(path):
    rows = [dict((k, int(v)) for k, v in re.findall(r"(\w+)=(-?\d+)", line))
            for line in path.read_text(errors="replace").splitlines() if line.startswith("[Pace]")]
    log_text = path.read_text(errors="replace")
    recovery_starts = [int(f) for f in re.findall(r"\[Rollback\] BEGIN frame=\d+ target=(\d+)", log_text)]
    first_recovery = min(recovery_starts) if recovery_starts else None
    playable = {r["f"] for r in rows if r.get("play", first_recovery is not None and r["f"] >= first_recovery)}
    samples = []
    for a, b in zip(rows, rows[1:]):
        if b["f"] != a["f"] + 1 or b["f"] < 131072:
            continue
        delta = b["qpc"] - a["qpc"]
        samples.append(dict(frame=b["f"], interval=delta, due=b["due"]-a["due"],
                            late=b["audio"]-b["due"], ready=b["ready"]-b["due"],
                            late_delta=(b["audio"]-b["due"])-(a["audio"]-a["due"]),
                            clock_delta=(b["audio"]-b["qpc"])-(a["audio"]-a["qpc"]),
                            read=b["read"], work=b["work"], play=b["f"] in playable))
    def summary(data):
        def stats(key):
            values = sorted(s[key] for s in data)
            return dict(min=values[0], median=values[len(values)//2], p99=values[int((len(values)-1)*.99)], max=values[-1]) if values else {}
        return dict(count=len(data), over17000=sum(s["interval"]>=17000 for s in data),
                    interval=stats("interval"), late=stats("late"), work=stats("work"), read=stats("read"),
                    examples=sorted(data,key=lambda s:abs(s["interval"]-16667),reverse=True)[:8])
    displays = [dict((k, int(v)) for k, v in re.findall(r"(\w+)=(-?\d+)", line))
                for line in path.read_text(errors="replace").splitlines() if line.startswith("[DisplayPace]")]
    phases = [dict((k, int(v)) for k, v in re.findall(r"(\w+)=(-?\d+)", line))
              for line in path.read_text(errors="replace").splitlines() if line.startswith("[PhasePace]")]
    def distribution(values):
        values = sorted(values)
        if not values:
            return {}
        at = lambda p: values[max(0, math.ceil(len(values)*p)-1)]
        return dict(count=len(values), minimum=values[0], median=at(.5), p99=at(.99), p999=at(.999), maximum=values[-1])
    def intervals(data):
        values = [d["interval"] for d in data]
        return dict(interval=distribution(values), abs_error=distribution([round(abs(v-1000000/60),2) for v in values]),
                    over17000=sum(v>=17000 for v in values), within50=sum(abs(v-1000000/60)<=50 for v in values),
                    within100=sum(abs(v-1000000/60)<=100 for v in values))
    active = [s for s in samples if s["play"]]
    active_displays = [d for d in displays if d["f"] in playable]
    rollback_active = [d for d in active if first_recovery is not None and d["frame"] >= first_recovery]
    rollback_displays = [d for d in active_displays if first_recovery is not None and d["f"] >= first_recovery]
    prepared = [d.get("prepare", d.get("budget",500)+d["late"]-d["wait"]) for d in rollback_displays if "late" in d and "wait" in d]
    # 1F超の停止は表示余裕で覆わず、別途原因調査する。
    normal_prepared = [max(0,p) for p in prepared if p<16667]
    needed = distribution(normal_prepared)
    calibration = dict(start_frame=first_recovery, prepare=needed, excluded_over_frame=sum(p>=16667 for p in prepared),
                       proposed_budget_us=math.ceil(needed["p999"]/50)*50 if needed else None)
    tail = samples[-480:]
    first = tail[0]["frame"] if tail else 0
    display_tail = [d for d in displays if d["f"] >= first]
    return dict(all=summary(samples), last480=summary(tail),
                playable_frames=intervals(active), playable_display=intervals(active_displays),
                rollback_frames=intervals(rollback_active), rollback_display=intervals(rollback_displays), calibration=calibration,
                rollback_count=len(recovery_starts),
                display_last480=dict(count=len(display_tail), over17000=sum(d["interval"]>=17000 for d in display_tail),
                                     examples=sorted(display_tail,key=lambda d:d["interval"],reverse=True)[:8]),
                phase_last480=dict(count=sum(p["f"]>=first for p in phases),
                                   examples=[p for p in phases if p["f"]>=first][:8]))

if __name__ == "__main__":
    root=Path(sys.argv[1])
    print(json.dumps({str(n):analyze(root/f"game_{n}.log") for n in (1,2)}, indent=2))
