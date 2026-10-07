"""ホストのCLI・ゲーム全スレッドを起動時から1論理CPUへ制限し、従来待機への縮退を検証する。"""
import argparse
import json
from pathlib import Path
import re
from analyze_deadline_diagnostics import read
from run_deadline_diagnostics import run


def validate(tables, text):
    pools = tables['BoundaryPool']
    if (len(pools) != 1 or any(pools[0].get(k) != 0 for k in ('capacity', 'active')) or
            pools[0].get('launched', 0) != 0):
        raise ValueError('1論理CPUで補助時計を生成した、または初期化結果がない')
    if not re.search(r'\[GameCpuGuard\].* process=1 before=1 ', text):
        raise ValueError('起動時からのCPU0だけの制限を確認できない')
    if tables['BoundaryWorker'] or tables['BoundaryRace']:
        raise ValueError('補助時計が単一CPUへ割り当てられた')
    releases = {r['f']: r for r in tables['FrameStart']}
    frames = [r['f'] for r in tables['UpdateCadence'] if r['play'] and r['consecutive']]
    if len(frames) < 1000:
        raise ValueError('単一CPUでの実更新標本が不足')
    for frame in frames:
        row = releases[frame]
        if row['actual'] != row['exit'] or row['exit'] < row['due']:
            raise ValueError('従来の実更新時刻への縮退が不正')
    return dict(passed=True, process_affinity=1, helpers_created=0, samples=len(frames), pool=pools[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--spin-prototype', action='store_true')
    args = parser.parse_args()
    output = args.output.resolve()
    # 同期比較にはイントロも含まれる。通常戦闘の実更新を1000標本確保する診断採取。
    env = {'CCCASTER_TEST_PROCESS_AFFINITY_1': '1'}
    if args.spin_prototype:
        env.update(CCCASTER_SPIN_PUBLICATION='1', CCCASTER_SPIN_CAPTURE='1',
                   CCCASTER_DISABLE_MONITOR_PRESENT='1')
    code = run(output, 90, extra_env=env, network='15,25,5')
    if code:
        return code
    text = (output/'game_1.log').read_text(encoding='utf-8')
    result = validate(read(output/'game_1.log'), text)
    if args.spin_prototype:
        tables = read(output/'game_1.log')
        if not tables['PublicationRace'] or not tables['CaptureRace']:
            raise ValueError('入力公開・採取のフォールバックが通っていない')
        if tables['PresentRace']:
            raise ValueError('撤去したPresent監視が動いている')
        if any(r['workers'] != 0 for r in tables['PublicationRace']):
            raise ValueError('単一CPUで観測者が起動した')
        if any(r['worker'] != -1 for r in tables['CaptureRace']):
            raise ValueError('単一CPUで補助採取が起動した')
    (output/'single_cpu.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
