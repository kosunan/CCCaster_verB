"""SpinOSの実QPC区間とETWを照合。観測できないCPU時間を原因と断定しない。"""
import argparse
from bisect import bisect_left, bisect_right
import json
import re
from collections import defaultdict
from pathlib import Path

TICK_HZ = 60_000_000


def union_length(intervals):
    end = None
    total = 0
    for a, b in sorted(intervals):
        if b <= a:
            continue
        total += b - max(a, end) if end is not None and b > end else (b - a if end is None else 0)
        end = max(b, end) if end is not None else b
    return total


def clipped(a, b, c, d):
    return max(a, c), min(b, d)


class IntervalIndex:
    def __init__(self, records, begin, end):
        self.records = sorted(records, key=begin)
        self.starts = [begin(row) for row in self.records]
        self.ends = []
        for row in self.records:
            self.ends.append(max(end(row), self.ends[-1]) if self.ends else end(row))

    def overlap(self, begin, end):
        return self.records[bisect_right(self.ends, begin):bisect_left(self.starts, end)]


def read_spin(path, minimum_us=5):
    tables = defaultdict(dict)
    sound_windows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(("[DrawWork]", "[TextureTransfer]")):
            row = {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}
            if {"f", "pid", "tid", "begin", "end"} <= row.keys() and row["end"]-row["begin"] >= minimum_us*60:
                sound_windows.append(dict(source=str(path), frame=row["f"], pid=row["pid"], tid=row["tid"],
                    name="drawIndexed" if line.startswith("[DrawWork]") else "textureTransfer", begin=row["begin"], end=row["end"]))
        if line.startswith("[InputWait]"):
            row = {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}
            if row["end"]-row["begin"] >= minimum_us*60:
                sound_windows.append(dict(source=str(path),frame=row["f"],pid=row["pid"],tid=row["tid"],
                    name={1:"remoteInputWait",2:"metronomeInputWait",3:"boundaryInputWait"}.get(row["reason"],"inputWait"),
                    begin=row["begin"],end=row["end"]))
        if line.startswith("[ReplayWork]"):
            row = {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}
            for name, begin, end in (("replayRestore", row["begin"], row["restoreEnd"]),
                                     ("replayExecution", row["restoreEnd"], row["end"])):
                if end - begin >= minimum_us * 60:
                    sound_windows.append(dict(source=str(path), frame=row["f"], pid=row["pid"], tid=row["tid"],
                        name=name, begin=begin, end=end))
        if line.startswith("[SoundApi]"):
            row = {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}
            api = re.search(r"api=(\w+)", line)
            if api and row["end"] - row["begin"] >= minimum_us * 60:
                sound_windows.append(dict(source=str(path), frame=row["f"], pid=row["pid"], tid=row["tid"],
                    name="soundApi_" + api[1], begin=row["begin"], end=row["end"],
                    serial=row["seq"], replay=row["replay"], caller=row["caller"], result=row["result"]))
        if line.startswith("[SoundProbe]"):
            row = {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}
            if {"begin", "end", "pid", "tid"} <= row.keys() and row["end"] - row["begin"] >= minimum_us * 60:
                sound_windows.append(dict(source=str(path), frame=row["f"], pid=row["pid"], tid=row["tid"],
                    name="soundReplay" if row["replay"] else "soundNormal", begin=row["begin"], end=row["end"],
                    calls=row["calls"], suppressed=row["suppressed"]))
        match = re.match(r"\[(SpinOS|SpinProbe|SpinTail|SpinClock|SpinReturn)\]", line)
        if match:
            row = {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}
            tables[match[1]][row["f"]] = row
    rows = []
    for frame, os in tables["SpinOS"].items():
        probe = tables["SpinProbe"].get(frame, {})
        tail = tables["SpinTail"].get(frame, {})
        clock = tables["SpinClock"].get(frame, {})
        ret = tables["SpinReturn"].get(frame, {})
        windows = []
        for name in ("maxGap", "maxAge"):
            if name + "Begin" in os and name + "End" in os:
                windows.append((name, os[name + "Begin"], os[name + "End"]))
        if "spin" in tail and {"age", "innerGap", "prevAge"} <= clock.keys() and probe.get("reads", 0) > 1:
            current_qpc = tail["spin"] - clock["age"]
            previous_qpc = current_qpc - clock["innerGap"]
            previous_after = previous_qpc + clock["prevAge"]
            windows.extend((("previousReadAge", previous_qpc, previous_after),
                            ("lastReadAge", current_qpc, tail["spin"]),
                            ("lastObservationGap", previous_after, tail["spin"])))
        points = dict(tail, **ret)
        order = ("spin", "bounded", "wait", "begin", "input", "trace", "commit", "step",
                 "entry", "observeBegin", "observeEnd", "callbackEnd", "game")
        for a, b in zip(order, order[1:]):
            if a in points and b in points:
                windows.append((a + "_to_" + b, points[a], points[b]))
        if "spin" in tail and "game" in tail:
            windows.append(("postSpin", tail["spin"], tail["game"]))
        for name, begin, end in windows:
            if begin > 0 and end > begin and end - begin >= minimum_us * 60:
                rows.append(dict(source=str(path), frame=frame, pid=os["pid"], tid=os["tid"],
                                 qpc_hz=os.get("qpcHz"), name=name, begin=begin, end=end,
                                 exit_late_us=probe.get("exitLate", 0) / 60))
    return rows + sound_windows


class Trace:
    def __init__(self, records):
        self.metadata = {}
        for row in records:
            if row["type"] == "meta":
                self.metadata.update(row)
        self.hz = self.metadata.get("qpc_hz", 0)
        if self.metadata.get("clock") != "qpc" or not isinstance(self.hz, int) or self.hz <= 0:
            raise ValueError("ETWはraw QPCと正のqpc_hzが必要です")
        self.quality = []
        if not self.metadata.get("final", False):
            self.quality.append("final_metadata_missing")
        if not self.metadata.get("complete", False):
            self.quality.append("trace_incomplete")
        if self.metadata.get("lost_events") != 0:
            self.quality.append("lost_or_unreported_events")
        if self.metadata.get("lost_buffers") != 0:
            self.quality.append("lost_or_unreported_buffers")
        self.switches = defaultdict(list)
        self.interrupts = defaultdict(list)
        self.images = defaultdict(list)
        self.lifetimes = defaultdict(list)
        for row in records:
            kind = row["type"]
            if kind == "cswitch":
                tick = self.tick(row["qpc"])
                for role in ("old", "new"):
                    tid = row[role + "_tid"]
                    self.switches[tid].append((tick, role, row["cpu"], row.get(role + "_pid")))
            elif kind in ("dpc", "isr"):
                a, b = self.tick(row["begin_qpc"]), self.tick(row["end_qpc"])
                if b < a:
                    self.quality.append("invalid_interrupt_interval")
                else:
                    self.interrupts[row["cpu"]].append(dict(row, begin=a, end=b))
            elif kind == "image":
                self.images[row["base"]].append(row)
            elif kind == "thread":
                self.lifetimes[row["tid"]].append(row)
        self.schedule_cache = {}
        self.interrupt_index = {cpu: IntervalIndex(rows, lambda e: e["begin"], lambda e: e["end"])
                                for cpu, rows in self.interrupts.items()}

    def tick(self, qpc):
        # Platform::RealMonotonicTicksと同じ整数変換。UTC/相対msから再変換しない。
        return qpc // self.hz * TICK_HZ + qpc % self.hz * TICK_HZ // self.hz

    def schedule(self, tid, pid):
        key = (tid, pid)
        if key in self.schedule_cache:
            return self.schedule_cache[key]
        events = sorted(self.switches[tid], key=lambda r: (r[0], r[1]))
        pieces = []
        for left, right in zip(events, events[1:]):
            a, role_a, cpu_a, pid_a = left
            b, role_b, cpu_b, pid_b = right
            if a >= b or (pid_a is not None and pid_a != pid) or (pid_b is not None and pid_b != pid):
                continue
            if role_a == "new" and role_b == "old" and cpu_a == cpu_b:
                pieces.append((a, b, "on_cpu", cpu_a))
            elif role_a == "old" and role_b == "new":
                pieces.append((a, b, "off_cpu", None))
        self.schedule_cache[key] = IntervalIndex(pieces, lambda e: e[0], lambda e: e[1])
        return self.schedule_cache[key]

    def module(self, event):
        if event.get("module"):
            return event["module"]
        address = event.get("routine")
        if isinstance(address, str):
            address = int(address, 0)
        if address is None:
            return None
        candidates = []
        for base, records in self.images.items():
            if not base <= address:
                continue
            # Kernel imageはPID=0/4。その他のPIDアドレスをドライバへ誤対応しない。
            applicable = [r for r in records if r.get("pid") in (0, 4)]
            earlier = [r for r in applicable if r["qpc"] <= event["begin_qpc"]]
            if earlier:
                latest = max(earlier, key=lambda r: r["qpc"])
                if latest["event"] not in ("Unload", "Stop") and address < base + latest["size"]:
                    candidates.append(latest["path"])
            else:
                # DCStopは終了時の在籍証拠だけ。途中ロード時刻不明のため過去へ外挿しない。
                continue
        return candidates[0] if len(set(candidates)) == 1 else None

    def lifetime_covers(self, tid, pid, begin, end):
        # 過去の別PIDによるTID使用を、現在の明確なStart/Stop区間へ持ち込まない。
        started = None
        for row in sorted(self.lifetimes[tid], key=lambda r: r["qpc"]):
            tick = self.tick(row["qpc"])
            kind = row["event"]
            if kind in ("Start", "DCStart"):
                if started is None or started[1] != row["pid"] or kind == "Start":
                    started = (tick, row["pid"])
            elif kind in ("Stop", "DCStop"):
                if started is not None and started[1] == row["pid"]:
                    if row["pid"] == pid and started[0] <= begin and end <= tick:
                        return True
                started = None
        return False

    def attribute(self, window):
        begin, end = window["begin"], window["end"]
        on, off, evidence = [], [], []
        pieces = self.schedule(window["tid"], window["pid"]).overlap(begin, end)
        for a, b, state, cpu in pieces:
            left, right = clipped(begin, end, a, b)
            if right <= left:
                continue
            if state == "off_cpu":
                off.append((left, right))
                continue
            on.append((left, right))
            candidates = self.interrupt_index[cpu].overlap(left, right) if cpu in self.interrupt_index else []
            for event in candidates:
                x, y = clipped(left, right, event["begin"], event["end"])
                if y > x:
                    evidence.append(dict(kind=event["type"], cpu=cpu, begin=x, end=y,
                                         routine=event.get("routine"), module=self.module(event)))
        on_ticks, off_ticks = union_length(on), union_length(off)
        interrupt_ticks = union_length((e["begin"], e["end"]) for e in evidence)
        unknown_ticks = end - begin - on_ticks - off_ticks
        reasons = list(dict.fromkeys(self.quality))
        if window.get("qpc_hz") not in (None, self.hz):
            reasons.append("qpc_frequency_mismatch")
        if unknown_ticks:
            reasons.append("scheduler_coverage_missing")
        # 対象区間のPID/TIDの生存両端が判明すれば、それ以前の再利用は曖昧さを残さない。
        thread_pids = {r["pid"] for r in self.lifetimes[window["tid"]]}
        if thread_pids - {window["pid"]} and not self.lifetime_covers(window["tid"], window["pid"], begin, end):
            reasons.append("thread_id_reused")
        driver_intervals = defaultdict(list)
        for row in evidence:
            driver_intervals[row["module"] or "unknown"].append((row["begin"], row["end"]))
        return dict(window, duration_us=(end - begin) / 60,
                    status="unknown" if reasons else "observed", reasons=reasons,
                    off_cpu_us=off_ticks / 60, interrupt_us=interrupt_ticks / 60,
                    on_cpu_other_us=(on_ticks - interrupt_ticks) / 60,
                    scheduler_unknown_us=unknown_ticks / 60,
                    drivers_us={key: union_length(value) / 60 for key, value in driver_intervals.items()},
                    interrupts=evidence)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("spin_dir", type=Path)
    parser.add_argument("etw_jsonl", type=Path)
    parser.add_argument("--minimum-us", type=float, default=5)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    records = [json.loads(line) for line in args.etw_jsonl.read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    trace = Trace(records)
    windows = [w for path in sorted(args.spin_dir.glob("game_*.log")) for w in read_spin(path, args.minimum_us)]
    results = [trace.attribute(w) for w in windows]
    report = dict(metadata=trace.metadata, interval_count=len(results),
                  explanation="observedは区間の観測であり原因の断定ではない。on_cpu_otherはCPU計算だけでなく未観測の停止も含む。DPC/ISR取得設定が別途確認できなければinterrupt_us=0は割込み不在の証明ではない。drivers_usはnested ISR/DPCで重複し得るがinterrupt_usは和集合。",
                  intervals=results)
    output = args.output or args.spin_dir / "spin_etw_analysis.json"
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(dict(output=str(output), intervals=len(results), unknown=sum(r["status"] == "unknown" for r in results)), ensure_ascii=False))


if __name__ == "__main__":
    main()
