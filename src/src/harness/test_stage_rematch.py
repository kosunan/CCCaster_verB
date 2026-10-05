import tempfile
import unittest
from pathlib import Path
from run_stage_rematch import analyze, analyze_intro_text, analyze_assembly_text, analyze_loading, analyze_spectator_drawing, analyze_intro_wait, analyze_round_starts, evaluate_behavior
from run_stage_rematch import analyze_direct_rematch
from run_stage_rematch import analyze_native_loops


class StageRematchAnalysis(unittest.TestCase):
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

    def check_logs(self, scenario, second_stage=12, second_color=35, ack=1):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            for side in (1, 2):
                fixed = scenario == 'fixed'
                first = 59 if fixed else 55
                log = '' if side == 2 or fixed else '[Select] RANDOM resolved=55 candidates=47\n'
                log += f'[Select] COMMIT p1=51/2/35 p2=33/1/15 stage={first}\n'
                log += f'[Select] LOADED p1=51/2/35 p2=33/1/15 stage={first}\n'
                log += f'[RetryMenu] RESOLVED epoch=262144 frame=262500 local=0 peer=0 target=0 ack={ack}/1\n'
                if not fixed:
                    if side == 1:
                        log += f'[Select] RANDOM resolved={second_stage} candidates=47\n'
                    log += f'[Select] COMMIT p1=51/2/{second_color} p2=33/1/15 stage={second_stage}\n'
                    log += '[StageRematch] FAST OFF phase=4 elapsedUs=150000\n'
                log += f'[Select] LOADED p1=51/2/{second_color} p2=33/1/15 stage={first if fixed else second_stage}\n'
                (folder / f'game_{side}.log').write_text(log, encoding='utf-8')
            return analyze(folder, scenario)['passed']

    def test_random_and_fixed(self):
        self.assertTrue(self.check_logs('random'))
        self.assertTrue(self.check_logs('random', second_stage=55))  # 同じ候補の再当選も正常。
        self.assertTrue(self.check_logs('fixed'))  # ランダム除外59の手動指定を維持。

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
