"""通常更新の全連続区間を整数ticksで判定する。起動後・初回RB前も除外しない。"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

PERIOD = 1_000_000  # 1/60 microsecond ticks
LIMIT = 180        # 3 microseconds; C++ UpdateCadence::Limit と一致


def read_evidence(path):
    data = path.read_bytes()
    if len(data) < 72:
        raise ValueError('状態ヘッダー欠落')
    h = struct.unpack_from('<12I3q', data)
    magic, version, frame, previous, ordinal, pid, count, fp_size, confirmed, next_frame, host, limit = h[:12]
    if magic != 0x53504343 or version != 1 or count != 10 or not 0 < fp_size <= 512 or limit != LIMIT:
        raise ValueError('状態形式不一致')
    offset, rows = 72, []
    for expected in range(frame - 10, frame):
        if offset + 32 > len(data):
            raise ValueError('状態レコード欠落')
        f, valid, size, local, remote, input_valid, resolved_local, resolved_remote = struct.unpack_from('<8I', data, offset)
        offset += 32
        if f != expected or valid not in (0, 1) or input_valid not in (0, 1) or (not valid and size):
            raise ValueError('状態フレームまたはサイズ不正')
        length = size + fp_size if valid else 0
        if (valid and not size) or offset + length > len(data):
            raise ValueError('状態本体切断')
        rows.append(dict(frame=f, valid=bool(valid), bytes=size, offset=offset,
                         input_valid=bool(input_valid), local=local, remote=remote,
                         resolved_local=resolved_local, resolved_remote=resolved_remote,
                         sha256=hashlib.sha256(data[offset:offset + length]).hexdigest()))
        offset += length
    if offset != len(data):
        raise ValueError('状態末尾の余剰バイト')
    return dict(path=str(path), sha256=hashlib.sha256(data).hexdigest(), frame=frame,
                ordinal=ordinal, pid=pid, previous=previous, confirmed=confirmed,
                next=next_frame, host=bool(host), ticks=h[12], interval=h[13], error=h[14],
                complete=all(r['valid'] and r['input_valid'] for r in rows), rows=rows)


def analyze_text(text, minimum=600, evidence_dir=None):
    failures, samples, intervals, spikes, evidence = [], [], [], [], []
    previous = None
    required = {'n', 'f', 'prev', 'ticks', 'interval', 'error', 'consecutive', 'spike', 'dropped', 'evidence', 'play'}
    for line in text.splitlines():
        if not line.startswith('[UpdateCadence]'):
            continue
        row = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
        if set(row) != required:
            failures.append('計測レコード欠落・形式不正')
            continue
        samples.append(row)
        if any(row[k] not in (0, 1) for k in ('play', 'spike', 'evidence', 'consecutive')):
            failures.append(f"{row['f']}: フラグ不正")
        if row['n'] != len(samples) or row['dropped']:
            failures.append(f"{row['f']}: 計測欠落")
        consecutive = bool(previous and previous['play'] and row['play'] and
                           previous['f'] // 65536 == row['f'] // 65536 and row['f'] == previous['f'] + 1)
        if previous and (row['prev'] != previous['f'] or row['ticks'] <= previous['ticks']):
            failures.append(f"{row['f']}: 時刻または前F不整合")
        if previous and previous['play'] and row['play'] and previous['f'] // 65536 == row['f'] // 65536 and not consecutive:
            failures.append(f"{row['f']}: 通常戦闘のフレーム欠落")
        if row['consecutive'] != int(consecutive):
            failures.append(f"{row['f']}: 連続区間不整合")
        if consecutive:
            delta = row['ticks'] - previous['ticks']
            error = delta - PERIOD
            spike = abs(error) > LIMIT
            if (row['interval'], row['error'], row['spike']) != (delta, error, int(spike)):
                failures.append(f"{row['f']}: 差分または検出フラグ不整合")
            intervals.append(dict(frame=row['f'], ticks=row['ticks'], interval_ticks=delta, error_ticks=error))
            if spike:
                spikes.append(intervals[-1])
        elif row['interval'] or row['error'] or row['spike']:
            failures.append(f"{row['f']}: 非連続区間の差分が非ゼロ")
        previous = row
    if len(intervals) < minimum:
        failures.append(f'通常戦闘の連続区間不足: {len(intervals)} < {minimum}')
    if re.search(r'\[\w*Drop(?:ped)?\]', text):
        failures.append('診断ログ欠落')
    # 元FRAME記録との独立照合。初回RBより前も、採取窓内の未確定Fも含む。
    observed = {s['f'] for s in samples}
    combat = {int(m[1]) for m in re.finditer(r'^\[FRAME\] (\d+) 4 0 ', text, re.M)}
    missing = sorted(combat - observed)
    if missing:
        failures.append(f'戦闘FRAMEに対応する採取なし: {missing[:16]}')
    excluded = sorted(combat & {s['f'] for s in samples if not s['play']})
    if excluded:
        failures.append(f'戦闘FRAMEを非戦闘として除外: {excluded[:16]}')
    errors = sorted(abs(r['error_ticks']) for r in intervals)
    maximum = errors[-1] if errors else None
    if spikes:
        failures.append(f'最大絶対誤差 {maximum / 60:.6f}us > {LIMIT / 60:g}us ({len(spikes)}区間)')
    diagnostic = any(s['evidence'] for s in samples)
    # 観測区間の内訳。これだけでOS/ドライバの因果帰属を断定しない。
    tables = {}
    for line in text.splitlines():
        tag = line.split(' ', 1)[0]
        if tag in ('[SpinProbe]', '[SpinTail]', '[SpinBegin]', '[InputWait]', '[ReplayWork]', '[DeadlineTraceCost]', '[SpikeState]'):
            values = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
            if 'f' in values:
                tables.setdefault((tag, values['f']), []).append(values)
    for spike in spikes:
        f = spike['frame']
        spike['context'] = {tag.strip('[]'): tables[(tag, f)] for tag in
            ('[SpinProbe]', '[SpinTail]', '[SpinBegin]', '[InputWait]', '[ReplayWork]', '[DeadlineTraceCost]') if (tag, f) in tables}
        spike['previous_export'] = tables.get(('[SpikeState]', f - 1), [])
        spike['attribution'] = '未帰属（付随区間は観測値）'
    if diagnostic:
        markers = {}
        for line in text.splitlines():
            if line.startswith('[SpikeState]'):
                row = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', line)}
                match = re.search(r' file=(.*)$', line)
                if match:
                    markers[row['n']] = (row, Path(match[1]).name)
        for s in samples:
            if not s['spike']:
                continue
            try:
                marker, name = markers[s['n']]
                if not marker['ok'] or not marker['complete'] or not evidence_dir:
                    raise ValueError('直前10F未完または書出し失敗')
                item = read_evidence(Path(evidence_dir) / name)
                if not item['complete'] or any(item[k] != s[v] for k, v in
                    [('frame', 'f'), ('ordinal', 'n'), ('ticks', 'ticks'), ('interval', 'interval'), ('error', 'error')]):
                    raise ValueError('状態と検出時刻の対応不一致')
                evidence.append(item)
            except (KeyError, OSError, ValueError, struct.error) as exc:
                failures.append(f"{s['f']}: 証拠保存不備: {exc}")
    return dict(passed=not failures, threshold_us=LIMIT / 60, period_us=PERIOD / 60,
                samples=len(samples), normal_samples=sum(s['play'] for s in samples),
                intervals=len(intervals), maximum_abs_error_us=None if maximum is None else maximum / 60,
                spikes=spikes, failures=failures, evidence=evidence,
                diagnostic_state_export=diagnostic, missing_frames=missing)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('folder', type=Path)
    p.add_argument('--minimum', type=int, default=600)
    a = p.parse_args()
    if a.minimum < 1:
        p.error('minimumは1以上')
    result = {str(s): analyze_text((a.folder / f'game_{s}.log').read_text(encoding='utf-8'),
                                  a.minimum, a.folder / 'states') for s in (1, 2)}
    (a.folder / 'update_cadence.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({s: {k: v for k, v in r.items() if k not in ('spikes', 'evidence')} for s, r in result.items()}, ensure_ascii=False, indent=2))
    return 0 if all(r['passed'] for r in result.values()) else 1


if __name__ == '__main__':
    raise SystemExit(main())
