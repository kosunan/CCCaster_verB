"""DDS再圧縮の差を4x4ブロックとRGBA値で比較する（DXT2/3/4/5）。"""
import argparse
import json
import struct
from pathlib import Path


def colors(value):
    r, g, b = (value >> 11) & 31, (value >> 5) & 63, value & 31
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


def decode(block, fourcc):
    c0, c1, indices = struct.unpack('<HHI', block[8:])
    palette = [colors(c0), colors(c1)]
    palette += [tuple((2*a+b)//3 for a,b in zip(*palette)),
                tuple((a+2*b)//3 for a,b in zip(*palette))]
    if fourcc in (b'DXT2', b'DXT3'):
        bits = int.from_bytes(block[:8], 'little')
        alpha = [((bits >> (4*i)) & 15) * 17 for i in range(16)]
    else:
        a, b = block[0], block[1]
        table = [a, b]
        table += [((7-i)*a+i*b)//7 for i in range(1,7)] if a > b else [((5-i)*a+i*b)//5 for i in range(1,5)] + [0,255]
        bits = int.from_bytes(block[2:8], 'little')
        alpha = [table[(bits >> (3*i)) & 7] for i in range(16)]
    return [(*palette[(indices >> (2*i)) & 3], alpha[i]) for i in range(16)]


def compare(a, b):
    if a[:128] != b[:128] or len(a) != len(b):
        raise ValueError('ヘッダーまたは長さが不一致')
    h, w = struct.unpack_from('<II', a, 12)
    fourcc = a[84:88]
    if fourcc not in (b'DXT2',b'DXT3',b'DXT4',b'DXT5') or len(a) != 128+w*h:
        raise ValueError('対象外DDS')
    result = dict(width=w, height=h, format=fourcc.decode(), blocks=w*h//16,
                  changedBlocks=0, changedPixels=0, changedAlphaPixels=0,
                  maxRGBDelta=0, maxAlphaDelta=0)
    for offset in range(128,len(a),16):
        x,y = a[offset:offset+16], b[offset:offset+16]
        if x == y:
            continue
        result['changedBlocks'] += 1
        for p,q in zip(decode(x,fourcc),decode(y,fourcc)):
            result['changedPixels'] += p != q
            result['changedAlphaPixels'] += p[3] != q[3]
            result['maxRGBDelta'] = max(result['maxRGBDelta'], *(abs(p[i]-q[i]) for i in range(3)))
            result['maxAlphaDelta'] = max(result['maxAlphaDelta'], abs(p[3]-q[3]))
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root',type=Path)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    results=[]
    for candidate in sorted(args.root.glob('*_candidate.dds'),key=lambda p:int(p.name.split('_')[0])):
        reference=candidate.with_name(candidate.name.replace('candidate','reference'))
        results.append(dict(file=candidate.name,**compare(candidate.read_bytes(),reference.read_bytes())))
    summary={'textures':len(results),'changedBlocks':sum(r['changedBlocks'] for r in results),
             'changedPixels':sum(r['changedPixels'] for r in results),
             'changedAlphaPixels':sum(r['changedAlphaPixels'] for r in results),
             'maxRGBDelta':max((r['maxRGBDelta'] for r in results),default=0),
             'maxAlphaDelta':max((r['maxAlphaDelta'] for r in results),default=0)}
    args.output.write_text(json.dumps({'summary':summary,'textures':results},indent=2),encoding='utf-8')
    print(json.dumps(summary))


if __name__=='__main__':
    main()
