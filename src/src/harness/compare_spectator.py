"""観戦の各世代の受信先頭から確定済み範囲を照合する。停止試験の末尾切断は別記する。"""
import argparse
import json
import re
from pathlib import Path
from compare_rollback_pair import read, FIELDS, EXCLUDED
from analyze_intro_pair import read as read_intro


def compare(folder, minimum=1000, minimum_starts=1):
    host, confirmed, hstats = read(folder / 'pair/game_1.log')
    viewer, _, vstats = read(folder / 'viewer.log')
    text = (folder / 'viewer.log').read_text(encoding='utf-8')
    result = {'fields': {}, 'scope': '確定入力、代表状態。絶対WTとメニューカウンターは除外。全メモリ完全性は対象外。',
              'host_failures': hstats['failures'], 'viewer_termination': vstats['failures'],
              'starts': [int(f) for f in re.findall(r'\[Spectator\] START frame=(\d+)', text)],
              'retries': re.findall(r'\[Spectator\] RETRY target=(\d+)', text),
              'scores': re.findall(r'\[Spectator\] SCORE revision=(\d+) p1=(\d+) p2=(\d+)', text),
              'catchup_frames': len(re.findall(r'\[Spectator\] FRAME f=\d+ catch=1', text)),
              'normal_frames': len(re.findall(r'\[Spectator\] FRAME f=\d+ catch=0', text))}
    # 自動試験のホスト終了だけは許容。観戦途中の失敗を成功扱いしない。
    unexpected = [failure for failure in vstats['failures'] if 'spectator connection closed' not in failure]
    result['unexpected_viewer_failures'] = unexpected
    passed = not hstats['failures'] and not unexpected and len(result['starts']) >= minimum_starts
    for tag in FIELDS:
        frames = sorted(f for f in host[tag].keys() & viewer[tag].keys() if f <= confirmed.get(f // 65536, 0))
        bad = [f for f in frames if any(a != b for i, (a, b) in enumerate(zip(host[tag][f], viewer[tag][f]))
                                       if i not in EXCLUDED.get(tag, set()))]
        epochs = sorted({f // 65536 for f in frames})
        missing = [f for e in epochs for f in range(e * 65536 + 1, max(f for f in frames if f // 65536 == e) + 1)
                   if f not in host[tag] or f not in viewer[tag]]
        result['fields'][tag] = {'frames': len(frames), 'different': len(bad), 'missing': len(missing), 'first_different': bad[:8]}
        passed &= len(frames) >= minimum and not bad and not missing
    a, _, _ = read_intro(folder / 'pair/game_1.log')
    b, _, _ = read_intro(folder / 'viewer.log')
    frames = sorted(f for f in viewer['[REC]'] if f <= confirmed.get(f // 65536, 0))
    missing = [f for f in frames if f not in a or f not in b]
    bad = [f for f in frames if f in a and f in b and a[f] != b[f]]
    result['intro'] = dict(frames=len(frames), different=len(bad), missing=len(missing), first_different=bad[:8])
    passed &= not missing and not bad
    # 設定倍率ではなく、連続する追いつき区間の実時刻と処理済みF数を報告する。
    samples = [(int(f), int(c), int(q), int(t)) for f, c, q, t in re.findall(
        r'\[Spectator\] FRAME f=(\d+) catch=(\d+) buffered=(\d+) qpc=(\d+)', text)]
    segments, begin = [], None
    for index, (_, catch, _, _) in enumerate(samples):
        if catch and begin is None:
            begin = index
        if not catch and begin is not None:
            elapsed = (samples[index][3] - samples[begin][3]) / 1e6
            segments.append(dict(frames=index-begin, seconds=round(elapsed, 6),
                                 updates_per_second=round((index-begin)/elapsed, 1) if elapsed else None,
                                 backlog_before=samples[begin][2], backlog_after=samples[index][2]))
            begin = None
    result['completed_catchup'] = segments
    restored, resumed = set(), []
    for frame, catch, _, _ in samples:
        epoch = frame // 65536
        if not catch:
            restored.add(epoch)
        elif epoch in restored:
            resumed.append(frame)
    result['skip_resumed_after_catchup'] = resumed[:8]
    passed &= not resumed
    result['passed'] = bool(passed)
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--min-frames', type=int, default=1000)
    parser.add_argument('--min-starts', type=int, default=1)
    args = parser.parse_args()
    result = compare(args.folder, args.min_frames, args.min_starts)
    (args.folder / 'spectator_comparison.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    print(json.dumps(result, indent=2, ensure_ascii=False))
    raise SystemExit(0 if result['passed'] else 1)
