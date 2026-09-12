"""実QPC(1/60us ticks)をF番号で結合。OS要求開始と相手到着を区別する。"""
import argparse
import json
import math
from pathlib import Path
import re


def stats(values):
    if not values:
        return None
    values = sorted(values)
    def percentile(p):
        x = (len(values) - 1) * p
        lo, hi = math.floor(x), math.ceil(x)
        return values[lo] + (values[hi] - values[lo]) * (x - lo)
    return dict(count=len(values), median=percentile(.5), p95=percentile(.95),
                p99=percentile(.99), maximum=values[-1])


def analyze(text):
    captures, sends = {}, {}
    failures = dropped = 0
    for line in text.splitlines():
        if '[InputCaptureSendDropped]' in line or '[InputSendFailure]' in line:
            failures += 1
        if '[InputCaptureSend]' not in line and '[InputSend]' not in line:
            continue
        fields = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
        frame = fields['f']
        if '[InputCaptureSend]' in line:
            if frame in captures:
                failures += 1
            captures[frame] = fields
        elif fields['ok']:
            if frame not in sends or fields['submit'] < sends[frame]['submit']:
                sends[frame] = fields
        else:
            dropped += 1
    rows, unmatched = [], []
    for frame, c in captures.items():
        if frame not in sends:
            unmatched.append(frame)
            continue
        s = sends[frame]
        # dispatchは公開をまたぐBuildPacket中に新入力を見つけた場合だけ公開より早くなり得る。
        if not (c['begin'] <= c['end'] <= c['published'] <= s['submit'] <= s['complete']):
            failures += 1
            continue
        rows.append(dict(frame=frame, direct=s['direct'],
                         poll=(c['end'] - c['begin']) / 60,
                         filter_publish=(c['published'] - c['end']) / 60,
                         handoff=(s['dispatch'] - c['published']) / 60,
                         build=(s['prepared'] - s['dispatch']) / 60,
                         transport_queue=(s['submit'] - s['prepared']) / 60,
                         capture_to_submit=(s['submit'] - c['begin']) / 60,
                         ready_to_submit=(s['submit'] - c['end']) / 60,
                         submit_completion=(s['complete'] - s['submit']) / 60))
    metrics = {key: stats([r[key] for r in rows]) for key in
               ('poll', 'filter_publish', 'handoff', 'build', 'transport_queue',
                'capture_to_submit', 'ready_to_submit', 'submit_completion')}
    return dict(captures=len(captures), matched=len(rows), unmatched=unmatched,
                trace_errors=failures, failed_send_reports=dropped,
                direct_counts={str(d): sum(r['direct'] == d for r in rows) for d in (0, 1)},
                units='microseconds; real QPC; submit is OS request start, not NIC/peer arrival',
                metrics=metrics, worst=sorted(rows, key=lambda r: r['capture_to_submit'], reverse=True)[:10])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = {p.name: analyze(p.read_text(encoding='utf-8', errors='replace'))
              for p in sorted(args.directory.glob('game_[12].log'))}
    (args.directory / 'input_send_summary.json').write_text(
        json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    for name, s in result.items():
        print(name, 'matched', s['matched'], 'unmatched', len(s['unmatched']),
              'errors', s['trace_errors'], 'capture_to_submit', s['metrics']['capture_to_submit'])
    return 0 if len(result) == 2 and all(s['matched'] > 0 and s['trace_errors'] == 0 and
                                       s['failed_send_reports'] == 0 for s in result.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
