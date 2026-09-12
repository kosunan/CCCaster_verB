"""実ゲーム両側の確定スコアを照合する。集計なし・二重加算も不合格。"""
import argparse
import json
import re
from pathlib import Path

ROW = re.compile(r'\[SessionScore\] revision=(\d+) match=(\d+) p1=(\d+) p2=(\d+) unresolved=(\d+) rounds=(\d+)/(\d+) required=(\d+)')


def inspect(text):
    rows = [tuple(map(int, m.groups())) for m in ROW.finditer(text)]
    errors = []
    if not rows:
        errors.append('確定結果なし')
    generations = set()
    previous = (0, 0, 0)
    for index, (revision, generation, p1, p2, unresolved, r1, r2, required) in enumerate(rows, 1):
        if revision != index or generation in generations or not generation:
            errors.append('revisionまたは試合世代の重複・欠落')
        generations.add(generation)
        valid = 1 <= required <= 5 and r1 <= required and r2 <= required
        winner1, winner2 = valid and r1 == required, valid and r2 == required
        expected = (int(winner1 and not winner2), int(winner2 and not winner1), int(winner1 == winner2))
        current = (p1, p2, unresolved)
        if tuple(a-b for a, b in zip(current, previous)) != expected:
            errors.append('ラウンド事実と集計増分の不一致')
        previous = current
    return rows, errors


def compare(host, client):
    h, he = inspect(host)
    c, ce = inspect(client)
    errors = [f'host: {e}' for e in he] + [f'client: {e}' for e in ce]
    if h != c:
        errors.append('両側の確定スコア列が不一致')
    return dict(passed=not errors, host=h, client=c, errors=errors)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = compare(*[(args.directory / f'game_{side}.log').read_text(encoding='utf-8') for side in (1, 2)])
    path = args.directory / 'session_score_comparison.json'
    path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
