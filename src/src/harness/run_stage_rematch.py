"""ランダムONCE・固定ONCE・片側キャラセレを短縮試合と現行P2Pで検証する。"""
import argparse
from contextlib import ExitStack
import json
from pathlib import Path
import re
import shutil
import socket
import statistics
import subprocess
import sys
import threading
import time

from test_p2p_service import Service, free_port, ROOT
from real_game_checkpoint import evaluate, clean_environment, protected_hashes
from verify_selection_pair import validate_load_sequence

EXCLUDED = {32, 43, 44, 51, 54, 57, 58, 59}
VALID_RANDOM = set(range(1, 60)) - EXCLUDED - {11, 14, 15, 33}
SELECTION = r"\[Select\] {} p1=(\d+)/(\d+)/(\d+) p2=(\d+)/(\d+)/(\d+) stage=(\d+)"


def analyze_assembly_text(text, expected):
    active = False
    changes = []
    failures = []
    for line in text.splitlines():
        event = re.search(r'\[StageRematchAsm\] enabled=([01]) patches=(\d+)', line)
        if event:
            enable, count = map(int, event.groups())
            if bool(enable) == active or count != 1:
                failures.append(line)
            active = bool(enable)
            changes.append(enable)
        if active and '[TransitionDraw] mode=1 ' in line:
            failures.append('イントロ開始前に命令が復元されていない')
        if '[StageRematchAsm] FAILED' in line:
            failures.append(line)
    passed = not failures and not active and (bool(changes) == expected)
    return dict(changes=changes, failures=failures, passed=passed)


def analyze_direct_rematch(text):
    transitions, failures, assets = [], [], []
    current = None
    for line in text.splitlines():
        direct = re.search(r'\[StageRematch\] DIRECT stage=(\d+) mode=(\d+)', line)
        if direct:
            if current is not None:
                failures.append('前の直接再戦が完了していない')
            stage, mode = map(int, direct.groups())
            if mode != 5 or stage not in VALID_RANDOM:
                failures.append('再戦画面以外または不正なステージで確定した')
            current = dict(stage=stage, loading=False, intro=False, asset=False)
            transitions.append(current)
        draw = re.search(r'\[TransitionDraw\] mode=(\d+) ', line)
        if draw and current is not None:
            mode = int(draw[1])
            if mode == 20:
                failures.append('直接再戦でキャラ選択を通過した')
            if mode == 8:
                current['loading'] = True
            if mode == 1:
                if not current['loading']:
                    failures.append('ロードを通らず対戦へ入った')
                current['intro'] = True
                current = None
        asset = re.search(r'\[StageAsset\] selected=(\d+) loaded=(\d+) expanded=(\d+) file=(.*)', line)
        if asset:
            selected, loaded, expanded = map(int, asset.groups()[:3])
            assets.append(loaded)
            if selected != loaded or expanded != 1 or not asset[4].strip():
                failures.append('選択番号と実ロード資産が不一致')
            if transitions:
                transitions[-1]['asset'] = selected == loaded == transitions[-1]['stage']
                if not transitions[-1]['asset']:
                    failures.append('再抽選結果とロード後の背景が不一致')
    complete = bool(transitions and all(t['loading'] and t['intro'] and t['asset'] for t in transitions))
    # 初回と再戦後の資産を確認。ラウンド開始でも同じ資産が記録される。
    return dict(transitions=transitions, assets=assets, failures=failures,
                passed=bool(complete and len(assets) >= 2 and not failures))


def analyze_intro_text(text):
    rows = []
    replaying = False
    for line in text.splitlines():
        if '[Rollback] BEGIN' in line:
            replaying = True
        elif '[Rollback] END' in line:
            replaying = False
        row = re.search(r'\[TransitionDraw\] mode=(\d+) intro=(\d+) skip=(\d+) replay=(\d+) qpc=(\d+)', line)
        if row:
            mode, intro, skip, replay, qpc = map(int, row.groups())
            # 最後の再計算画像では次更新用replayカウンターが既に終端を指す。
            rows.append((mode, intro, skip, int(replay or replaying), qpc))
    battles = []
    previous_mode = None
    for row in rows:
        if row[0] == 1:
            if previous_mode != 1:
                battles.append([])
            battles[-1].append(row)
        previous_mode = row[0]
    results = []
    for battle in battles:
        intro = [r for r in battle if r[1] in (1, 2) and not r[3]]
        intervals = [b[4] - a[4] for a, b in zip(battle, battle[1:])
                     if a[1] in (1, 2) and b[1] in (1, 2) and not any((a[2], b[2], a[3], b[3]))]
        results.append(dict(first_skipped=bool(battle[0][2]), intro_frames=len(intro),
                            intro_skips=sum(r[2] for r in intro),
                            median_interval_us=statistics.median(intervals) if intervals else None,
                            maximum_interval_us=max(intervals, default=0),
                            entered_battle=any(r[1] == 0 for r in battle)))
    return dict(battles=results, passed=bool(len(results) >= 2 and all(
        not r['first_skipped'] and not r['intro_skips'] and
        r['intro_frames'] >= 100 and r['entered_battle'] for r in results)))


def analyze_loading(folder, requested):
    sides = []
    for side in (1, 2, 3):
        text = (folder / f'game_{side}.log').read_text(encoding='utf-8')
        rows = [tuple(map(int, row)) for row in re.findall(
            r'\[TransitionDraw\] mode=(\d+) intro=(\d+) skip=(\d+) replay=(\d+) qpc=(\d+)', text)]
        intro = [r for r in rows if r[0] == 1 and not r[3]]
        begin = text.find('[Spectator] INTRO ENTER') if side == 3 else text.find('[LoadingInput] BEGIN')
        end = text.find('[Spectator] START') if side == 3 else text.find('[LoadingInput] END')
        presses = len(re.findall(r'\[LoadingInput\] PRESS ', text[begin:end])) if side != 3 else 0
        requests = re.findall(r'\[LoadingSkip\] REQUEST role=\d+ epoch=(\d+) source=(local|peer) qpc=(\d+)', text[begin:end])
        expected = requested == 'both' or requested == ('host' if side == 1 else 'client')
        passed = bool(intro and intro[0][1] == 2 and not intro[0][2] and begin >= 0 and end > begin)
        if side != 3:
            if requested != 'both':
                passed &= bool(presses) == expected
            passed &= len(requests) == (0 if requested == 'none' else 1)
            if requests and intro:
                if requested != 'both':
                    passed &= requests[0][1] == ('local' if expected else 'peer')
                # 実データの読込み時間をスキップ性能と混同しない。
                passed &= int(requests[0][2]) <= intro[0][4]
        else:
            # イントロ入口到達からStart到着までは、確定入力を1Fも再生しない。
            passed &= '[Spectator] FRAME ' not in text[begin:end]
        sides.append(dict(side=side, presses=presses, intro_qpc=intro[0][4] if intro else 0,
                          requests=requests,
                          first_gap_us=intro[1][4] - intro[0][4] if len(intro) > 1 else 0,
                          passed=passed))
    # 観戦は先着して待機。片側だけの押下でも両端末が同じ世代をスキップする。
    order = sides[2]['intro_qpc'] < min(s['intro_qpc'] for s in sides[:2])
    epochs = [s['requests'][0][0] for s in sides[:2] if s['requests']]
    shared = not epochs if requested == 'none' else len(epochs) == 2 and epochs[0] == epochs[1]
    if requested == 'both':
        shared &= any(s['presses'] for s in sides[:2])
    if requested in ('host', 'client'):
        # 同じ小さい資産を使うこの試験では、押さなかった側も押下側の到達後
        # 2秒以内に追従する。自然終了まで片側だけ待つ旧挙動を検出する。
        source, peer = (0, 1) if requested == 'host' else (1, 0)
        shared &= sides[peer]['intro_qpc'] < sides[source]['intro_qpc'] + 2000000
    return dict(sides=sides, arrival_order=order, shared_skip=shared,
                passed=all(s['passed'] for s in sides) and order and shared)


def analyze_spectator_drawing(text, random_rematch=False):
    selected = loading = intros = 0
    hidden_selection = hidden_loading = rematches = 0
    reselecting = False
    failures = []
    previous_intro = None
    for line in text.splitlines():
        retry = re.search(r'\[Spectator\] RETRY target=(\d+)', line)
        if retry:
            reselecting = int(retry[1]) == 2
            rematches += reselecting
        row = re.search(r'\[TransitionDraw\] mode=(\d+) intro=(\d+) skip=(\d+) replay=(\d+) qpc=(\d+)', line)
        if not row:
            continue
        mode, intro, skip, replay, qpc = map(int, row.groups())
        if mode == 1:
            reselecting = False  # 最初のイントロ画像から復帰する。
        visible = mode in (20, 8) or mode == 1 and (intro in (1, 2) or previous_intro in (1, 2))
        expected_skip = int(reselecting and mode in (20, 8))
        if visible and skip != expected_skip and not replay:
            failures.append(dict(mode=mode, intro=intro, expected_skip=expected_skip, qpc=qpc))
        selected += mode == 20 and not reselecting
        loading += mode == 8 and not reselecting
        hidden_selection += mode == 20 and reselecting
        hidden_loading += mode == 8 and reselecting
        intros += mode == 1 and intro in (1, 2)
        previous_intro = intro if mode == 1 else None
    # 描画を戻しても待機省略の高速モードは維持する。
    fast_visible = '[Spectator] PACE fast=1 draw=1' in text
    fast_hidden = '[Spectator] PACE fast=1 draw=0' in text
    scope_ok = bool(rematches and not hidden_selection and hidden_loading and fast_hidden) if random_rematch else not rematches
    return dict(selection_frames=selected, loading_frames=loading, intro_frames=intros,
                hidden_selection_frames=hidden_selection, hidden_loading_frames=hidden_loading,
                random_rematches=rematches, fast_visible=fast_visible, fast_hidden=fast_hidden, failures=failures,
                passed=bool(selected and loading and intros and fast_visible and scope_ok and not failures))


def analyze_intro_wait(text):
    previews = []
    current = None
    failures = []
    for line in text.splitlines():
        if '[Spectator] INTRO PREVIEW ' in line:
            if current is not None:
                failures.append('前の表示準備が終わらず次のイントロへ進んだ')
            stamp = re.search(r'wt=(\d+)', line)
            current = dict(draws=0, waiting=False, started=False, cover_hidden=False,
                           saved_wt=int(stamp[1]) if stamp else None)
            previews.append(current)
        if current is None:
            continue
        if '[SpectatorIntroDraw] loading_cover_hidden=1' in line:
            current['cover_hidden'] = True
        draw = re.search(r'\[TransitionDraw\] mode=(\d+) intro=(\d+) skip=(\d+) replay=(\d+) qpc=(\d+)', line)
        if draw and not current['waiting']:
            mode, intro, skip, replay, _ = map(int, draw.groups())
            if (mode, intro, skip, replay) != (1, 2, 0, 0):
                failures.append('表示用1更新後のイントロ画像が描画されていない')
            current['draws'] += 1
        if '[Spectator] INTRO WAIT ' in line:
            current['waiting'] = True
            stamp = re.search(r'wt=(\d+)', line)
            if not stamp or int(stamp[1]) != current['saved_wt']:
                failures.append('表示用更新の進行位置を復元していない')
            if current['draws'] != 1:
                failures.append('待機前の表示用更新が1回ではない')
        if '[Spectator] FRAME ' in line and not current['started']:
            failures.append('Start前に確定入力の再生位置が進んだ')
        if '[Spectator] START ' in line:
            if not current['waiting']:
                failures.append('最初の画像を描く前に開始した')
            current['started'] = True
            current = None
    return dict(previews=previews, failures=failures,
                passed=bool(previews and any(p['cover_hidden'] for p in previews) and
                            all(p['waiting'] and p['started'] for p in previews) and not failures))


def analyze_round_starts(text):
    starts, failures = [], []
    current = None
    for line in text.splitlines():
        begin = re.search(r'\[RoundStart\] BEGIN nextRound=(\d+) WT=(\d+) draw=(\d+) qpc=(\d+)', line)
        end = re.search(r'\[RoundStart\] END epoch=(\d+) elapsedUs=(\d+) draw=(\d+) qpc=(\d+)', line)
        if begin:
            if current is not None:
                failures.append('開始待機が完了せず次のラウンドへ進んだ')
            next_round, world, draw, qpc = map(int, begin.groups())
            current = dict(next_round=bool(next_round), world=world, draw=draw, qpc=qpc)
        if end:
            epoch, elapsed, draw, qpc = map(int, end.groups())
            if current is None:
                failures.append('開始待機の入口がない')
                continue
            if not current['draw'] or not draw:
                failures.append('開始待機中が描画OFF')
            if qpc <= current['qpc'] or abs(qpc - current['qpc'] - elapsed) > 1000:
                failures.append('開始待機の計測が不整合')
            starts.append(dict(epoch=epoch, next_round=current['next_round'], elapsed_us=elapsed))
            current = None
    if current is not None:
        failures.append('最後の開始待機が未完了')
    return dict(starts=starts, failures=failures, passed=bool(
        starts and any(r['next_round'] for r in starts) and
        any(not r['next_round'] for r in starts) and not failures))


def free_match_port():
    # UDPで使えてもTCPはOSの予約範囲で拒否されることがある。観戦側も検査する。
    for _ in range(100):
        port = free_port()
        if port == 65535:
            continue
        try:
            with ExitStack() as stack:
                for value in (port, port + 1):
                    for kind in (socket.SOCK_DGRAM, socket.SOCK_STREAM):
                        sock = stack.enter_context(socket.socket(socket.AF_INET, kind))
                        sock.bind(('0.0.0.0', value))
                return port
        except OSError:
            continue
    raise RuntimeError('対戦UDP・観戦TCPの空きポートを確保できません')


def analyze(folder, scenario):
    sides = []
    for side in (1, 2):
        text = (folder / f'game_{side}.log').read_text(encoding='utf-8')
        rows = lambda kind: [tuple(map(int, r)) for r in re.findall(SELECTION.format(kind), text)]
        commits, loaded = rows('COMMIT'), rows('LOADED')
        random = list(map(int, re.findall(r'\[Select\] RANDOM resolved=(\d+)', text)))
        draws = [tuple(map(int, r)) for r in re.findall(
            r'\[Select\] RANDOM resolved=(\d+) candidates=(\d+) excluded=(\d+)', text)]
        retries = [tuple(map(int, r)) for r in re.findall(
            r'\[RetryMenu\] RESOLVED epoch=(\d+) frame=(\d+) local=(-?\d+) peer=(-?\d+) target=(\d+) ack=(\d+)/(\d+)', text)]
        fast = list(map(int, re.findall(r'\[StageRematch\] FAST OFF phase=4 elapsedUs=(\d+)', text)))
        failures = [line for line in text.splitlines() if 'FAILED' in line]
        ok = bool(commits and loaded and retries) and not failures and not validate_load_sequence(text)
        ok &= all(ack == peer + 1 and (own == -1 or peer_ack == own + 1)
                  for _, _, own, peer, _, ack, peer_ack in retries)
        if scenario == 'random':
            ok &= len(commits) >= 2 and len(fast) >= 1
            ok &= all(c[:6] == commits[0][:6] and c[-1] in VALID_RANDOM for c in commits)
            ok &= all(a[-1] != b[-1] for a, b in zip(commits, commits[1:]))
            ok &= all(target == 0 and own == peer == 0 for _, _, own, peer, target, _, _ in retries)
            ok &= random == [c[-1] for c in commits] if side == 1 else not random
            if side == 1:
                ok &= draws == [(c[-1], 46 if i else 47, commits[i-1][-1] if i else 0)
                                for i, c in enumerate(commits)]
        elif scenario == 'fixed':
            ok &= len(commits) == 1 and len(loaded) >= 2 and not random and not fast
            ok &= all(c == commits[0] and c[-1] == 59 for c in loaded)
            ok &= all(target == 0 for _, _, _, _, target, _, _ in retries)
        else:
            ok &= len(commits) >= 2 and not fast
            ok &= all(target == 1 for _, _, _, _, target, _, _ in retries)
        sides.append(dict(commits=commits, loaded=loaded, random=random, draws=draws, retries=retries,
                          fast_elapsed_us=fast, failures=failures, passed=bool(ok)))
    same = sides[0]['commits'] == sides[1]['commits'] and sides[0]['loaded'] == sides[1]['loaded']
    return dict(scenario=scenario, sides=sides, passed=bool(same and all(s['passed'] for s in sides)))


def evaluate_behavior(out, config):
    scenario = config['scenario']
    spectator, full_intro = config.get('spectator'), config.get('full_intro')
    result = dict(behavior=analyze(out, scenario))
    checks = [result['behavior']['passed']]
    if config.get('round_frames'):
        injections = []
        for side in ((1, 2, 3) if spectator else (1, 2)):
            text = (out / f'game_{side}.log').read_text(encoding='utf-8')
            frames = list(map(int, re.findall(r'\[RetryTest\] SHORTEN frame=(\d+) draw=0', text)))
            epochs = {f // 65536 for f in frames}
            limit = config.get('round_max_epoch', 0)
            passed = {2, 3} <= epochs and all(f % 65536 >= config['round_frames'] for f in frames)
            passed &= not limit or all(e <= limit for e in epochs)
            injections.append(dict(frames=frames, passed=bool(passed)))
        result['injections'] = injections
        checks.extend(side['passed'] for side in injections)
    result['assembly'] = [analyze_assembly_text(
        (out / f'game_{side}.log').read_text(encoding='utf-8'),
        scenario == 'random' and not config.get('baseline'))
        for side in ((1, 2, 3) if spectator else (1, 2))]
    checks.extend(side['passed'] for side in result['assembly'])
    if config.get('loading_input'):
        result['loading'] = analyze_loading(out, config['loading_input'])
        checks.append(result['loading']['passed'])
    if full_intro:
        if scenario == 'random':
            result['direct_rematch'] = [analyze_direct_rematch(
                (out / f'game_{side}.log').read_text(encoding='utf-8'))
                for side in ((1, 2, 3) if spectator else (1, 2))]
            checks.extend(side['passed'] for side in result['direct_rematch'])
        result['intro'] = [analyze_intro_text((out / f'game_{side}.log').read_text(encoding='utf-8'))
                           for side in ((1, 2, 3) if spectator else (1, 2))]
        checks.extend(side['passed'] for side in result['intro'])
        result['round_starts'] = [analyze_round_starts((out / f'game_{side}.log').read_text(encoding='utf-8'))
                                  for side in (1, 2)]
        checks.extend(side['passed'] for side in result['round_starts'])
        if spectator:
            text = (out / 'game_3.log').read_text(encoding='utf-8')
            result['spectator_drawing'] = analyze_spectator_drawing(text, scenario == 'random')
            result['spectator_intro_wait'] = analyze_intro_wait(text)
            checks.extend(result[key]['passed'] for key in ('spectator_drawing', 'spectator_intro_wait'))
    result['behavior_passed'] = all(checks)
    return result


def analyze_native_loops(logs, comparison):
    sides = []
    for side, text in enumerate(logs, 1):
        baseline = comparison and side == 1
        expected = '[NativeLoops] disabled=1' if baseline else '[NativeLoops] enabled=1 scene=1'
        sides.append(dict(side=side, baseline=baseline,
                          passed=expected in text and '[NativeLoops] signature mismatch' not in text))
    return dict(sides=sides, passed=bool(sides) and all(s['passed'] for s in sides))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('scenario', choices=('random', 'fixed', 'character'))
    parser.add_argument('--seconds', type=int, default=110)
    parser.add_argument('--fixed-duration', action='store_true', help='条件達成後も指定秒まで継続する比較・耐久用')
    parser.add_argument('--round-frames', type=int, default=120, help='HP/タイマー注入の世代内F（従来値600）')
    parser.add_argument('--test-root', type=Path, default=ROOT / 'test/runtime', help='独立したMBAACC_1〜3の親フォルダー')
    parser.add_argument('--network', default='15,25,5', help='片道遅延min,maxミリ秒,損失率（既定15,25,5）')
    parser.add_argument('--spectator', action='store_true')
    parser.add_argument('--spin-prototype', action='store_true', help='入力公開・採取の共有スピン試作を有効化する')
    parser.add_argument('--legacy-present', action='store_true', help='モニター同期を無効にし、元の単独Present待機を検証する')
    parser.add_argument('--input-runahead', choices=['1', '2', '12'], help='指定した対戦端だけで1F先行表示を検証')
    parser.add_argument('--full-intro', action='store_true', help='登場演出を自然終了させ、描画・周期を記録する')
    parser.add_argument('--monitor-timing', action='store_true', help='実Presentと60Hz更新を別々に採取する')
    parser.add_argument('--monitor-hz', type=int, choices=range(20, 1001), metavar='20..1000',
                        help='検証専用の表示要求Hz。実モニター設定は変更しない')
    parser.add_argument('--baseline', action='store_true', help='再抽選のアセンブリ待機短縮だけを無効にして比較する')
    parser.add_argument('--native-loop-comparison', action='store_true', help='ホストだけ新しいメインループ最適化を無効にし、相手・観戦との同期を比較する')
    parser.add_argument('--native-loop-trace', action='store_true', help='メインループ最適化の診断用通過回数を採取する（速度比較には使わない）')
    parser.add_argument('--loading-input', choices=('none', 'host', 'client', 'both'),
                        help='ロード画面の押下端末を指定し、先着・イントロ待機を検証する（spectator/full-intro必須）')
    args = parser.parse_args()
    if not 1 <= args.round_frames <= 60000 or args.seconds < 1:
        parser.error('--round-framesは1〜60000、--secondsは1以上')
    if not re.fullmatch(r'\d+,\d+,\d+', args.network):
        parser.error('--networkは遅延min,max,損失率の整数3個で指定する')
    low, high, loss = map(int, args.network.split(','))
    if low > high or high > 1000 or loss > 100:
        parser.error('--networkは0<=min<=max<=1000、損失率0〜100で指定する')
    if args.loading_input and not (args.spectator and args.full_intro):
        parser.error('--loading-inputには--spectator --full-introが必要')
    out = ROOT / 'test/logs' / time.strftime(f'stage_rematch_{args.scenario}_%Y%m%d_%H%M%S')
    out.mkdir()
    runtime = args.test_root.resolve()
    before = protected_hashes(runtime)
    (out / 'protected_before.json').write_text(json.dumps(before, indent=2), encoding='utf-8')
    service = Service()
    threading.Thread(target=service.serve_forever, daemon=True).start()
    env = clean_environment()
    if args.spin_prototype:
        env.update(CCCASTER_SPIN_PUBLICATION='1', CCCASTER_SPIN_CAPTURE='1',
                   CCCASTER_PACE_TRACE='1')
    if args.legacy_present:
        env.update(CCCASTER_DISABLE_MONITOR_PRESENT='1', CCCASTER_PACE_TRACE='1')
    if args.input_runahead: env['CCCASTER_TEST_INPUT_RUNAHEAD_SIDES'] = args.input_runahead
    env.update(CCCASTER_NTFY_SERVER=f'http://127.0.0.1:{service.server_port}',
               CCCASTER_TEST_RETRY_QUICK='1', CCCASTER_TEST_NATIVE_RETRY='1',
               CCCASTER_TEST_REMATCH='2' if args.scenario == 'character' else '0')
    env['CCCASTER_TEST_ROUND_END_FRAME'] = str(args.round_frames)
    if args.native_loop_comparison:
        env['CCCASTER_TEST_BASELINE_HOST_NATIVE_LOOPS'] = '1'
    if args.native_loop_trace:
        env['CCCASTER_NATIVE_LOOP_TRACE'] = '1'
    if args.monitor_timing:
        env.update(CCCASTER_MONITOR_PRESENT_TRACE='1', CCCASTER_FRAME_TIMING_TRACE='1',
                   CCCASTER_UPDATE_CADENCE='1')
    if args.monitor_hz:
        env['CCCASTER_TEST_MONITOR_HZ'] = str(args.monitor_hz)
    # 現行の初回選択=世代1、初回対戦の2ラウンド=世代2/3。
    # 最初の再戦境界だけを短縮し、以後は通常進行で同期標本と終了の余裕を取る。
    if not args.fixed_duration:
        env['CCCASTER_TEST_ROUND_END_MAX_EPOCH'] = '3'
    env['CCCASTER_TEST_FIXED_STAGE' if args.scenario == 'fixed' else 'CCCASTER_TEST_RANDOM_STAGE'] = (
        '59' if args.scenario == 'fixed' else '1')
    if args.full_intro:
        env.update(CCCASTER_TEST_FULL_INTRO='1', CCCASTER_PACE_TRACE='1',
                   CCCASTER_CLOCK_FOLLOW_TRACE='1', CCCASTER_TRANSITION_DRAW_TRACE='1')
    if args.baseline:
        env['CCCASTER_TEST_REMATCH_BASELINE'] = '1'
    if args.loading_input:
        env['CCCASTER_TEST_LOADING_INPUT'] = args.loading_input
    cmd = [shutil.which('pwsh'), '-NoProfile', '-File', str(ROOT / 'src/src/harness/run_bounded_real_pair.ps1'),
           '-Seconds', str(args.seconds), '-Port', str(free_match_port()), '-Network', args.network,
           '-UseConnectionCode', '-CloseSide', '1', '-OutputDirectory', str(out), '-TestRoot', str(runtime)]
    if args.spectator:
        cmd.append('-StandbySpectator')
    config = dict(scenario=args.scenario, spectator=args.spectator, full_intro=args.full_intro, input_runahead=args.input_runahead,
                  native_input_writes=False,
                  baseline=args.baseline, loading_input=args.loading_input,
                  round_frames=args.round_frames, round_max_epoch=0 if args.fixed_duration else 3)
    (out / 'checkpoint_config.json').write_text(json.dumps(config), encoding='utf-8')
    if not args.fixed_duration:
        cmd += ['-CheckpointConfig', str(out / 'checkpoint_config.json'), '-Python', sys.executable]
    result = dict(passed=False, scenario=args.scenario, command=cmd)
    result['environment'] = {k: v for k, v in env.items() if k.startswith('CCCASTER_')}
    started = time.monotonic()
    print(f'Logs: {out}', flush=True)
    try:
        with (out / 'runner.log').open('w', encoding='utf-8') as log:
            run = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
        result['exit_code'] = run.returncode
        result.update(evaluate(out, config))
        if args.native_loop_comparison or args.native_loop_trace:
            result['native_loops'] = analyze_native_loops(
                [(out / f'game_{side}.log').read_text(encoding='utf-8', errors='replace')
                 for side in range(1, 4 if args.spectator else 3)], args.native_loop_comparison)
            result['passed'] &= result['native_loops']['passed']
        result['passed'] &= run.returncode == 0
    except Exception as exc:
        result.update(passed=False, error=str(exc))
    finally:
        service.running = False
        with service.cv:
            service.cv.notify_all()
        service.shutdown()
        service.server_close()
        result['protected_unchanged'] = before == protected_hashes(runtime)
        result['protected_count'] = len(before)
        result['passed'] &= result['protected_unchanged']
        result['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(passed=result['passed'], seconds=result['elapsed_seconds'], logs=str(out)),
                     ensure_ascii=False), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
