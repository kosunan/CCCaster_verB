"""共通フレーム観測と、待機終了・入力準備のQPCを同じFで照合する。"""
import argparse
import json
from pathlib import Path
import re
from bench_legacy_real import analyze, RECORD, dist


def summarize(folder):
    result = json.loads((folder/'result.json').read_text(encoding='utf-8'))
    raw = (folder/'frames.bin').read_bytes()
    analysis = analyze(raw)
    fixed = analysis['fixed_battle']
    parsed = []
    log = (folder/'cccaster_hook_log.txt').read_text(encoding='utf-8', errors='replace')
    for line in log.splitlines():
        if line.startswith('[OfflinePacing] '):
            parsed.append({k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',line)})
    by_frame = {}
    for sample in parsed:
        by_frame.setdefault(sample['f'], []).append(sample)
    rows = []
    for i in range(fixed['first_ordinal'],fixed['last_ordinal']+1):
        record = RECORD.unpack_from(raw,4096+i*RECORD.size)
        end = record[0]*60000000/analysis['frequency']
        samples = [s for s in by_frame.get(record[2],[]) if 0 <= end-s['waitQpc'] < 2*1000000]
        if not samples:
            continue
        s = max(samples, key=lambda s:s['waitQpc'])
        rows.append(dict(frame=record[2], ordinal=i, end=end,
            wait_late_us=(s['waitAudio']-s['due'])/60,
            prepare_us=(s['tailQpc']-s['waitQpc'])/60,
            after_wait_us=(end-s['waitQpc'])/60,
            sleep_us=(s.get('sleepEnd',0)-s.get('sleepBegin',0))/60,
            sleep_remaining_us=s.get('sleepRemaining',0)/60,
            spin_after_sleep_us=(s['waitQpc']-s['sleepEnd'])/60 if s.get('sleepEnd') else 0))
    timing = {name:dist([r[name] for r in rows]) for name in ('wait_late_us','prepare_us','after_wait_us')}
    # 隣接Fの遅れの差が間隔誤差を作る。単一Fの遅れと間隔誤差を混同しない。
    differences = []
    for a,b in zip(rows,rows[1:]):
        if b['ordinal'] != a['ordinal']+1:
            continue
        interval_error = (b['end']-a['end'])/60-1e6/60
        delta_wait = b['wait_late_us']-a['wait_late_us']
        delta_tail = b['after_wait_us']-a['after_wait_us']
        differences.append(dict(frame=b['frame'], interval_error_us=interval_error,
            delta_wait_us=delta_wait, delta_tail_us=delta_tail,
            sleep_us=b['sleep_us'], sleep_remaining_us=b['sleep_remaining_us'],
            spin_after_sleep_us=b['spin_after_sleep_us'],
            residual_us=interval_error-delta_wait-delta_tail))
    residual = dist([abs(r['residual_us']) for r in differences])
    complete = len(rows) == fixed['intervals'] + 1 and len(differences) == fixed['intervals']
    out = dict(passed=result['passed'] and complete, scene=result['scene'], files=result['files'], environment=result['environment'],
        fixed_battle=fixed, matched_frames=len(rows), stages=timing, residual_abs_us=residual,
        largest_intervals=sorted(differences,key=lambda r:abs(r['interval_error_us']),reverse=True)[:12],
        wasapi_active='[Clock] WASAPI active' in log, qpc_fallback='continuous QPC fallback' in log,
        game_priority=[line for line in log.splitlines() if '[TimingThread] role=game' in line or '[GameCpuPin]' in line])
    (folder/'offline_analysis.json').write_text(json.dumps(out,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return out


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder',type=Path)
    args=parser.parse_args()
    print(json.dumps(summarize(args.folder),ensure_ascii=False,indent=2))
