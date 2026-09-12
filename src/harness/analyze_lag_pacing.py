"""PACE_TRACEログから通常戦闘の表示・実行開始・処理・再計算を分離する。"""
import json, re, statistics, sys
from pathlib import Path

def stats(values):
    values = sorted(values)
    if not values:
        return {'count': 0}
    def q(p): return values[round((len(values)-1)*p)]
    return dict(count=len(values), median=statistics.median(values), p95=q(.95),
                p99=q(.99), maximum=values[-1], over_20ms=sum(v>20000 for v in values))

def analyze(path):
    lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
    def fields(line): return {k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)', line)}
    pace = [fields(line) for line in lines if line.startswith('[Pace]') and 'play=1' in line]
    # 開始演出直後120Fを除外。計測値はログに存在するフレームのみ。
    start = pace[0]['f'] + 120 if pace else 0
    pace = [row for row in pace if row['f'] >= start]
    frames = {r['f'] for r in pace}
    display = [fields(line) for line in lines if line.startswith('[DisplayPace]')]
    display = [row for row in display if row['f'] in frames]
    periods = [b['qpc']-a['qpc'] for a,b in zip(pace,pace[1:]) if b['f']==a['f']+1]
    waits = [fields(line)['elapsedUs'] for line in lines if 'WAIT reason=remote input' in line and fields(line).get('frame') in frames]
    rollups = [fields(line)['elapsedUs'] for line in lines if '[Rollback] END' in line and fields(line).get('target') in frames]
    return dict(path=str(path), first_frame=min(frames,default=0), last_frame=max(frames,default=0),
                display_interval_us=stats([r['interval'] for r in display]),
                simulation_start_interval_us=stats(periods),
                work_us=stats([r['work'] for r in display]),
                simulation_late_us=stats([max(0,r['audio']-r['due']) for r in pace]),
                remote_wait_us=stats(waits), rollup_us=stats(rollups),
                failures=[line for line in lines if 'FAILED' in line])

if __name__ == '__main__':
    result = [analyze(Path(p)) for p in sys.argv[1:]]
    print(json.dumps(result, ensure_ascii=False, indent=2))
