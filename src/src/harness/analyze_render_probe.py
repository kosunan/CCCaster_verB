"""D3D9計測を通常描画・再計算中の描画へ分ける（API経過時間、GPU完了時間ではない）。"""
import json
import math
import re
import sys
from pathlib import Path


def dist(values):
    values = sorted(values)
    if not values:
        return {"count": 0}
    return dict(count=len(values), median=values[len(values)//2],
                p99=values[math.ceil(len(values)*.99)-1], maximum=values[-1])


def analyze(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    tags = {}
    for line in text.splitlines():
        tag = re.match(r"\[(\w+)\]", line)
        if tag:
            row = {k: v for k, v in re.findall(r"(\w+)=([^ ]+)", line)}
            tags.setdefault(tag[1], []).append(row)
    first = re.search(r"\[Rollback\] BEGIN frame=\d+ target=(\d+)", text)
    if not first:
        return {"error": "ロールバック区間なし"}
    first = int(first[1])
    playable = {int(r['f']) for r in (tags.get('Pace', []) + tags.get('SpinProbe', [])) if r.get('play') == '1'
                and int(r['f']) >= first and int(r['f']) // 65536 == first // 65536}
    frames = {int(r['seq']): r for r in tags.get('RenderFrame', []) if int(r['f']) in playable}
    report = {}
    for skip in ('0', '1'):
        selected = {seq: r for seq, r in frames.items() if r['skip'] == skip}
        apis = {}
        totals = {seq: 0 for seq in selected}
        for r in tags.get('RenderApi', []):
            seq = int(r['seq'])
            if seq not in selected:
                continue
            apis.setdefault(r['api'], {})[seq] = r
            totals[seq] += int(r['ticks']) / 60
        report['normal' if skip == '0' else 'render_skipped'] = {
            'frames': len(selected),
            'passes': dist([int(r['passes']) for r in selected.values()]),
            'api_total_us': dist(list(totals.values())),
            'apis': {api: {
                'calls_per_frame': dist([int(rows.get(seq, {}).get('count', 0)) for seq in selected]),
                'us_per_frame': dist([int(rows.get(seq, {}).get('ticks', 0))/60 for seq in selected]),
                'max_call_us': max(int(r['maxTicks'])/60 for r in rows.values()),
                'failed': sum(int(r['failed']) for r in rows.values())}
                for api, rows in apis.items()},
            'pass_examples': [r for r in tags.get('RenderPass', []) if int(r['seq']) in selected][:24]}
    report['hook_status'] = tags.get('RenderProbeHook', [])
    sampled = [r for r in tags.get('RenderPass', []) if int(r['seq']) in frames]
    targets = {}
    for r in sampled:
        key = f"back={r['back']} {r['w']}x{r['h']}"
        group = targets.setdefault(key, dict(passes=0, without_draw=0, draws=0))
        group['passes'] += 1
        group['without_draw'] += int(r['draws']) == 0
        group['draws'] += int(r['draws'])
    report['sampled_pass_targets'] = targets
    report['unmeasured'] = 'Texture/Surface/VertexBuffer/IndexBufferのLock/Unlock、GPU完了、未フックAPI、HUD。'
    return report


if __name__ == '__main__':
    root = Path(sys.argv[1])
    output = json.dumps({str(side): analyze(root / f'game_{side}.log') for side in (1, 2)},
                        ensure_ascii=False, indent=2)
    (root / 'render_analysis.json').write_text(output, encoding='utf-8')
    print(output)
