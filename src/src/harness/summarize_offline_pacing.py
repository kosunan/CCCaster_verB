"""成立済みの修正前後各2回を、固定1500間隔ずつ全件集計する。"""
import json
from pathlib import Path
from bench_legacy_real import analyze, RECORD, dist

ROOT = Path(__file__).resolve().parents[2]
LOGS = ROOT/'build_logs/offline_pacing_20260913'
OUT = ROOT/'docs/benchmarks/2026-09-13_offline_pacing.json'


def collect():
    data = dict(date='2026-09-13', trials=[], aggregate={})
    hashes = set()
    for kind in ('before','after'):
        errors = []
        for trial in ((1,3) if kind == 'before' else (1,2)):
            # 前面条件が成立した完了試行のみ。ユーザー指定で追加測定は行わない。
            folder = LOGS/f'quiet_{kind}_{trial}'
            result = json.loads((folder/'result.json').read_text(encoding='utf-8'))
            raw = (folder/'frames.bin').read_bytes()
            analysis = analyze(raw)
            fixed = analysis['fixed_battle']
            samples = [s for s in result['foreground_samples'] if fixed['first_tick'] <= s['tick'] <= fixed['last_tick']]
            log = (folder/'cccaster_hook_log.txt').read_text(encoding='utf-8', errors='replace')
            assert result['passed'] and result['protected_unchanged']
            assert result['scene'] == dict(mode=1, stage=50, p1=0, p2=11, p1moon=0, p2moon=0, p1color=0, p2color=0)
            assert samples and all(s['game'] for s in samples), f'前面条件不一致: {folder}'
            assert '[Clock] WASAPI active' in log and 'continuous QPC fallback' not in log
            assert '[OfflinePacing]' not in log, '詳細診断が有効'
            assert result['environment'] == ({'CCCASTER_TEST_OFFLINE_PACING':'legacy'} if kind=='before' else {})
            hashes.add(tuple(sorted(result['files'].values())))
            values = [RECORD.unpack_from(raw,4096+i*RECORD.size)[0] for i in range(fixed['first_ordinal'],fixed['last_ordinal']+1)]
            errors.extend(abs((b-a)*1e6/analysis['frequency']-1e6/60) for a,b in zip(values,values[1:]))
            data['trials'].append(dict(kind=kind, trial=trial, folder=folder.relative_to(ROOT).as_posix(),
                foreground_samples=len(samples), fixed_battle=fixed, files=result['files']))
        data['aggregate'][kind] = dict(abs_error_us=dist(errors), over_3us=sum(e>3 for e in errors),
            over_100us=sum(e>100 for e in errors), over_1ms=sum(e>1000 for e in errors))
    assert len(hashes)==1, '成果物が試行間で異なる'
    OUT.write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n',encoding='utf-8')
    return data


if __name__ == '__main__':
    data=collect()
    print(json.dumps(data['aggregate'],ensure_ascii=False,indent=2))
