"""2026-09-13の旧版比較を生データから再集計し、配布用JSONを生成する。"""
import hashlib
import json
from pathlib import Path
import statistics
from bench_legacy_real import analyze

ROOT = Path(__file__).resolve().parents[2]
LOGS = ROOT / 'build_logs/legacy_benchmark_20260913'
OUTPUT = ROOT / 'docs/benchmarks'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    startup, cadence, evidence = {}, {}, {}
    for variant in ('old', 'new'):
        trials = []
        for iteration in (1, 2, 3):
            folder = LOGS / f'startup_{iteration}_{variant}'
            result = json.loads((folder / 'result.json').read_text(encoding='utf-8'))
            assert result['passed'] and result['protected_unchanged']
            trials.append({key: result[key] for key in ('startup_seconds', 'poll_bracket_us',
                'start_tick', 'ready_tick', 'ready_previous_poll_tick', 'files')})
            evidence[str((folder/'result.json').relative_to(ROOT))] = sha(folder/'result.json')
        values = [r['startup_seconds'] for r in trials]
        startup[variant] = dict(trials=trials, median=statistics.median(values), min=min(values), max=max(values))
    for variant, name in [('old', 'stage50_old_final'), ('new', 'fixed_new')]:
        folder = LOGS / name
        result = json.loads((folder/'result.json').read_text(encoding='utf-8'))
        assert result['passed'] and result['protected_unchanged']
        expected = dict(mode=1, stage=50, p1=0, p2=11, p1moon=0, p2moon=0, p1color=0, p2color=0)
        assert result['scene'] == expected
        analysis = analyze((folder/'frames.bin').read_bytes())
        assert analysis.get('fixed_battle'), f'{name}: 固定区間不足'
        assert result['files'] == startup[variant]['trials'][0]['files']
        cadence[variant] = dict(folder=str(folder.relative_to(ROOT)), scene=result['scene'],
            measurement=analysis['fixed_battle'], frequency=analysis['frequency'], probe_cost_us=analysis['probe_cost_us'])
        for filename in ('result.json', 'frames.bin'):
            evidence[str((folder/filename).relative_to(ROOT))] = sha(folder/filename)
    o, n = startup['old']['median'], startup['new']['median']
    sources = [ROOT/'src/harness'/name for name in ('bench_legacy_real.py', 'legacy_benchmark_probe.cpp',
        'legacy_benchmark_inject.cpp', 'build_legacy_benchmark.ps1', 'test_legacy_benchmark.py',
        'summarize_legacy_benchmark.py', 'plot_legacy_benchmark.py')]
    for name in ('targets/DllMain.cpp', 'targets/DllFrameRate.cpp', 'targets/DllNetplayManager.cpp',
                 'lib/ConsoleUi.cpp'):
        sources.append(ROOT/'.ai_workspace/legacy_bench_20260913/old_source'/name)
    result = dict(date='2026-09-13', target='MBAACC 1.07 Rev.1.4.0 x86',
        legacy='I:/work_space/CCCaster/OLD の3.1.007ローカルソースのReleaseビルド（コンソールフォント列挙0件の回避のみ追加）',
        machine=dict(cpu='AMD Ryzen 7 5800X3D', cores=8, threads=16, gpu='NVIDIA GeForce RTX 5070 Ti',
            driver='32.0.16.1088', os='Windows 11 Pro 10.0.26200', memory_bytes=51460755456),
        startup=startup, startup_change=dict(reduction_percent=100*(1-n/o), saved_seconds=o-n, speed_ratio=o/n),
        cadence=cadence, evidence_sha256=evidence,
        source_sha256={str(p.relative_to(ROOT)): sha(p) for p in sources},
        measurement_tools=json.loads((LOGS/'measurement_tools.json').read_text(encoding='utf-8')),
        scope='起動は交互各3回。周期は各1回、120F準備後の最初の1500連続区間。ロールアップ・ネット対戦・物理入力遅延は今回の比較対象外。')
    OUTPUT.mkdir(parents=True, exist_ok=True)
    target = OUTPUT/'2026-09-13_legacy_comparison.json'
    target.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(dict(startup=result['startup_change'], cadence={v:c['measurement'] for v,c in cadence.items()}), ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
