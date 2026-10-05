"""通常窓と全画面のキャラ選択画像で、実ゲームの縦横比を比較する。

Computer Useで採取した画像を入力する。外枠だけが4:3でも、ゲーム本体が
二重に横圧縮されれば失敗する。描画の拡大補間/JPEGに対して相関で比較。
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def compare(reference, fullscreen, client, content):
    if min(client[2:]) <= 0 or min(content[2:]) <= 0:
        raise ValueError("比較領域の幅と高さは正数で指定してください")
    scale_x, scale_y = content[2] / client[2], content[3] / client[3]
    # 縮小して画像を比較する前に、物理画素での縦横倍率も照合する。
    ratio_matches = abs(content[2] * client[3] - content[3] * client[2]) <= max(client[2:])
    x, y, w, h = client
    a = Image.open(reference).convert("RGB").crop((x, y, x + w, y + h)).resize((640, 480))
    x, y, w, h = content
    b = Image.open(fullscreen).convert("RGB").crop((x, y, x + w, y + h)).resize((640, 480))
    a, b = np.asarray(a, dtype=float), np.asarray(b, dtype=float)
    # カーソル・選択中の点滅・動く背景を避け、上段の静止した顔を比べる。
    regions = [(266, 52, 28, 30), (305, 52, 28, 30), (385, 52, 28, 30)]
    scores = []
    for x, y, w, h in regions:
        src = a[y:y+h, x:x+w].ravel().copy()
        src -= src.mean()
        best = -1.0
        for dy in range(-1, 2):
            for dx in range(-1, 2):
                dst = b[y+dy:y+dy+h, x+dx:x+dx+w].ravel().copy()
                dst -= dst.mean()
                denominator = np.linalg.norm(src) * np.linalg.norm(dst)
                best = max(best, float(np.dot(src, dst) / denominator) if denominator else 0)
        scores.append(best)
    return {"portrait_correlations": scores, "threshold": 0.96,
            "scale_x": scale_x, "scale_y": scale_y, "ratio_matches": ratio_matches,
            "passed": ratio_matches and all(score >= 0.96 for score in scores),
            "window_client": client, "fullscreen_content": content}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("windowed", type=Path)
    parser.add_argument("fullscreen", type=Path)
    parser.add_argument("--client", nargs=4, type=int, required=True, metavar=("X", "Y", "W", "H"))
    parser.add_argument("--content", nargs=4, type=int, required=True, metavar=("X", "Y", "W", "H"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = compare(args.windowed, args.fullscreen, args.client, args.content)
    output = json.dumps(result, ensure_ascii=False, indent=2)
    print(output)
    if args.output:
        args.output.write_text(output + "\n", encoding="utf-8")
    raise SystemExit(0 if result["passed"] else 1)
