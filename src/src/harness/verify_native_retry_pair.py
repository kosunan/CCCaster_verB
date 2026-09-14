"""本来の再戦メニューの確定通知、ACK、遷移先とローカル入力進行を照合する。"""
import argparse
import json
import re
from pathlib import Path


def analyze(folder, scenario):
    sides = []
    for side in (1, 2):
        text = (folder / f"game_{side}.log").read_text(encoding="utf-8", errors="replace")
        resolves = list(re.finditer(
            r"\[RetryMenu\] RESOLVED epoch=(\d+) frame=(\d+) local=(-?\d+) peer=(-?\d+) target=(\d+) ack=(\d+)/(\d+)", text))
        local = [tuple(map(int, row)) for row in re.findall(
            r"\[RetryMenu\] LOCAL epoch=(\d+) frame=(\d+) choice=(\d+)", text)]
        ticks = [tuple(map(int, row)) for row in re.findall(
            r"\[RetryPace\] f=(\d+) WT=(\d+) interval=(\d+) input=(\d+) choice=(\d+) peer=(\d+)", text)]
        failures = [line for line in text.splitlines() if "FAILED" in line]
        evidence = []
        for match in resolves:
            epoch, frame, own, peer, target, ack, peer_ack = map(int, match.groups())
            transition = re.search(r"\[SceneRunner\] Phase change: 5 -> (\d+)", text[match.end():])
            p1, p2 = (own, peer) if side == 1 else (peer, own)
            expected = 0 if scenario in (0, 3) else 1
            valid = target == expected and transition is not None and int(transition[1]) == (3 if expected == 0 else 2)
            valid &= ack == peer + 1 and (own == -1 or peer_ack == own + 1)
            valid &= (p1, p2) == ((0, 0) if expected == 0 else ((1, -1) if scenario == 1 else (-1, 1)))
            evidence.append(dict(epoch=epoch, frame=frame, p1=p1, p2=p2, target=target,
                                 next_phase=int(transition[1]) if transition else None, passed=bool(valid)))
        discontinuities = sum(b[0] != a[0] + 1 or b[1] != a[1] + 1 for a, b in zip(ticks, ticks[1:])
                              if a[0] // 65536 == b[0] // 65536)
        after_confirm_input = sum(bool(row[3]) for row in ticks if row[4])
        own_frames = {epoch: frame for epoch, frame, _ in local}
        for e in evidence:
            own_choice = e["p1"] if side == 1 else e["p2"]
            e["passed"] &= (own_choice < 0 or
                            any(epoch == e["epoch"] and choice == own_choice and frame <= e["frame"]
                                for epoch, frame, choice in local))
        waits = [row[0] - own_frames[row[0] // 65536 * 65536] for row in ticks
                 if row[4] and row[0] // 65536 * 65536 in own_frames and not row[5]]
        sides.append(dict(side=side, local=local, resolutions=evidence, samples=len(ticks),
                          discontinuities=discontinuities, after_confirm_input=after_confirm_input,
                          waiting_for_peer_frames=max(waits, default=0), failures=failures,
                          passed=bool(evidence and ticks and all(e["passed"] for e in evidence) and
                                      not failures and not discontinuities and not after_confirm_input)))
    same = [[(e["epoch"], e["target"]) for e in s["resolutions"]] for s in sides]
    return dict(scenario=scenario, sides=sides, passed=all(s["passed"] for s in sides) and same[0] == same[1])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("scenario", type=int, choices=(0, 1, 2, 3))
    args = parser.parse_args()
    result = analyze(args.folder, args.scenario)
    output = json.dumps(result, ensure_ascii=False, indent=2)
    (args.folder / "native_retry_comparison.json").write_text(output, encoding="utf-8")
    print(output)
    raise SystemExit(0 if result["passed"] else 1)
