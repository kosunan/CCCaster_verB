"""実ログの必要条件が揃ったら終了を要求する。最終回収ログも同じ判定で再検査する。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import time

from compare_rollback_pair import compare
from compare_spectator import compare as compare_spectator


def clean_environment():
    return {k: v for k, v in os.environ.items()
            if not k.upper().startswith(('CCCASTER_', 'CCBENCH_'))}


def protected_hashes(runtime):
    files = [p for side in (1, 2, 3) for p in [
        *(runtime / f'MBAACC_{side}').rglob('*.ini'), runtime / f'MBAACC_{side}/MBAA.exe']]
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}


def evaluate(folder, config):
    for side in ((1, 2, 3) if config.get('spectator') else (1, 2)):
        text = (folder / f'game_{side}.log').read_text(encoding='utf-8')
        if text.count('[InitThread] Starting hook initialization...') > 1:
            return dict(passed=False, fatal_error=f'game_{side}.log: 複数ゲームの初期化記録が混入')
    result = dict(sync=compare(folder))  # 最低1000F・RB発生を維持する。
    result['passed'] = result['sync']['passed']
    if config.get('scenario'):
        from run_stage_rematch import evaluate_behavior
        result.update(evaluate_behavior(folder, config))
        result['passed'] = result['sync']['passed'] and result['behavior_passed']
    if config.get('spectator'):
        view = folder / 'spectator_comparison'
        (view / 'pair').mkdir(parents=True, exist_ok=True)
        shutil.copyfile(folder / 'game_1.log', view / 'pair/game_1.log')
        shutil.copyfile(folder / 'game_3.log', view / 'viewer.log')
        result['spectator'] = compare_spectator(view)
        result['passed'] &= result['spectator']['passed']
    return result


def snapshot(runtime, folder, spectator):
    # 書込み途中の末尾行は次のポーリングで再読する。元ログは変更しない。
    for side in ((1, 2, 3) if spectator else (1, 2)):
        source = runtime / f'MBAACC_{side}/cccaster_B/cccaster_hook_log.txt'
        data = source.read_bytes()
        data = data[:data.rfind(b'\n') + 1]
        (folder / f'game_{side}.log').write_bytes(data)


def monitor(runtime, output, config, seconds, interval=1.0):
    folder = output / 'checkpoint_snapshot'
    folder.mkdir()
    started = time.monotonic()
    attempts = 0
    last = dict(passed=False, error='ログ未出力')
    while True:
        attempts += 1
        try:
            snapshot(runtime, folder, config.get('spectator'))
            last = evaluate(folder, config)
        except (OSError, UnicodeError) as exc:
            last = dict(passed=False, error=str(exc))
        except ValueError as exc:
            last = dict(passed=False, fatal_error=f'解析できないログ: {exc}')
        elapsed = time.monotonic() - started
        last.update(elapsed_seconds=round(elapsed, 3), attempts=attempts,
                    stop_reason='conditions_met' if last['passed'] else 'waiting', config=config)
        if not last['passed'] and elapsed >= seconds:
            last['stop_reason'] = 'timeout'
        if last.get('fatal_error'):
            last['stop_reason'] = 'invalid_log'
        temporary = output / 'checkpoint.tmp'
        temporary.write_text(json.dumps(last, ensure_ascii=False, indent=2), encoding='utf-8')
        temporary.replace(output / 'checkpoint.json')
        if last['passed'] or elapsed >= seconds or last.get('fatal_error'):
            return 0 if last['passed'] else 1
        time.sleep(min(interval, max(0, seconds - elapsed)))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--seconds', type=float, required=True)
    args = parser.parse_args()
    raise SystemExit(monitor(args.runtime, args.output,
                            json.loads(args.config.read_text(encoding='utf-8')), args.seconds))
