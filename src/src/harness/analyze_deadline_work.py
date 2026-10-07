"""採用担当の処理・公開だけを集計し、非採用者と受取側の遅れを混ぜない。"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re

from analyze_deadline_diagnostics import distribution


def analyze(text):
    rows = []
    for line in text.splitlines():
        if not line.startswith('[DeadlineWork] '):
            continue
        row = {key: int(value) for key, value in re.findall(r'(\w+)=(-?\d+)(?=\s|$)', line)}
        required = {'f', 'worker', 'due', 'start', 'done', 'pub', 'seen', 'reader', 'armed'}
        if not required <= row.keys():
            raise ValueError('採用担当の計測行が欠落・途中切れ')
        if (not -1 <= row['worker'] < 4 or row['reader'] not in (0, 1) or
                not row['due'] <= row['start'] <= row['done'] <= row['pub'] <= row['seen']):
            raise ValueError('採用担当の時刻・番号が不正')
        rows.append(row)
    if not rows:
        raise ValueError('採用担当の計測がない')
    # 過去の音声締切をnowへ丸めた区間を、遅れゼロとして扱わない。
    valid = [row for row in rows if row['armed'] < row['due']]
    return dict(
        meaning='採用担当のみ。pubは結果公開時刻の上限。seenは受取り側の別指標。',
        samples=len(rows), measured_deadlines=len(valid), late_submission_or_clamped=len(rows)-len(valid),
        adopted_workers=dict(Counter(str(row['worker']) for row in rows)),
        publication_upper_from_reader=sum(row['reader'] for row in valid),
        adopted_start_late_us=distribution([(row['start']-row['due'])/60 for row in valid]),
        adopted_complete_late_us=distribution([(row['done']-row['due'])/60 for row in valid]),
        adopted_publication_upper_late_us=distribution([(row['pub']-row['due'])/60 for row in valid]),
        receiver_after_completion_us=distribution([(row['seen']-row['done'])/60 for row in valid]),
        rows=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = {str(path): analyze(path.read_text(encoding='utf-8-sig')) for path in args.logs}
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({path: {k:v for k,v in report.items() if k != 'rows'} for path,report in result.items()}, ensure_ascii=False))


if __name__ == '__main__':
    main()
