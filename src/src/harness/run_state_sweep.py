"""解析済みMBAAで、入力なしの状態定義列挙・保存復元往復を実行する。

各stateの入口を作る合成試験であり、実戦で到達可能な全状態・全組合せの証明ではない。
物理操作は不要。--hold-fixturesでは継続用の保持信号をGameMemから与える。
起動・ロード・登場演出は既存経路の決定入力でスキップする。
"""
import argparse
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import time
from threading import Lock

ROOT = Path(__file__).resolve().parents[3]
EXE_SHA256 = '6d1415ca9573100e86a779ac2f81e9bedd322664e3daeae0229a67d13720310a'
CHARACTERS = (22, 7, 51, 15, 28, 8, 2, 0, 30, 11, 9, 31, 4, 3, 1, 19, 12,
              13, 14, 29, 17, 18, 33, 23, 10, 25, 35, 5, 20, 6, 34)
EXTRA_COMBINATIONS = ((30, 9), (51, 9), (16, 9), (32, 0), (53, 8),
                      (58, 9), (59, 9), (72, 9), (73, 9), (85, 9))
ALL_CHARACTERS = CHARACTERS + (16, 32, 53, 58, 59, 72, 73, 85)
ALL_COMBINATIONS = tuple((c, m) for c in CHARACTERS for m in range(3)) + EXTRA_COMBINATIONS
BINARIES = ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll')


def binary_build_id(data, dll=False):
    # BootDiagnostics::DescriptorとGameLauncherの起動識別表示。ロードせずファイルを検査する。
    pattern = (rb'SBCC\x01\x00\x00\x00\x50\x00\x00\x00([0-9a-f]{64})\x00' if dll else
               rb'\[BOOT_READY\] build=([0-9a-f]{64}) stage=running')
    values = set(re.findall(pattern, data))
    if len(values) != 1:
        raise ValueError('バイナリのビルド識別子を一意に確認できません')
    return values.pop().decode('ascii')


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def protected(paths):
    return {str(p): sha(p) for root in paths for p in root.rglob('*')
            if p.is_file() and (p.suffix.lower() in ('.ini', '.rep') or p.name.lower() == 'mbaa.exe')}


def read_events(path):
    if not path.exists():
        return []
    events = []
    for line in path.read_text(encoding='utf-8').splitlines():
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            # 強制終了時の末尾切断。endの存在と件数の検査により合格にしない。
            events.append({'event': 'truncated'})
    return events


def summarize(events, catalog=False):
    if any(e.get('event') == 'start' and e.get('schema') == 2 for e in events):
        return summarize_frames(events, catalog)
    starts = [e for e in events if e.get('event') == 'start']
    ends = [e for e in events if e.get('event') == 'end']
    definitions = [e for e in events if e.get('event') == 'definition']
    selected = [e for e in definitions if e.get('selected')]
    begun = [e for e in events if e.get('event') == 'begin_case']
    results = [e for e in events if e.get('event') == 'result']
    watches = [e for e in events if e.get('event') == 'watch']
    catalogs = [e for e in events if e.get('event') == 'catalog_end']
    end = ends[-1] if ends else {}
    enumerated = len(catalogs) == 1 and catalogs[0].get('definitions') == len(definitions) and (
        catalogs[0].get('selected') == len(selected)) and bool(selected)
    complete = len(starts) == len(ends) == 1 and bool(selected) and not any(
        e.get('event') == 'truncated' for e in events)
    complete &= enumerated
    complete &= end.get('defined') == len(selected)
    identities = [(e.get('slot'), e.get('pattern'), e.get('state')) for e in selected]
    complete &= len(set(identities)) == len(identities)
    if catalog:
        complete &= end.get('status') == 'catalog' and not results
    else:
        complete &= end.get('status') == 'complete' and end.get('checked') == len(selected)
        complete &= len(begun) == len(results) == len(selected)
        complete &= [e.get('case') for e in results] == list(range(len(selected)))
        complete &= [(e.get('slot'), e.get('pattern'), e.get('state')) for e in begun] == identities
        complete &= all(e.get('equal') is True and e.get('different_bytes') == 0 for e in results)
        complete &= end.get('mismatches') == 0
    return dict(passed=bool(complete and (catalog or not watches)),
                enumeration_complete=bool(enumerated),
                comparison_complete=bool(complete and not catalog),
                all_game_states_covered=False, scope='synthetic_state_entry_one_update',
                definitions=len(definitions), selected=len(selected), checked=len(results),
                matched=sum(e.get('equal') is True for e in results),
                watch_changes=len(watches), watch_status='needs_review' if watches else 'no_changes_observed',
                last_case=begun[-1] if begun else None, termination=end or {'reason': 'no_end_record'})


def summarize_frames(events, catalog=False):
    groups = defaultdict(list)
    for event in events:
        groups[event.get('event')].append(event)
    starts, ends = groups['start'], groups['end']
    definitions = groups['definition']
    selected = [e for e in definitions if e.get('selected')]
    results, begins, finished = groups['result'], groups['begin_case'], groups['end_case']
    end = ends[-1] if ends else {}
    identities = [(e.get('slot'), e.get('pattern'), e.get('state')) for e in selected]
    valid_duration = all(isinstance(e.get('duration'), int) and 0 <= e['duration'] <= 65535 for e in selected)
    nominal = sum(max(1, e['duration']) for e in selected) if valid_duration else 0
    catalogs = groups['catalog_end']
    enumerated = bool(selected) and valid_duration and len(catalogs) == 1 and (
        catalogs[0].get('definitions') == len(definitions) and catalogs[0].get('selected') == len(selected)
        and catalogs[0].get('nominal_frame_points') == nominal and len(set(identities)) == len(identities))
    valid = enumerated and len(starts) == len(ends) == 1 and not groups['truncated']
    valid &= end.get('defined') == len(selected) and end.get('nominal_frame_points') == nominal
    valid &= end.get('watch_changes') == len(groups['watch'])
    entry_only = len(starts) == 1 and starts[0].get('entry_only') is True
    frames_by_case = defaultdict(list)
    covered_by_case = defaultdict(set)
    world_covered_by_case = defaultdict(set)
    for e in results:
        frames_by_case[e.get('case')].append(e)
        case, age = e.get('case'), e.get('state_age')
        if (isinstance(case, int) and 0 <= case < len(selected) and valid_duration
                and isinstance(age, int) and 0 <= age < max(1, selected[case]['duration'])
                and e.get('equal') is True and e.get('different_bytes') == 0):
            covered_by_case[case].add(age)
            if e.get('frame_source', 'native_world_update') == 'native_world_update':
                world_covered_by_case[case].add(age)
    covered = sum(len(ages) for ages in covered_by_case.values())
    world_covered = sum(len(ages) for ages in world_covered_by_case.values())
    if catalog:
        complete = False
        passed = valid and end.get('status') == 'catalog' and not results and not begins
    else:
        valid &= end.get('status') == 'complete' and end.get('checked') == len(results)
        valid &= end.get('ended_cases') == len(selected) and len(begins) == len(finished) == len(selected)
        valid &= [e.get('case') for e in begins] == list(range(len(selected)))
        valid &= [e.get('case') for e in finished] == list(range(len(selected)))
        valid &= [(e.get('slot'), e.get('pattern'), e.get('state')) for e in begins] == identities
        def frame_identity(e):
            return (e.get('case'), e.get('frame'), e.get('state_age'), e.get('attempt'),
                    e.get('frame_source', 'native_world_update'))
        valid &= [frame_identity(e) for e in groups['frame_begin']] == [frame_identity(e) for e in results]
        valid &= all(e.get('frame_source', 'native_world_update') in (
            'native_world_update', 'native_actor_setup') for e in results)
        valid &= set(frames_by_case) == set(range(len(selected)))
        valid &= [e.get('case') for e in results] == [
            case for case in range(len(selected)) for _ in frames_by_case[case]]
        for e in finished:
            case = e.get('case')
            rows = frames_by_case[case]
            valid &= bool(rows) and [r.get('frame') for r in rows] == list(range(len(rows)))
            valid &= e.get('frames') == len(rows) and e.get('covered_frame_points') == len(covered_by_case[case])
            if isinstance(case, int) and 0 <= case < len(selected) and valid_duration:
                valid &= e.get('nominal_frame_points') == max(1, selected[case]['duration'])
            else:
                valid = False
        valid &= end.get('covered_frame_points') == covered
        complete = bool(valid and end.get('mismatches') == 0 and all(
            e.get('equal') is True and e.get('different_bytes') == 0 for e in results))
        passed = complete and not groups['watch'] and (entry_only or covered == nominal)
    return dict(passed=bool(passed), enumeration_complete=bool(enumerated),
                comparison_complete=complete, all_game_states_covered=False,
                all_selected_nominal_frames_covered=bool(complete and covered == nominal),
                scope='synthetic_state_frame_steps', entry_only=entry_only,
                definitions=len(definitions), selected=len(selected), started_states=len(begins),
                selected_motions=len({(e.get('slot'), e.get('pattern')) for e in selected}),
                ended_states=len(finished), checked=len(results), checked_frames=len(results),
                nominal_frame_points=nominal, covered_frame_points=covered,
                world_update_frame_points=world_covered,
                actor_setup_only_frame_points=covered-world_covered,
                actor_setup_comparisons=sum(e.get('frame_source') == 'native_actor_setup' for e in results),
                all_frames_reached_by_world_updates=bool(complete and world_covered == nominal),
                missing_frame_points=max(0, nominal-covered),
                matched=sum(e.get('equal') is True for e in results),
                watch_changes=len(groups['watch']),
                watch_status='needs_review' if groups['watch'] else 'no_changes_observed',
                state_end_reasons=dict((reason, sum(e.get('reason') == reason for e in finished))
                                      for reason in sorted({e.get('reason', '') for e in finished})),
                last_case=begins[-1] if begins else None,
                last_frame=groups['frame_begin'][-1] if groups['frame_begin'] else None,
                termination=end or {'reason': 'no_end_record'})


def coverage_gaps(events):
    selected = [e for e in events if e.get('event') == 'definition' and e.get('selected')]
    covered = defaultdict(set)
    for event in events:
        if event.get('event') == 'result' and event.get('equal') is True and event.get('different_bytes') == 0:
            covered[event.get('case')].add(event.get('state_age'))
    gaps = []
    for case, definition in enumerate(selected):
        intervals = []
        missing = [age for age in range(max(1, definition.get('duration', 0))) if age not in covered[case]]
        for age in missing:
            if intervals and intervals[-1][1] == age:
                intervals[-1][1] += 1
            else:
                intervals.append([age, age+1])
        if missing:
            gaps.append(dict(definition, case=case, missing_frames=len(missing),
                             missing_age_ranges_end_exclusive=intervals))
    return dict(states_with_gaps=len(gaps), missing_frame_points=sum(g['missing_frames'] for g in gaps), gaps=gaps)


def batch_coverage(cases, characters, moons, filtered=False, combinations=None):
    expected = set(combinations) if combinations is not None else {(c, m) for c in characters for m in moons}
    actual = [(c['character'], c['moon']) for c in cases]
    complete = len(actual) == len(expected) and set(actual) == expected and all(
        c.get('passed') is True and c.get('all_selected_nominal_frames_covered') is True for c in cases)
    return dict(requested_combinations=len(expected), completed_combinations=sum(c.get('comparison_complete', False) for c in cases),
                selected_states=sum(c.get('selected', 0) for c in cases),
                selected_motions=sum(c.get('selected_motions', 0) for c in cases),
                checked_frames=sum(c.get('checked_frames', 0) for c in cases),
                nominal_frame_points=sum(c.get('nominal_frame_points', 0) for c in cases),
                covered_frame_points=sum(c.get('covered_frame_points', 0) for c in cases),
                world_update_frame_points=sum(c.get('world_update_frame_points', c.get('covered_frame_points', 0)) for c in cases),
                actor_setup_only_frame_points=sum(c.get('actor_setup_only_frame_points', 0) for c in cases),
                actor_setup_comparisons=sum(c.get('actor_setup_comparisons', 0) for c in cases),
                missing_frame_points=sum(c.get('missing_frame_points', 0) for c in cases),
                all_standard_character_motion_frames_covered=bool(not filtered and len(actual) == len(set(actual)) and
                    all(any(c['character'] == character and c['moon'] == moon and c.get('passed') is True and
                            c.get('all_selected_nominal_frames_covered') is True for c in cases)
                        for character in CHARACTERS for moon in range(3))),
                all_character_motion_frames_covered=bool(complete and not filtered and expected == set(ALL_COMBINATIONS)),
                all_game_states_covered=False)


def has_end(path):
    if not path.exists():
        return False
    with path.open('rb') as stream:
        stream.seek(max(0, path.stat().st_size - 8192))
        tail = stream.read().decode('utf-8', errors='replace')
    for line in tail.splitlines():
        try:
            if json.loads(line).get('event') == 'end':
                return True
        except (json.JSONDecodeError, AttributeError):
            pass
    return False


def inside_runtime(path):
    path = path.absolute()
    boundary = (ROOT / 'test/runtime').resolve()
    if not path.is_relative_to(boundary) or path == boundary:
        raise ValueError('test/runtime配下の独立した3コピーの親を指定してください')
    for item in (path, *path.parents):
        if item == boundary:
            break
        if item.is_symlink() or item.is_junction():
            raise ValueError(f'リンクは使用できません: {item}')
    return path


def stop_game(game):
    # 絶対パスが一致するプロセスだけ終了する。名前だけで他のゲームを止めない。
    literal = str(game / 'MBAA.exe').replace("'", "''")
    command = ("Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | "
               f"Where-Object {{ $_.ExecutablePath -eq '{literal}' }} | "
               'ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }')
    subprocess.run(['pwsh', '-NoProfile', '-Command', command], check=True,
                   creationflags=subprocess.CREATE_NO_WINDOW)


def choices(value, allowed):
    values = list(allowed) if value == 'all' else [int(v) for v in value.split(',')]
    if not values or len(set(values)) != len(values) or any(v not in allowed for v in values):
        raise ValueError(f'無効な指定: {value}')
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--characters', default='0', help='IDのカンマ区切り、またはall（選択可能31キャラ）')
    parser.add_argument('--moons', default='0', help='0,1,2 またはall。未対応の組合せも失敗として記録')
    parser.add_argument('--catalog', action='store_true', help='状態定義の列挙だけ。戦闘状態を書き換えない')
    parser.add_argument('--pattern', type=int)
    parser.add_argument('--state', type=int)
    parser.add_argument('--entry-only', action='store_true', help='旧方式の入口1Fだけ。全F検査ではない')
    parser.add_argument('--fixtures', action='store_true', help='空中位置・モーション先頭からの自然到達を追加探索')
    parser.add_argument('--hold-fixtures', action='store_true', help='継続に必要な方向・ボタン保持をスクリプトから追加探索')
    parser.add_argument('--primary-side-only', action='store_true', help='選択キャラとそのパートナーを検査。固定相手の重複定義は除く')
    parser.add_argument('--max-state-frames', type=int, default=100000, help='1状態の更新上限。未到達Fは未検査として残す')
    parser.add_argument('--reproduce', type=Path, help='過去のバッチresult.jsonに記録した各組の最初の不一致を再検査')
    parser.add_argument('--seconds', type=float, default=90, help='各キャラ・ムーンの上限。完了時は即終了')
    parser.add_argument('--test-root', type=Path, help='配置済みの独立3コピーを再利用。指定先へ最新3点を配置する')
    parser.add_argument('--workers', type=int, choices=(1, 2, 3), default=1, help='独立コピーごとの同時実行数')
    roster = parser.add_mutually_exclusive_group()
    roster.add_argument('--extra-styles', action='store_true', help='通常93組に収録済みの隠し・専用ムーン10組も加える')
    roster.add_argument('--extra-only', action='store_true', help='隠し・専用ムーン10組だけを検査')
    args = parser.parse_args()
    expanded = args.extra_styles or args.extra_only
    characters = choices(args.characters, ALL_CHARACTERS if expanded else CHARACTERS)
    moons = choices(args.moons, (0, 1, 2, 8, 9) if expanded else range(3))
    available = EXTRA_COMBINATIONS if args.extra_only else ALL_COMBINATIONS
    combinations = [(c, m) for c in characters for m in moons if (c, m) in available]
    if not combinations:
        parser.error('指定したキャラ・ムーンには収録済みの検査対象がありません')
    reproductions = {}
    if args.reproduce:
        if args.pattern is not None or args.state is not None or args.catalog:
            parser.error('--reproduceとpattern/state/catalogは併用できません')
        previous = json.loads(args.reproduce.read_text(encoding='utf-8'))
        for case in previous['cases']:
            if case.get('termination', {}).get('reason') == 'one_update_mismatch':
                reproductions[(case['character'], case['moon'])] = case['last_case']
        if any(pair not in reproductions for pair in combinations):
            parser.error('指定したキャラ・ムーンの不一致ケースが過去結果にありません')
    if (args.seconds <= 0 or not 1 <= args.max_state_frames <= 100000
            or any(v is not None and not 0 <= v < 10000 for v in (args.pattern, args.state))):
        parser.error('上限時間は正、pattern/stateは0〜9999で指定してください')
    stamp = datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    out = ROOT / 'test/logs' / f'state_sweep_{stamp}'
    out.mkdir(parents=True)
    runtime = inside_runtime(args.test_root or ROOT / 'test/runtime' / f'StateSweep_{stamp}')
    sources = [ROOT / 'test/runtime' / f'MBAACC_{i}' for i in range(1, 4)]
    source_before = protected(sources)
    source_copy_unchanged = True
    report = dict(runtime=str(runtime), cases=[], errors=[], passed=False,
                  scope='synthetic_state_frame_steps', all_game_states_covered=False)
    report['requested_combinations'] = combinations
    if args.reproduce:
        report['reproduced_from'] = str(args.reproduce.resolve())
    proc = None
    game = runtime / 'MBAACC_1'
    runtime_before = None
    try:
        if args.test_root is None:
            for source in sources:
                shutil.copytree(source, runtime / source.name,
                                ignore=shutil.ignore_patterns('cccaster_hook_log.txt', 'broadcast'))
            source_copy_unchanged = source_before == protected(sources)
        runtime_before = protected([runtime])
        (out / 'protected_before.json').write_text(json.dumps(dict(
            source=source_before, runtime=runtime_before), ensure_ascii=False, indent=2), encoding='utf-8')
        for i in range(1, 4):
            if sha(runtime / f'MBAACC_{i}' / 'MBAA.exe') != EXE_SHA256:
                raise RuntimeError('逆アセンブル資料とゲームEXEのSHA-256が一致しません')
        with (out / 'deploy.log').open('w', encoding='utf-8') as stream:
            subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'deploy.ps1'), '-TestRoot', str(runtime)],
                           cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT, check=True,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        caster = game / 'cccaster_B'
        report['binaries'] = {name: sha(ROOT / 'build/bin' / name) for name in BINARIES}
        for i in range(1, 4):
            for name, expected in report['binaries'].items():
                if sha(runtime / f'MBAACC_{i}/cccaster_B' / name) != expected:
                    raise RuntimeError('配置バイナリ不一致')
        identities = {name: binary_build_id((caster / name).read_bytes(), name.endswith('.dll')) for name in BINARIES}
        if len(set(identities.values())) != 1:
            raise RuntimeError(f'共有ビルドのCLI・GUI・DLL識別子が不一致: {identities}')
        report['build_identity'] = next(iter(identities.values()))
        report['source_sha256'] = {str(p.relative_to(ROOT)): sha(p) for p in (
            ROOT / 'src/src/core_dll/mbaa_mem/StateSweep.cpp',
            ROOT / 'src/src/core_dll/mbaa_mem/RealGameMemory.cpp',
            ROOT / 'src/src/core_dll/rollback/GameSnapshotLayout.hpp', Path(__file__))}
        (out / 'started.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'Logs: {out}', flush=True)
        report_lock = Lock()
        def run_lane(lane, combinations):
            proc = None
            game = runtime / f'MBAACC_{lane + 1}'
            caster = game / 'cccaster_B'
            for character, moon in combinations:
                if (out / 'stop.request').exists():
                    raise RuntimeError('停止要求により未完了で終了')
                target = out / f'char_{character}_moon_{moon}'
                target.mkdir()
                env = {k: v for k, v in os.environ.items() if not k.startswith('CCCASTER_')}
                env.update(CCCASTER_STATE_SWEEP=str(target), CCCASTER_SWEEP_CHARACTER=str(character),
                           CCCASTER_SWEEP_MOON=str(moon), CCCASTER_SWEEP_CATALOG=str(int(args.catalog)),
                           CCCASTER_SWEEP_ENTRY_ONLY=str(int(args.entry_only)),
                           CCCASTER_SWEEP_FIXTURES=str(int(args.fixtures or args.hold_fixtures)),
                           CCCASTER_SWEEP_HOLD_FIXTURES=str(int(args.hold_fixtures)),
                           CCCASTER_SWEEP_PRIMARY_SIDE_ONLY=str(int(args.primary_side_only)),
                           CCCASTER_SWEEP_MAX_STATE_FRAMES=str(args.max_state_frames))
                for key in ('pattern', 'state'):
                    value = reproductions[(character, moon)][key] if args.reproduce else getattr(args, key)
                    if value is not None:
                        env['CCCASTER_SWEEP_' + key.upper()] = str(value)
                timed_out = False
                try:
                    with (target / 'launcher.log').open('w', encoding='utf-8') as stream:
                        proc = subprocess.Popen([str(caster / 'CCCaster_B.exe'), '--training'], cwd=caster,
                                                env=env, stdout=stream, stderr=subprocess.STDOUT,
                                                creationflags=subprocess.CREATE_NO_WINDOW)
                    deadline = time.monotonic() + args.seconds
                    while proc.poll() is None:
                        if (out / 'stop.request').exists():
                            raise RuntimeError('停止要求により未完了で終了')
                        if time.monotonic() >= deadline:
                            timed_out = True
                            break
                        if has_end(target / 'events.jsonl'):
                            break
                        time.sleep(.1)
                finally:
                    stop_game(game)
                    if proc is not None:
                        # CLIはゲーム終了後もメニューで待つため、毎組4秒の待機は不要。
                        # この試験が起動したCLIだけ回収する。ゲームログはendでflush済み。
                        if proc.poll() is None:
                            proc.terminate()
                        proc.wait(timeout=4)
                        proc = None
                events = read_events(target / 'events.jsonl')
                result = summarize(events, args.catalog)
                result.update(character=character, moon=moon, timed_out=timed_out, path=str(target))
                if timed_out:
                    result['passed'] = False
                for event in events:
                    if event.get('event') == 'start' and (event.get('character'), event.get('moon')) != (character, moon):
                        result['passed'] = False
                if (caster / 'cccaster_hook_log.txt').exists():
                    shutil.copy2(caster / 'cccaster_hook_log.txt', target / 'game.log')
                (target / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
                (target / 'coverage_gaps.json').write_text(json.dumps(coverage_gaps(events), ensure_ascii=False, indent=2), encoding='utf-8')
                with report_lock:
                    report['cases'].append(result)
                    # 各組の完了ごとに結果を残す。途中終了を成功扱いにはしない。
                    (out / 'checkpoint.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
                    print(f'char={character} moon={moon} selected={result["selected"]} '
                          f'frames={result["checked"]} missing={result.get("missing_frame_points", "unknown")} '
                          f'watch={result["watch_changes"]} passed={result["passed"]}', flush=True)
                if not events:
                    raise RuntimeError(f'ゲーム検査が開始されませんでした: {target / "launcher.log"}')
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            futures = [pool.submit(run_lane, lane, combinations[lane::args.workers]) for lane in range(args.workers)]
            for future in as_completed(futures):
                try:
                    future.result()
                except Exception as exc:
                    report['errors'].append(str(exc))
                    (out / 'stop.request').write_text('worker failed', encoding='utf-8')
    except Exception as exc:
        report['errors'].append(str(exc))
    finally:
        if proc is not None:
            stop_game(game)
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=4)
        # 共有コピーは他の作業でも使われる。作成時の保全と試験中の独立runtimeを分けて検証する。
        # 試験中に触らない共有コピーの外部更新を、実行したゲームの設定変更と混同しない。
        report['source_protected_unchanged'] = source_before == protected(sources)
        report['source_copy_unchanged'] = source_copy_unchanged
        report['runtime_protected_unchanged'] = runtime_before is None or runtime_before == protected([runtime])
        report['protected_scope'] = 'source_during_copy_and_runtime_during_test'
        report['protected_unchanged'] = source_copy_unchanged and report['runtime_protected_unchanged']
        report['binaries_unchanged'] = 'binaries' in report and all(
            sha(ROOT / 'build/bin' / name) == expected for name, expected in report.get('binaries', {}).items())
        # 実行中に別作業がbuild/binを更新しても、独立コピーの実行条件とは分ける。
        report['runtime_binaries_unchanged'] = 'binaries' in report and all(
            sha(runtime / f'MBAACC_{i}/cccaster_B' / name) == expected
            for i in range(1, 4) for name, expected in report.get('binaries', {}).items())
        report['passed'] = not report['errors'] and report['protected_unchanged'] and report['runtime_binaries_unchanged'] and (
            len(report['cases']) == len(combinations)) and all(c['passed'] for c in report['cases'])
        report['coverage'] = batch_coverage(report['cases'], characters, moons,
            bool(args.entry_only or args.catalog or args.reproduce or args.pattern is not None or args.state is not None), combinations)
        (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({k: v for k, v in report.items() if k != 'cases'}, ensure_ascii=False), flush=True)
    return int(not report['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
