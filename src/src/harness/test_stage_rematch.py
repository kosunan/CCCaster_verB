import tempfile
import unittest
import subprocess
import sys
from pathlib import Path
from run_stage_rematch import analyze, analyze_intro_text, analyze_assembly_text, analyze_loading, analyze_spectator_drawing, analyze_intro_wait, analyze_round_starts, evaluate_behavior
from run_stage_rematch import analyze_direct_rematch
from run_stage_rematch import analyze_native_loops
from run_stage_rematch import analyze_extra_colors
from run_stage_rematch import analyze_loading_pace


class StageRematchAnalysis(unittest.TestCase):
    @staticmethod
    def loading_pace_log(side=1, period=1000000, real_period=None, slow_asset=False, wait=None):
        rows = []
        for load in range(2):
            if load:
                rows.append('[Spectator] RETRY target=2' if side == 3 else
                            '[StageRematch] DIRECT stage=12 mode=5')
            base = 60000000 + load * 600000000
            for frame in range(1, 9):
                # 資産ロードで100ms停止した後、次締切が現在時刻+1Fへ再基準化。
                extra = 6000000 if slow_asset and frame >= 4 else 0
                due = base + frame * period + extra
                qpc = (base + frame * (period if real_period is None else real_period) + extra) // 60
                clock = qpc * 60 if wait == 0 else due
                rows.append(f'[LoadingPace] app={2 if side == 3 else 0} role={int(side == 1)} '
                            f'frame={frame} due={due} clock={clock} qpc={qpc}' +
                            (f' wait={wait}' if wait is not None else ''))
                rows.append(f'[TransitionDraw] mode=8 intro=0 skip={load} replay=0 qpc={qpc + 1}')
            rows.append(f'[TransitionDraw] mode=1 intro=2 skip=0 replay=0 qpc={qpc + 2}')
        return '\n'.join(rows) + '\n'

    def test_loading_pace_requires_both_loads_on_both_players(self):
        for side in (1, 2):
            good = self.loading_pace_log(side)
            result = analyze_loading_pace(good, side, True)
            self.assertTrue(result['passed'], result)
            self.assertEqual([r['frames'] for r in result['loads']], [8, 8])
            self.assertAlmostEqual(result['loads'][0]['average_fps'], 60, places=2)
            for bad in ('', good[:good.index('[StageRematch]')], good.replace('frame=4 ', 'frame=5 '),
                        '\n'.join(line for line in good.splitlines() if 'frame=3 ' not in line),
                        good.replace('clock=64000000', 'clock=63999999'),
                        good.replace('due=64000000', 'due=invalid'),
                        good.replace('mode=1 ', 'mode=5 '),
                        good.replace('frame=4 ', 'frame=1 ')):
                self.assertFalse(analyze_loading_pace(bad, side, True)['passed'], bad)
            self.assertFalse(analyze_loading_pace(good, 2 if side != 2 else 1, True)['passed'])
            missing_rematch = good.replace('[StageRematch] DIRECT ', '[Other] ').replace(
                '[Spectator] RETRY target=2', '[Spectator] RETRY target=0')
            self.assertFalse(analyze_loading_pace(missing_rematch, side, True)['passed'])

    def test_loading_pace_rejects_wrong_period_and_false_real_time(self):
        for period in (500000, 999999, 2000000):
            self.assertFalse(analyze_loading_pace(self.loading_pace_log(period=period), 1, True)['passed'])
        # 締切と音声時計を60Hzに見せても、QPCで無制限更新を検出する。
        fast = self.loading_pace_log(real_period=1000)
        self.assertFalse(analyze_loading_pace(fast, 1, True)['passed'])
        for bad in (self.loading_pace_log().replace('qpc=1066666', 'qpc=1050000'),
                    self.loading_pace_log(period=7000000, real_period=1000000)):
            self.assertFalse(analyze_loading_pace(bad, 1, True)['passed'])

    def test_loading_pace_accepts_asset_stalls_and_short_catchup(self):
        slow = analyze_loading_pace(self.loading_pace_log(slow_asset=True), 1, True)
        self.assertTrue(slow['passed'], slow)
        self.assertEqual([r['deadline_rebases'] for r in slow['loads']], [1, 1])
        self.assertLess(slow['loads'][0]['average_fps'], 60)
        # 最初の復帰が20ms遅れたときは、直後の短い追いつきで過速と誤判定しない。
        good = self.loading_pace_log().replace('clock=61000000 qpc=1016666',
                                             'clock=62200000 qpc=1036666').replace(
                                             'clock=62000000 qpc=1033333',
                                             'clock=62200100 qpc=1036668')
        self.assertTrue(analyze_loading_pace(good, 1, True)['passed'])

    def test_loading_unpaced_requires_explicit_comparison_and_preserves_boundaries(self):
        for side in (1, 2):
            good = self.loading_pace_log(side, real_period=6000, wait=0)
            result = analyze_loading_pace(good, side, True, unpaced=True)
            self.assertTrue(result['passed'], result)
            self.assertGreater(result['loads'][0]['average_fps'], 60)
            self.assertFalse(analyze_loading_pace(good, side, True)['passed'])
            for bad in (good.replace(' wait=0', ''), good.replace('wait=0', 'wait=1'),
                        good.replace('wait=0', 'wait=invalid'), good.replace('frame=4 ', 'frame=5 '),
                        good.replace('mode=1 ', 'mode=5 '),
                        good.replace('due=64000000', 'due=64500000'),
                        good.replace('clock=60024000', 'clock=60000000'),
                        good.replace('qpc=1000400', 'qpc=1000100')):
                self.assertFalse(analyze_loading_pace(bad, side, True, unpaced=True)['passed'], bad)
        self.assertTrue(analyze_loading_pace(self.loading_pace_log(wait=1), 1, True)['passed'])

    def test_loading_comparison_options_reject_invalid_scenarios_before_launch(self):
        script = Path(__file__).with_name('run_stage_rematch.py')
        for args, message in ((['random', '--loading-unpaced'], '--full-intro'),
                              (['fixed', '--repeatable-stage'], 'random'),
                              (['character', '--repeatable-stage'], 'random')):
            result = subprocess.run([sys.executable, '-X', 'utf8', str(script), *args],
                                    capture_output=True, text=True, encoding='utf-8')
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn(message, result.stderr)

    def test_full_intro_includes_loading_pace_in_checkpoint_result(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            for side in (1, 2):
                (folder / f'game_{side}.log').write_text(self.loading_pace_log(side), encoding='utf-8')
            spectator_path = folder / 'game_3.log'
            spectator_log = '\n'.join(line for line in self.loading_pace_log(3).splitlines()
                                      if '[LoadingPace]' not in line)
            spectator_path.write_text(spectator_log, encoding='utf-8')
            config = dict(scenario='random', spectator=True, full_intro=True)
            result = evaluate_behavior(folder, config)
            self.assertEqual([r['side'] for r in result['loading_pace']], [1, 2])
            self.assertTrue(all(r['passed'] for r in result['loading_pace']))
            self.assertTrue(result['spectator_loading_pace']['passed'])
            (folder / 'game_2.log').write_text('', encoding='utf-8')
            result = evaluate_behavior(folder, config)
            self.assertFalse(result['loading_pace'][1]['passed'])
            self.assertFalse(result['behavior_passed'])
            (folder / 'game_2.log').write_text(self.loading_pace_log(2), encoding='utf-8')
            spectator_path.write_text(self.loading_pace_log(3), encoding='utf-8')
            result = evaluate_behavior(folder, config)
            self.assertFalse(result['spectator_loading_pace']['passed'])
            self.assertEqual(result['spectator_loading_pace']['unexpected_records'], 16)
            self.assertFalse(result['behavior_passed'])
            spectator_path.write_text(spectator_log, encoding='utf-8')
            config['loading_unpaced'] = True
            for side in (1, 2):
                (folder / f'game_{side}.log').write_text(
                    self.loading_pace_log(side, real_period=6000, wait=0), encoding='utf-8')
            comparison = evaluate_behavior(folder, config)
            self.assertTrue(all(r['passed'] and r['unpaced'] for r in comparison['loading_pace']))
            self.assertTrue(comparison['spectator_loading_pace']['passed'])

    def test_extra_color_permission_keeps_own_color_and_stops_rejected_transfer(self):
        for blocked in ('', '1', '2', '12'):
            for side in (1, 2):
                receives, peer_receives = str(side) not in blocked, str(3-side) not in blocked
                selected = 6 if side == 1 else 0
                sent = selected if peer_receives else 0
                size = 47628 if sent else 12
                policy = f'[ExtraColor] POLICY receive={int(receives)} peerReceive={int(peer_receives)} selected={selected} sent={sent}\n'
                send = f'[ExtraColor] SEND epoch=65536 serial=3 character=0 extra={sent} bytes={size} hash=abcd fast=1 qpc=1\n'
                applied = '[ExtraColor] LOAD slot=0 character=0 layout=abcd matched=1 applied=1\n' if side == 1 or receives else ''
                good = (policy + send + '[Select] COMMIT p1=0/0/0\n' + applied + '[ExtraColor] READY epoch=65536 waitUs=0 fast=1 qpc=1\n') * 2
                self.assertTrue(analyze_extra_colors(good, side, blocked)['passed'], (blocked, side))
                for bad in ('', good.replace(policy, ''), good.replace('[Select] COMMIT ', ''),
                            good.replace('READY', 'WAITING')):
                    # 受信拒否側でも遷移完了が必要。確定ログは元の同期判定器が別途必須にする。
                    if '[Select] COMMIT ' not in bad and not applied and bad:
                        continue
                    self.assertFalse(analyze_extra_colors(bad, side, blocked)['passed'], (blocked, side, bad))
                if applied:
                    self.assertFalse(analyze_extra_colors(good.replace(applied, ''), side, blocked)['passed'])
                else:
                    self.assertFalse(analyze_extra_colors(good + '[ExtraColor] LOAD slot=0 character=0 layout=abcd matched=1 applied=1\n', side, blocked)['passed'])
                if side == 1 and not peer_receives:
                    self.assertFalse(analyze_extra_colors(good.replace('bytes=12', 'bytes=47628'), side, blocked)['passed'])

    def test_native_loops_require_each_side_to_use_the_requested_code(self):
        enabled = '[NativeLoops] enabled=1 scene=1 sse2=1'
        disabled = '[NativeLoops] disabled=1'
        self.assertTrue(analyze_native_loops([disabled, enabled, enabled], True)['passed'])
        self.assertTrue(analyze_native_loops([enabled, enabled, enabled], False)['passed'])
        for logs in ([], [disabled, enabled, disabled], [disabled, 'signature mismatch'],
                     [enabled, enabled], [disabled, enabled.replace('scene=1', 'scene=0')]):
            self.assertFalse(analyze_native_loops(logs, True)['passed'])

    def test_direct_rematch_requires_real_load_and_no_character_select(self):
        initial = '[StageAsset] selected=55 loaded=55 expanded=1 file=stage55.dat\n'
        direct = '[StageRematch] DIRECT stage=12 mode=5\n'
        load = '[TransitionDraw] mode=8 intro=0 skip=1 replay=0 qpc=100\n'
        intro = '[TransitionDraw] mode=1 intro=2 skip=0 replay=0 qpc=200\n'
        asset = '[StageAsset] selected=12 loaded=12 expanded=1 file=stage12.dat\n'
        good = initial + direct + load + intro + asset
        self.assertTrue(analyze_direct_rematch(good)['passed'])
        for bad in ('', good.replace(load, ''), good.replace(intro, ''), good.replace(asset, ''),
                    good.replace(load, load + load.replace('mode=8', 'mode=20')),
                    good.replace(asset, initial),
                    good.replace('loaded=12', 'loaded=55'), good.replace('expanded=1', 'expanded=0'),
                    good.replace('file=stage12.dat', 'file='), good.replace(direct, direct * 2)):
            self.assertFalse(analyze_direct_rematch(bad)['passed'])
    def test_injection_must_reach_both_rounds_and_stop_after_first_match(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            config = dict(scenario='fixed', spectator=True, round_frames=120, round_max_epoch=3)
            good = '[RetryTest] SHORTEN frame=131192 draw=0\n[RetryTest] SHORTEN frame=196728 draw=0\n'
            for side in (1, 2, 3):
                (folder / f'game_{side}.log').write_text(good, encoding='utf-8')
            self.assertTrue(all(s['passed'] for s in evaluate_behavior(folder, config)['injections']))
            for bad in ('', good.replace('196728', '196727'),
                        good + '[RetryTest] SHORTEN frame=393336 draw=0\n'):
                (folder / 'game_3.log').write_text(bad, encoding='utf-8')
                self.assertFalse(evaluate_behavior(folder, config)['injections'][2]['passed'])

    def test_round_start_wait_is_visible_and_completes(self):
        first = '[RoundStart] BEGIN nextRound=0 WT=100 draw=1 qpc=100\n'
        next_round = '[RoundStart] BEGIN nextRound=1 WT=1000 draw=1 qpc=2000000\n'
        first_end = '[RoundStart] END epoch=131072 elapsedUs=1250000 draw=1 qpc=1250100\n'
        second_end = '[RoundStart] END epoch=196608 elapsedUs=1250000 draw=1 qpc=3250000\n'
        good = first + first_end + next_round + second_end
        self.assertTrue(analyze_round_starts(good)['passed'])
        for bad in ('', first + first_end, good.replace('draw=1', 'draw=0', 1),
                    first + next_round + second_end, good + next_round,
                    good.replace('qpc=3250000', 'qpc=4250000')):
            self.assertFalse(analyze_round_starts(bad)['passed'])

    def test_loading_requires_shared_skip_and_spectator_wait(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            for side, when in ((1, 2000000), (2, 2050000), (3, 800000)):
                if side == 3:
                    log = '[Spectator] INTRO ENTER\n[Spectator] START\n'
                else:
                    log = '[LoadingInput] BEGIN\n'
                    if side == 1: log += '[LoadingInput] PRESS role=1\n'
                    log += f'[LoadingSkip] REQUEST role={2-side} epoch=131072 source={"local" if side == 1 else "peer"} qpc=1900000\n'
                    log += '[LoadingInput] END\n'
                log += f'[TransitionDraw] mode=1 intro=2 skip=0 replay=0 qpc={when}\n'
                log += '[TransitionDraw] mode=1 intro=2 skip=0 replay=0 qpc=9500000\n'
                (folder / f'game_{side}.log').write_text(log, encoding='utf-8')
            self.assertTrue(analyze_loading(folder, 'host')['passed'])
            self.assertFalse(analyze_loading(folder, 'client')['passed'])
            self.assertFalse(analyze_loading(folder, 'none')['passed'])
            client = folder / 'game_2.log'
            original = client.read_text()
            for bad in (original.replace('epoch=131072', 'epoch=327680'),
                        original.replace('source=peer', 'source=local'),
                        original.replace('qpc=2050000', 'qpc=9000000')):
                client.write_text(bad, encoding='utf-8')
                self.assertFalse(analyze_loading(folder, 'host')['passed'])
            client.write_text(original, encoding='utf-8')
            host = folder / 'game_1.log'
            original_host = host.read_text()
            # 実ロードが双方で長引いても、完了後に揃って進めれば共有スキップは正常。
            host.write_text(original_host.replace('qpc=2000000', 'qpc=5000000'), encoding='utf-8')
            client.write_text(original.replace('qpc=2050000', 'qpc=5050000'), encoding='utf-8')
            self.assertTrue(analyze_loading(folder, 'host')['passed'])
            host.write_text(original_host, encoding='utf-8')
            client.write_text(original, encoding='utf-8')
            viewer = folder / 'game_3.log'
            viewer.write_text(viewer.read_text().replace('[Spectator] START',
                '[Spectator] FRAME f=1\n[Spectator] START'), encoding='utf-8')
            self.assertFalse(analyze_loading(folder, 'host')['passed'])

    def test_spectator_transition_draws_with_fast_mode(self):
        log = '[Spectator] PACE fast=1 draw=1\n' + '\n'.join(
            f'[TransitionDraw] mode={mode} intro={intro} skip=0 replay=0 qpc={tick}'
            for tick, (mode, intro) in enumerate(((20, 0), (8, 0), (1, 2), (1, 1), (1, 0))))
        self.assertTrue(analyze_spectator_drawing(log)['passed'])
        for line in log.splitlines()[1:]:
            self.assertFalse(analyze_spectator_drawing(log.replace(line, line.replace('skip=0', 'skip=1')))['passed'])
        self.assertFalse(analyze_spectator_drawing(log.replace('fast=1', 'fast=0'))['passed'])

    def test_spectator_hides_only_random_once_reselection(self):
        row = lambda mode, skip: f'[TransitionDraw] mode={mode} intro=2 skip={skip} replay=0 qpc=100\n'
        normal = '[Spectator] PACE fast=1 draw=1\n' + row(20, 0) + row(8, 0) + row(1, 0)
        hidden = '[Spectator] RETRY target=2\n[Spectator] PACE fast=1 draw=0\n' + row(8, 1)
        restored = row(1, 0)
        good = normal + hidden + restored
        self.assertTrue(analyze_spectator_drawing(good, True)['passed'])
        self.assertFalse(analyze_spectator_drawing(normal + hidden + row(20, 1) + restored, True)['passed'])
        self.assertFalse(analyze_spectator_drawing(good, False)['passed'])
        self.assertFalse(analyze_spectator_drawing(normal, True)['passed'])
        self.assertFalse(analyze_spectator_drawing(normal + hidden.replace('skip=1', 'skip=0') + restored, True)['passed'])
        self.assertFalse(analyze_spectator_drawing(normal + hidden + row(1, 1), True)['passed'])
        self.assertTrue(analyze_spectator_drawing(normal + '[Spectator] RETRY target=0\n' + row(8, 0) + restored)['passed'])
        self.assertTrue(analyze_spectator_drawing(normal + '[Spectator] RETRY target=1\n' + normal)['passed'])

    def test_intro_image_precedes_wait_and_stream_consumption(self):
        preview = '[Spectator] INTRO PREVIEW wt=50 qpc=100\n'
        draw = '[TransitionDraw] mode=1 intro=2 skip=0 replay=0 qpc=101\n'
        cover = '[SpectatorIntroDraw] loading_cover_hidden=1\n'
        wait = '[Spectator] INTRO WAIT wt=50 qpc=102\n'
        start = '[Spectator] START frame=131073\n'
        good = preview + cover + draw + wait + start
        self.assertTrue(analyze_intro_wait(good * 2)['passed'])
        for bad in ('', preview + wait + start, preview + draw * 2 + wait + start,
                    preview + draw + start, preview + draw + wait,
                    good.replace(cover, ''), good.replace('WAIT wt=50', 'WAIT wt=51'),
                    good.replace('skip=0', 'skip=1'),
                    good.replace(start, '[Spectator] FRAME f=131073\n' + start)):
            self.assertFalse(analyze_intro_wait(bad)['passed'])

    def test_assembly_scope_and_restore_before_intro(self):
        on = '[StageRematchAsm] enabled=1 patches=1\n'
        off = '[StageRematchAsm] enabled=0 patches=1\n'
        intro = '[TransitionDraw] mode=1 intro=1 skip=0 replay=0 qpc=500\n'
        good = on + off + intro
        self.assertTrue(analyze_assembly_text(good * 2, True)['passed'])
        self.assertTrue(analyze_assembly_text(intro, False)['passed'])
        for bad in (on + intro + off, on, off, on + on + off, ''):
            self.assertFalse(analyze_assembly_text(bad, True)['passed'])
        self.assertFalse(analyze_assembly_text(good, False)['passed'])
        self.assertFalse(analyze_assembly_text(good.replace('patches=1', 'patches=7'), True)['passed'])

    def check_logs(self, scenario, second_stage=12, second_color=35, ack=1, excluded=55, candidates=46):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            for side in (1, 2):
                fixed = scenario == 'fixed'
                first = 59 if fixed else 55
                log = '' if side == 2 or fixed else '[Select] RANDOM resolved=55 candidates=47 excluded=0\n'
                log += f'[Select] COMMIT p1=51/2/35 p2=33/1/15 stage={first}\n'
                log += f'[Select] LOADED p1=51/2/35 p2=33/1/15 stage={first}\n'
                log += f'[RetryMenu] RESOLVED epoch=262144 frame=262500 local=0 peer=0 target=0 ack={ack}/1\n'
                if not fixed:
                    if side == 1:
                        log += f'[Select] RANDOM resolved={second_stage} candidates={candidates} excluded={excluded}\n'
                    log += f'[Select] COMMIT p1=51/2/{second_color} p2=33/1/15 stage={second_stage}\n'
                    log += '[StageRematch] FAST OFF phase=4 elapsedUs=150000\n'
                log += f'[Select] LOADED p1=51/2/{second_color} p2=33/1/15 stage={first if fixed else second_stage}\n'
                (folder / f'game_{side}.log').write_text(log, encoding='utf-8')
            return analyze(folder, scenario)['passed']

    def test_random_and_fixed(self):
        self.assertTrue(self.check_logs('random'))
        self.assertFalse(self.check_logs('random', second_stage=55))  # 直前と同じステージは不合格。
        self.assertTrue(self.check_logs('fixed'))  # ランダム除外59の手動指定を維持。

    def test_previous_stage_must_be_removed_from_the_pool(self):
        # 偶然違う番号が出ただけでは成功にしない。
        self.assertFalse(self.check_logs('random', excluded=0))
        self.assertFalse(self.check_logs('random', excluded=12))
        self.assertFalse(self.check_logs('random', candidates=47))

    def test_excluded_or_unavailable_stage_is_failure(self):
        for stage in (0, 11, 32, 43, 44, 51, 54, 57, 58, 59):
            self.assertFalse(self.check_logs('random', second_stage=stage))

    def test_character_settings_and_ack_are_required(self):
        self.assertFalse(self.check_logs('random', second_color=0))
        self.assertFalse(self.check_logs('fixed', second_color=0))
        self.assertFalse(self.check_logs('random', ack=0))

    def test_intro_first_picture_must_be_visible(self):
        def log(first_skip=0, frames=130):
            rows = []
            for battle in range(2):
                rows.append('[TransitionDraw] mode=8 intro=0 skip=1 replay=0 qpc=0')
                for frame in range(frames):
                    rows.append(f'[TransitionDraw] mode=1 intro=2 skip={first_skip if frame == 0 else 0} replay=0 qpc={frame * 16667}')
                rows.append(f'[TransitionDraw] mode=1 intro=0 skip=0 replay=0 qpc={frames * 16667}')
            return '\n'.join(rows)
        self.assertTrue(analyze_intro_text(log())['passed'])
        self.assertFalse(analyze_intro_text(log(first_skip=1))['passed'])
        self.assertFalse(analyze_intro_text(log(frames=5))['passed'])
        self.assertFalse(analyze_intro_text('')['passed'])
        hidden = '[TransitionDraw] mode=1 intro=2 skip=1 replay=0 qpc=1500000'
        marker = '[TransitionDraw] mode=1 intro=2 skip=0 replay=0 qpc=1516697'
        # 通常前進中の省略は失敗。再計算最終画像の省略は正常。
        self.assertFalse(analyze_intro_text(log().replace(marker, hidden))['passed'])
        replay = '[Rollback] BEGIN frame=1 target=2\n' + hidden + '\n[Rollback] END target=2'
        self.assertTrue(analyze_intro_text(log().replace(marker, replay))['passed'])


if __name__ == '__main__':
    unittest.main()
