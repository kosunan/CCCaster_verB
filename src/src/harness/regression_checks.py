"""シナリオが目的の状態まで到達したかを、同期一致とは独立して判定する。"""


def resumed_frames(comparison, retry):
    # 再戦前の複数ラウンドだけで「復帰成功」と誤認しない。
    first = max(side['resolutions'][0]['epoch'] for side in retry['sides']) // 65536
    return sum(int(last) % 65536 for epoch, last in comparison['confirmed_by_epoch'].items()
               if int(epoch) > first)


def baseline_failures(current, previous, ratio):
    if any(previous.get(k) != current.get(k) for k in ('name', 'case', 'seconds', 'network', 'cheats')):
        return ['基準の試験条件が不一致']
    failures = []
    for side in ('host', 'client'):
        old = previous['comparison'][side]['rollup_p99_us']
        new = current['comparison'][side]['rollup_p99_us']
        if old <= 0 or new <= 0 or new > old * ratio:
            failures.append(f'{side}: 再計算p99が基準超過またはサンプルなし {old} -> {new}us')
    return failures
