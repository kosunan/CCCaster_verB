"""設定表示だけでなく、DLLの実受信保留時間と往復測定値を検査する。"""
import argparse
import json
import re
import statistics
from pathlib import Path


def analyze_text(text, loss=20):
    configuration = re.findall(r'\[NetworkTest\] receive delay=(\d+)\.\.(\d+) ms loss=(\d+)%', text)
    samples = [tuple(map(int, row)) for row in re.findall(
        r'\[NetworkTest\] delayed=(\d+) dropped=(\d+) requestedUs=(\d+) observedUs=(\d+)', text)]
    rtt = [int(x) for x in re.findall(r'\[NetplaySession\].*? RTT=(\d+)us', text) if int(x) > 0]
    count, dropped, requested, observed = samples[-1] if samples else (0, 0, 0, 0)
    mean_requested = requested/count if count else 0
    mean_observed = observed/count if count else 0
    median_rtt = statistics.median(rtt) if rtt else 0
    # 4カウンターは個別atomic読出しなので、記録中の1件増加分に1msの余裕を取る。
    # OSの遅配は200msを超え得るため、実測上限を設定値で代用して合格させない。
    passed = (configuration == [('150', '200', str(loss))] and count >= 100
              and 150000 <= mean_requested <= 200000
              and mean_observed >= mean_requested-1000 and median_rtt >= 250000)
    return dict(passed=passed, delayed_packets=count, dropped_packets=dropped,
                requested_mean_ms=round(mean_requested/1000, 3),
                observed_hold_mean_ms=round(mean_observed/1000, 3), rtt_samples=len(rtt),
                rtt_median_ms=round(median_rtt/1000, 3),
                rtt_min_ms=round(min(rtt)/1000, 3) if rtt else None,
                rtt_max_ms=round(max(rtt)/1000, 3) if rtt else None)


def analyze(folder, loss=20):
    sides = [analyze_text((folder/f'pair/game_{i}.log').read_text(encoding='utf-8'), loss) for i in (1, 2)]
    return dict(passed=all(side['passed'] for side in sides), sides=sides,
                scope='対戦UDPの受信後からコールバックまでの実時間集計とRTT。設定範囲は片道150〜200ms。'
                      '個別パケットの最大保留時間・物理NIC遅延・観戦TCPへの遅延注入は測定対象外。')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--loss-percent', type=int, default=20)
    args = parser.parse_args()
    result = analyze(args.folder, args.loss_percent)
    output = json.dumps(result, indent=2, ensure_ascii=False)
    (args.folder/'network_comparison.json').write_text(output, encoding='utf-8')
    print(output)
    raise SystemExit(0 if result['passed'] else 1)
