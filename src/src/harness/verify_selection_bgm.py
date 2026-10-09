"""ゲームのBGMバッファから採取した初回／復帰後のPCMを同じ曲位置で比較する。"""
import argparse
import json
from pathlib import Path


def compare_pcm(first, returned, first_start, returned_start, bytes_per_second, minimum_seconds=10):
    start=max(first_start,returned_start)
    first=first[start-first_start:]
    returned=returned[start-returned_start:]
    count=min(len(first),len(returned))
    first,returned=first[:count],returned[:count]
    differences=sum(a!=b for a,b in zip(first,returned))
    first_difference=next((i for i,(a,b) in enumerate(zip(first,returned)) if a!=b),None)
    audible=any(first) and any(returned)
    return dict(passed=count>=bytes_per_second*minimum_seconds and audible and differences==0,
                compared_bytes=count,compared_seconds=count/bytes_per_second,different_bytes=differences,
                first_difference_seconds=None if first_difference is None else first_difference/bytes_per_second,
                non_silent=audible)


def verify(folder):
    folder=Path(folder)
    paths=[folder/f'bgm-{i}.pcm' for i in (1,2)]
    metadata=[list(map(int,p.with_suffix('.txt').read_text().split())) for p in paths]
    for meta in metadata:
        if len(meta)==5: meta.append(0)  # 修正前の診断記録との比較用。
        if meta[:4]!=[1,2,44100,16]: raise ValueError('対象外のPCM形式')
    result=compare_pcm(*(p.read_bytes() for p in paths),metadata[0][5],metadata[1][5],44100*4)
    (folder/'waveform_result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder')
    result=verify(parser.parse_args().folder)
    print(json.dumps(result))
    raise SystemExit(0 if result['passed'] else 1)
