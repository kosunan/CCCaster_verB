"""解析済みMBAAで、入力なしの状態定義列挙・保存復元往復を実行する。

各stateの入口を作る合成試験であり、実戦で到達可能な全状態・全組合せの証明ではない。
戦闘入力は常に中立。起動・ロード・登場演出は既存経路の決定入力でスキップする。
"""
import argparse
from collections import defaultdict
from datetime import datetime
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]
EXE_SHA256 = '6d1415ca9573100e86a779ac2f81e9bedd322664e3daeae0229a67d13720310a'
CHARACTERS = (22, 7, 51, 15, 28, 8, 2, 0, 30, 11, 9, 31, 4, 3, 1, 19, 12,
              13, 14, 29, 17, 18, 33, 23, 10, 25, 35, 5, 20, 6, 34)
BINARIES = ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll')


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
    for e in results:
        frames_by_case[e.get('case')].append(e)
        case, age = e.get('case'), e.get('state_age')
        if (isinstance(case, int) and 0 <= case < len(selected) and valid_duration
                and isinstance(age, int) and 0 <= age < max(1, selected[case]['duration'])
                and e.get('equal') is True and e.get('different_bytes') == 0):
            covered_by_case[case].add(age)
    covered = sum(len(ages) for ages in covered_by_case.values())
    if catalog:
        complete = False
        passed = valid and end.get('status') == 'catalog' and not results and not begins
    else:
        valid &= end.get('status') == 'complete' and end.get('checked') == len(results)
        valid &= end.get('ended_cases') == len(selected) and len(begins) == len(finished) == len(selected)
        valid &= [e.get('case') for e in begins] == list(range(len(selected)))
        valid &= [e.get('case') for e in finished] == list(range(len(selected)))
        valid &= [(e.get('slot'), e.get('pattern'), e.get('state')) for e in begins] == identities
        valid &= [(e.get('case'), e.get('frame'), e.get('state_age')) for e in groups['frame_begin']] == [
            (e.get('case'), e.get('frame'), e.get('state_age')) for e in results]
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
                ended_states=len(finished), checked=len(results), checked_frames=len(results),
                nominal_frame_points=nominal, covered_frame_points=covered,
                missing_frame_points=max(0, nominal-covered),
                matched=sum(e.get('equal') is True for e in results),
                watch_changes=len(groups['watch']),
                watch_status='needs_review' if groups['watch'] else 'no_changes_observed',
                state_end_reasons=dict((reason, sum(e.get('reason') == reason for e in finished))
                                      for reason in sorted({e.get('reason', '') for e in finished})),
                last_case=begins[-1] if begins else None,
                last_frame=groups['frame_begin'][-1] if groups['frame_begin'] else None,
                termination=end or {'reason': 'no_end_record'})


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
    parser.add_argument('--max-state-frames', type=int, default=600, help='1状態の更新上限。未到達Fは未検査として残す')
    parser.add_argument('--reproduce', type=Path, help='過去のバッチresult.jsonに記録した各組の最初の不一致を再検査')
    parser.add_argument('--seconds', type=float, default=90, help='各キャラ・ムーンの上限。完了時は即終了')
    parser.add_argument('--test-root', type=Path, help='配置済みの独立3コピーを再利用。指定先へ最新3点を配置する')
    args = parser.parse_args()
    characters, moons = choices(args.characters, CHARACTERS), choices(args.moons, range(3))
    reproductions = {}
    if args.reproduce:
        if args.pattern is not None or args.state is not None or args.catalog:
            parser.error('--reproduceとpattern/state/catalogは併用できません')
        previous = json.loads(args.reproduce.read_text(encoding='utf-8'))
        for case in previous['cases']:
            if case.get('termination', {}).get('reason') == 'one_update_mismatch':
                reproductions[(case['character'], case['moon'])] = case['last_case']
        if any((c, m) not in reproductions for c in characters for m in moons):
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
    report = dict(runtime=str(runtime), cases=[], errors=[], passed=False,
                  scope='synthetic_state_frame_steps', all_game_states_covered=False)
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
        runtime_before = protected([runtime])
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
        print(f'Logs: {out}', flush=True)
        for character in characters:
            for moon in moons:
                target = out / f'char_{character}_moon_{moon}'
                target.mkdir()
                env = {k: v for k, v in os.environ.items() if not k.startswith('CCCASTER_')}
                env.update(CCCASTER_STATE_SWEEP=str(target), CCCASTER_SWEEP_CHARACTER=str(character),
                           CCCASTER_SWEEP_MOON=str(moon), CCCASTER_SWEEP_CATALOG=str(int(args.catalog)),
                           CCCASTER_SWEEP_ENTRY_ONLY=str(int(args.entry_only)),
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
                report['cases'].append(result)
                print(f'char={character} moon={moon} selected={result["selected"]} '
                      f'frames={result["checked"]} missing={result.get("missing_frame_points", "unknown")} '
                      f'watch={result["watch_changes"]} passed={result["passed"]}', flush=True)
    except Exception as exc:
        report['errors'].append(str(exc))
    finally:
        if proc is not None:
            stop_game(game)
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=4)
        report['protected_unchanged'] = source_before == protected(sources) and (
            runtime_before is None or runtime_before == protected([runtime]))
        report['binaries_unchanged'] = 'binaries' in report and all(
            sha(ROOT / 'build/bin' / name) == expected for name, expected in report.get('binaries', {}).items())
        # 実行中に別作業がbuild/binを更新しても、独立コピーの実行条件とは分ける。
        report['runtime_binaries_unchanged'] = 'binaries' in report and all(
            sha(runtime / f'MBAACC_{i}/cccaster_B' / name) == expected
            for i in range(1, 4) for name, expected in report.get('binaries', {}).items())
        report['passed'] = not report['errors'] and report['protected_unchanged'] and report['runtime_binaries_unchanged'] and (
            len(report['cases']) == len(characters) * len(moons)) and all(c['passed'] for c in report['cases'])
        (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({k: v for k, v in report.items() if k != 'cases'}, ensure_ascii=False), flush=True)
    return int(not report['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
