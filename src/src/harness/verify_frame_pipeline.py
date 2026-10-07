"""実時刻で待機→入力→保存→native更新→最終描画の順序と各区間を確認する。"""
import argparse
import json
import re
from pathlib import Path
from analyze_deadline_diagnostics import distribution


def verify(text, first=None, last=None, minimum=120, require_saved=False, spike_us=0):
    tables = {name: {} for name in ('FrameStart', 'FramePipeline', 'UpdateCadence')}
    failures = []
    for line in text.splitlines():
        tag = re.match(r'\[(\w+)\]', line)
        if not tag or tag[1] not in tables:
            continue
        row = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
        if 'f' not in row:
            failures.append('フレーム番号欠落')
            continue
        if row['f'] in tables[tag[1]]:
            failures.append(f'{tag[1]}: 通常フレームの重複 {row["f"]}')
        tables[tag[1]][row['f']] = row
    rows = []
    for frame, update in tables['UpdateCadence'].items():
        if not update.get('play') or not update.get('consecutive'):
            continue
        if first is not None and frame < first or last is not None and frame >= last:
            continue
        start = tables['FrameStart'].get(frame, {})
        row = tables['FramePipeline'].get(frame, {})
        previous = tables['FrameStart'].get(update.get('prev'), {})
        try:
            assert start['actual'] == row['start'] == update['ticks'], '入口の計測境界が不一致'
            assert row['start'] - previous['actual'] == update['interval'], '実測周期が不一致'
            assert update['dropped'] == 0, '通常更新の標本欠落'
            assert 0 < row['start'] <= row['input'] <= row['prepared'] <= row['native'] <= row['present'], '入力／更新／描画の順序が不正'
            if require_saved:
                assert row['input'] <= row['saved'] <= row['prepared'], '状態保存が処理入口より前'
            assert start['actual'] >= start['due'], '締切前に処理開始'
            rows.append(dict(row, interval=update['interval'], readyLate=start['readyLate']))
        except (AssertionError, KeyError) as exc:
            failures.append(f'{frame}: {exc}')
    if len(rows) < minimum:
        failures.append(f'標本不足: {len(rows)} < {minimum}')
    pairs = [(a,b) for a,b in zip(rows, rows[1:]) if b['f'] == a['f']+1]
    stats = dict(
        start_interval_us=distribution([r['interval']/60 for r in rows]),
        input_us=distribution([(r['input']-r['start'])/60 for r in rows]),
        preparation_us=distribution([(r['prepared']-r['start'])/60 for r in rows]),
        native_entry_us=distribution([(r['native']-r['start'])/60 for r in rows]),
        work_to_render_us=distribution([(r['present']-r['start'])/60 for r in rows]),
        native_interval_us=distribution([(b['native']-a['native'])/60 for a,b in pairs]),
        render_interval_us=distribution([(b['present']-a['present'])/60 for a,b in pairs]))
    spikes = [r for r in rows if r['f'] % 60 == 0]
    if spike_us:
        if len(spikes) < minimum // 60:
            failures.append('負荷注入フレーム不足')
        if any(r['input']-r['start'] < spike_us*60 for r in spikes):
            failures.append('指定した準備負荷を実時間で観測できない')
    return dict(passed=not failures, failures=failures, samples=len(rows), stats=stats,
                boundary='before input and snapshot; present is final rendering before Present',
                ready_after_deadline=sum(r['readyLate']>0 for r in rows),
                interval_outside_3us=sum(abs(r['interval']-1_000_000)>180 for r in rows),
                spike_frames=len(spikes) if spike_us else 0,
                top_preparation=sorted(rows,key=lambda r:r['prepared']-r['start'],reverse=True)[:5])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--saved', action='store_true')
    parser.add_argument('--last', type=int)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = verify(args.log.read_text(encoding='utf-8',errors='replace'), last=args.last, require_saved=args.saved)
    args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(report,ensure_ascii=False))
    raise SystemExit(not report['passed'])
