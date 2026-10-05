"""フレーム末尾のQPCとPresentMon 1.x CSVを対応させ、戦闘中の表示欠落を集計する。"""
import argparse
from bisect import bisect_left
from collections import Counter
import csv
import json
from pathlib import Path
import statistics
import struct

HEADER = struct.Struct('<4Iq2I')
RECORD = struct.Struct('<qq8I')


def distribution(values):
    if not values:
        return None
    values = sorted(values)
    def percentile(p):
        n = (len(values)-1)*p
        i = int(n)
        return values[i]+(values[min(i+1,len(values)-1)]-values[i])*(n-i)
    return dict(count=len(values), mean=statistics.mean(values), median=statistics.median(values),
                p95=percentile(.95), p99=percentile(.99), maximum=max(values),
                over_20ms=sum(v > 20 for v in values), over_25ms=sum(v > 25 for v in values),
                over_33_34ms=sum(v > 33.34 for v in values))


def correlate_dwm(presents, dwm_rows, frequency):
    """D3D9コピーの推定表示時刻がどのDWM出力に由来するかQPCで照合する。

    複数出力に一致する標本は曖昧として除外し、最寄りの出力へ決め打ちしない。
    異なるETWセッションでも同じQPCを使う。CSVの丸め誤差は1tickまで許容。
    """
    def displayed_tick(row):
        return int(row['QPCTime']) + round(float(row['msUntilDisplayed'])*frequency/1000)
    by_tick, chain_intervals = {}, {}
    for row in dwm_rows:
        if row['Dropped'] != '0' or float(row['msUntilDisplayed']) <= 0:
            continue
        chain = row['SwapChainAddress']
        by_tick.setdefault(displayed_tick(row), set()).add(chain)
        interval = float(row['msBetweenDisplayChange'])
        if interval > 0:
            chain_intervals.setdefault(chain, []).append(interval)
    matched, unmatched, ambiguous, transitions, previous = Counter(), 0, 0, 0, None
    for row in presents:
        if row['Dropped'] != '0' or float(row.get('msUntilDisplayed', 0)) <= 0:
            continue
        tick = displayed_tick(row)
        chains = set().union(*(by_tick.get(tick+offset, set()) for offset in (-1,0,1)))
        if len(chains) == 1:
            chain = chains.pop()
            matched[chain] += 1
            transitions += previous is not None and previous != chain
            previous = chain
        else:
            unmatched += not chains
            ambiguous += len(chains) > 1
            previous = None
    total = sum(matched.values()) + unmatched + ambiguous
    return dict(matched_by_output=dict(matched), unmatched=unmatched, ambiguous=ambiguous,
                matched_fraction=sum(matched.values())/total if total else 0,
                output_switches=transitions, mixed_outputs=len(matched)>1,
                output_display_ms={k:distribution(v) for k,v in chain_intervals.items()})


def analyze(directory):
    directory = Path(directory)
    capture = json.loads((directory/'capture.json').read_text(encoding='utf-8'))
    data = (directory/'frames.bin').read_bytes()
    header = HEADER.unpack_from(data)
    if header[0] != 0x42434343 or header[1] != 1 or header[6] != RECORD.size or header[5] != 1:
        raise ValueError(f'観測器ヘッダー不正または標本欠落: {header}')
    records = [dict(zip(('tick','end','world','mode','real','round_timer','skip','intro','ordinal','thread'),row))
               for row in RECORD.iter_unpack(data[4096:4096+header[2]*RECORD.size])]
    if len(records) != header[2]:
        raise ValueError('観測器の記録が途中で切れています')
    times = [r['tick'] for r in records]
    frames = [[] for _ in records]
    with (directory/'presents.csv').open(encoding='utf-8-sig', newline='') as source:
        rows = list(csv.DictReader(source))
    for row in rows:
        if int(row['ProcessID']) != capture['game_pid']:
            continue
        # 観測点は当該Presentからゲームへ戻り、締切待機を終えた直後。
        tick = int(row['QPCTime'])
        index = bisect_left(times, tick)
        if index < len(times) and times[index]-tick < header[4]*.5:
            row['_tick'] = tick
            row['_frame'] = index
            frames[index].append(row)
    segments = []
    for i,r in enumerate(records):
        if r['mode'] != 1:
            continue
        if not segments or i != segments[-1][-1]+1 or r['real'] < records[i-1]['real']:
            segments.append([])
        segments[-1].append(i)
    battle_indices = [i for segment in segments for i in segment]
    def summarize(indices):
        selected = [row for i in indices for row in frames[i]]
        consecutive = [(a,b) for a,b in zip(indices,indices[1:]) if b==a+1]
        intervals = [(records[b]['tick']-records[a]['tick'])*1000/header[4] for a,b in consecutive]
        gaps = [(a,b,records[b]['world']-records[a]['world']) for a,b in consecutive
                if records[b]['world'] != records[a]['world']+1]
        presented = [r for r in selected if r['Dropped']=='0']
        known = set(indices)
        # 場面への最初の提示は、前の場面からの時間を含むので間隔の集計から除外。
        present_intervals = [float(r['msBetweenPresents']) for r in selected
                             if r['_frame']-1 in known and float(r['msBetweenPresents']) > 0]
        display_intervals = [float(r['msBetweenDisplayChange']) for r in presented
                             if r['_frame']-1 in known and float(r.get('msBetweenDisplayChange',0)) > 0]
        foreground = [r['game'] for r in capture['foreground']
                      if bisect_left(times,r['tick']) in known]
        return dict(game_frames=len(indices), presents=len(selected), displayed=len(presented),
                    dropped=len(selected)-len(presented),
                    no_present_frames=sum(not frames[i] for i in indices),
                    multi_present_frames=sum(len(frames[i])>1 for i in indices),
                    native_skip_values=dict(Counter(records[i]['skip'] for i in indices)),
                    world_discontinuities=len(gaps), first_discontinuities=gaps[:10],
                    foreground_samples=len(foreground), foreground_all=bool(foreground) and all(foreground),
                    loop_ms=distribution(intervals), present_ms=distribution(present_intervals),
                    display_ms=distribution(display_intervals),
                    render_complete_ms=distribution([float(r['msUntilRenderComplete']) for r in selected
                                                      if r.get('msUntilRenderComplete')]),
                    display_latency_ms=distribution([float(r['msUntilDisplayed']) for r in presented
                                                      if r.get('msUntilDisplayed')]),
                    gpu_active_ms=distribution([float(r['msGPUActive']) for r in selected
                                                if r.get('msGPUActive')]),
                    display_interval_error_ms=distribution([abs(v-1000/60) for v in display_intervals]),
                    display_within_1ms_of_60hz=sum(abs(v-1000/60)<=1 for v in display_intervals),
                    present_api_ms=distribution([float(r['msInPresentAPI']) for r in selected]),
                    present_modes=dict(Counter(r.get('PresentMode','') for r in selected)),
                    swapchains=dict(Counter(r['SwapChainAddress'] for r in selected)))
    result = dict(directory=str(directory.resolve()), replay=capture['replay'],
                  replay_sha256=capture['replay_sha256'], capture_complete=capture['capture_complete'],
                  protected_changes=capture['protected_changes'], observer_status=header[5],
                  total_csv_rows=len(rows), battle=summarize(battle_indices),
                  rounds=[summarize(segment) for segment in segments],
                  active_battle=summarize([i for i in battle_indices if records[i]['intro']==0]),
                  observer_cost_us={k:v for k,v in distribution(
                      [(r['end']-r['tick'])*1e6/header[4] for r in records]).items() if not k.startswith('over_')})
    # 上位の遅い表示要求を残し、場面・ゲームフレームと突き合わせられるようにする。
    selected = [row for i in battle_indices for row in frames[i]]
    result['slowest_presents'] = [dict(world=records[r['_frame']]['world'],
        intro=records[r['_frame']]['intro'], tick=r['_tick'],
        interval_ms=float(r['msBetweenPresents']), dropped=int(r['Dropped']),
        display_ms=float(r.get('msBetweenDisplayChange',0)))
        for r in sorted(selected,key=lambda r:float(r['msBetweenPresents']),reverse=True)[:15]]
    result['dropped_battle_frames'] = [dict(world=records[r['_frame']]['world'],
        intro=records[r['_frame']]['intro'], tick=r['_tick'], interval_ms=float(r['msBetweenPresents']))
        for r in selected if r['Dropped']=='1']
    result['dropped_by_game_mode'] = dict(Counter(records[i]['mode'] for i,group in enumerate(frames)
                                                for row in group if row['Dropped']=='1'))
    result['game_present_trace_available'] = any(int(r['ProcessID'])==capture['game_pid'] for r in rows)
    result['valid_for_drop_assessment'] = bool(capture['capture_complete'] and
        result['game_present_trace_available'] and not capture['protected_changes'])
    result['display_timing_scope'] = ('compositor_estimate_unvalidated'
        if any('Composed: Copy' in r.get('PresentMode','') for r in selected) else 'presentmon')
    if capture.get('dwm_pid'):
        result['dwm_correlation'] = correlate_dwm(
            [r for r in selected if records[r['_frame']]['intro']==0],
            [r for r in rows if int(r['ProcessID'])==capture['dwm_pid']], header[4])
    elif (directory/'dwm.csv').exists():
        with (directory/'dwm.csv').open(encoding='utf-8-sig', newline='') as file:
            result['dwm_correlation'] = correlate_dwm(
                [r for r in selected if records[r['_frame']]['intro']==0], list(csv.DictReader(file)), header[4])
    correlation = result.get('dwm_correlation', {})
    if correlation.get('mixed_outputs') and correlation.get('matched_fraction',0) >= .95:
        result['display_timing_scope'] = 'mixed_dwm_outputs_not_single_monitor_timing'
    if not result['game_present_trace_available']:
        result['display_timing_scope'] = 'unavailable_game_trace'
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = analyze(args.directory)
    (args.directory/'analysis.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2))
