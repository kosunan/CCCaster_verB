"""固定したETMソースから資料用レイアウトを再生成。ゲームには接続しない。"""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

COMMIT = "038887d7d8e6e70963ce9eb5780a25ac35cf1cac"
SOURCE = "Extended-Training-Mode-DLL/DebugInfo.h"
URL = f"https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/{COMMIT}/"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_root", type=Path)
    args = parser.parse_args()
    root = args.source_root
    actual_commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    if actual_commit != COMMIT or subprocess.check_output(["git", "-C", str(root), "status", "--porcelain"], text=True).strip():
        raise ValueError("指定コミットの変更なしチェックアウトを指定してください")
    lines = (root / SOURCE).read_text(encoding="utf-8-sig").splitlines()
    sizes = {"BYTE": 1, "byte": 1, "char": 1, "WORD": 2, "short": 2,
             "DWORD": 4, "int": 4, "float": 4, "uint8_t": 1}
    specifications = [("CameraBoxData", 0, 0x10, None), ("RawBoxData", 0, 8, None),
                      ("ActorData", 0, 0x338, 0x555134),
                      ("PlayerData", 0x33C, 0xAFC, 0x555130),
                      ("AttackDisplayData", 0, 0x18, None),
                      ("ComboCalcData", 0, 0x2C, None),
                      ("PlayerAuxData", 0, 0x20C, 0x557DB8),
                      ("CommandData", 0, 0x2C, None),
                      ("CommandFileData", 0, 0x68, None)]
    structs = []
    checks = 0
    for name, offset, expected, base in specifications:
        start = next(i for i, line in enumerate(lines)
                     if re.match(r"typedef struct " + name + r"(?:\s|:)", line) and "{" in line)
        end = next(i for i in range(start + 1, len(lines)) if lines[i].strip() == "} " + name + ";")
        fields = []
        for i in range(start + 1, end):
            code = lines[i].split("//", 1)[0].strip()
            if not code:
                continue
            unused = re.fullmatch(r"UNUSED\((0x[0-9A-Fa-f]+|\d+)\);", code)
            if unused:
                offset += int(unused[1], 0)
                continue
            m = re.fullmatch(r"([\w<>*]+)\s+(\w+)(?:\[(0x[0-9A-Fa-f]+|\d+)\])?;", code)
            if not m:
                raise ValueError(f"未対応の宣言 {SOURCE}:{i+1}: {code}")
            typ, field, count = m.groups()
            count = int(count, 0) if count else 1
            size = (4 if typ.endswith("*") else sizes[typ]) * count
            fields.append(dict(name=field, type=typ, count=count, offset=f"0x{offset:X}",
                               size=size, example_va=f"0x{base+offset:08X}" if base else None,
                               source_line=i+1, source_url=URL+SOURCE+f"#L{i+1}"))
            offset += size
        if offset != expected:
            raise ValueError(f"{name}: {offset:#x} != {expected:#x}")
        next_struct = next((i for i in range(end+1, len(lines)) if "typedef struct " in lines[i]), len(lines))
        for line in lines[end+1:next_struct]:
            check = re.search(r"CHECKOFFSET\((\w+),\s*(0x[0-9a-fA-F]+|\d+)\)", line)
            if check and check[1] != "exists":
                actual = next(f for f in fields if f["name"] == check[1])
                assert int(actual["offset"], 0) == int(check[2], 0), check[1]
                checks += 1
        sizes[name] = expected
        structs.append(dict(name=name, size=expected, inherited_bytes=specifications[len(structs)][1],
                            example_base=f"0x{base:08X}" if base else None, fields=fields))
    sources = ["README.md", "Common/Common.h", SOURCE,
               "Extended-Training-Mode-DLL/SaveState.h", "Extended-Training-Mode-DLL/SaveState.cpp",
               "Extended-Training-Mode-DLL/FrameBar.cpp"]
    manifest = {p: hashlib.sha256((root/p).read_bytes()).hexdigest() for p in sources}
    data = dict(upstream_commit=COMMIT, pointer_bytes=4, packing=1,
                status="参照ソースの静的解析。実ゲーム未確認。", source_sha256=manifest,
                checked_offsets=checks, structs=structs)
    out = Path(__file__).resolve().parent
    (out/"etm_layout.json").write_text(json.dumps(data, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    md = ["# ETM構造体フィールド索引", "", "[調査資料と注意点](README.md)へ戻る。",
          "", f"参照コミット: `{COMMIT}`。32bit・pack(1)として算出。実ゲーム未確認。",
          "ActorDataの例示VAはP1のsubObj基点、PlayerDataはP1本体、PlayerAuxDataはP1側補助構造体。",
          "PlayerDataの先頭0x33CはEffectData継承部分（exists 4B + ActorData）。未命名の空白は省略。",
          "型は参照元の宣言であり、変数名だけでは意味・符号・有効条件を保証しない。",
          "", f"算術検査: {len(structs)}構造体のサイズとCHECKOFFSET {checks}件一致。", ""]
    for st in structs:
        md += [f"## {st['name']}（0x{st['size']:X} bytes）", "",
               "| フィールド | 型・要素数 | 相対offset | bytes | 例示VA | 根拠 |",
               "|---|---|---|---|---|---|"]
        for f in st["fields"]:
            md.append(f"| `{f['name']}` | `{f['type']}[{f['count']}]` | `{f['offset']}` | {f['size']} | {f['example_va'] or '動的／親構造体基点'} | [L{f['source_line']}]({f['source_url']}) |")
        md.append("")
    (out/"etm_fields.md").write_text("\n".join(md), encoding="utf-8")
    layout_path = out.parents[1]/"src/core_dll/rollback/GameSnapshotLayout.hpp"
    layout = layout_path.read_text(encoding="utf-8")
    direct = [(int(a, 0), int(n, 0)) for a, n in re.findall(
        r"\{-1,\s*(0x[\da-fA-F]+),\s*0x0,\s*(\d+)\}", layout)]
    regions = [("CameraZoom", 0x54EB70, 12), ("CameraDestination", 0x555124, 12),
               *[(f"Player{i+1}", 0x555130+0xAFC*i, 0x3E8) for i in range(4)],
               ("PlayerAux4", 0x557DB8, 0x830), ("StopSituation", 0x558600, 0xF38),
               ("FrameCount", 0x55D1CC, 4), ("SlowMo", 0x55D208, 2),
               ("Camera", 0x55DEC4, 12), ("TrueFrameCount", 0x562A40, 4),
               ("GlobalFreeze", 0x562A48, 4), ("RNG", 0x564068, 0xE4),
               ("CameraNext", 0x564B14, 12), ("Effects", 0x67BDE8, 0x33C*1000)]
    coverage = []
    for name, begin, size in regions:
        spans = [(begin, begin+size)]
        for a, n in direct:
            remaining = []
            for lo, hi in spans:
                if hi <= a or lo >= a+n:
                    remaining.append((lo, hi))
                else:
                    if lo < a:
                        remaining.append((lo, a))
                    if hi > a+n:
                        remaining.append((a+n, hi))
            spans = remaining
        coverage.append(dict(name=name, va=hex(begin), size=size,
                             outside_direct_snapshot=[dict(begin=hex(lo), end_exclusive=hex(hi), size=hi-lo) for lo, hi in spans]))
    report = dict(upstream_commit=COMMIT, local_layout_sha256=hashlib.sha256(layout_path.read_bytes()).hexdigest(),
                  limitation="FullSaveの実コピー範囲と固定保存ノードのみの静的比較。動的ノード・別経路保存・実行時変更は判定しない。保存漏れやデシンクの証明ではない。",
                  regions=coverage)
    (out/"snapshot_comparison.json").write_text(json.dumps(report, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")
    print(json.dumps(dict(structures=len(structs), fields=sum(len(s['fields']) for s in structs), checked_offsets=checks)))


if __name__ == "__main__":
    main()
