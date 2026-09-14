"""MBAA標準REPを旧ReplayCreatorと同じ構造で読み、長さ・入力・RNGを照合する。"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def inspect(path):
    data = Path(path).read_bytes()
    if len(data) < 96 or data[:14] != b'MBAAReplayFile':
        raise ValueError('標準REPヘッダーではありません')
    rounds = struct.unpack_from('<I', data, 92)[0]
    if not 0 < rounds <= 10000:
        raise ValueError('ラウンド数が範囲外です')
    position = 96
    def take(size):
        nonlocal position
        if size > len(data) - position:
            raise ValueError('リプレイが途中で切れています')
        result = data[position:position+size]
        position += size
        return result
    result = []
    for _ in range(rounds):
        header = take(140)
        streams = []
        for _ in range(4):
            count, = struct.unpack('<I', take(4))
            inputs = take(count*6)
            streams.append(dict(entries=count, frames=sum(inputs[::6]),
                                sha256=hashlib.sha256(inputs).hexdigest()))
        count, = struct.unpack('<I', take(4))
        rng = take(count*4)
        tail = take(144)
        result.append(dict(inputs=streams, rngCount=count, rngSha256=hashlib.sha256(rng).hexdigest(),
                           stateSha256=hashlib.sha256(header+tail).hexdigest()))
    if position != len(data):
        raise ValueError('リプレイ末尾に余分なデータがあります')
    return dict(path=str(Path(path).resolve()), bytes=len(data), rounds=result,
                sha256=hashlib.sha256(data).hexdigest(),
                battleSha256=hashlib.sha256(data[96:]).hexdigest())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('files', nargs='+', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = [inspect(path) for path in args.files]
    text = json.dumps(report, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text, encoding='utf-8')
    print(text)
