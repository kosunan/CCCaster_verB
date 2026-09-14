"""両MBAA実行ファイルの命令列から移植調査用候補を出力。実行時パッチ表は生成しない。

解析依存: pefile、capstone。候補はレジスター割当と絶対参照を正規化した連続命令一致。
同じ処理／データ型／書込み可能性を証明するものではない。逆アセンブルと実測で再確認する。
"""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re
from inspect_mbaa_build import inspect


def instructions(path, relaxed):
    import capstone
    import pefile
    pe = pefile.PE(str(path))
    section = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.skipdata = True
    rows = []
    for i in decoder.disasm(section.get_data()[:section.Misc_VirtualSize],
                           pe.OPTIONAL_HEADER.ImageBase + section.VirtualAddress):
        assembly = f"{i.mnemonic} {i.op_str}"
        key = re.sub(r"0x[0-9a-f]{6,}", "@", assembly)
        if relaxed:
            key = re.sub(r"\b(eax|ecx|edx|ebx|esi|edi|ebp)\b", "reg", key)
        rows.append((i.address, assembly, key))
    return rows


def compare(old, new, window):
    index = {}
    for j in range(len(new) - window + 1):
        key = tuple(row[2] for row in new[j:j+window])
        index[key] = j if key not in index else None
    # 重複・相反する対応を上書きして確定扱いにしない。
    instruction_candidates = defaultdict(set)
    for i in range(len(old) - window + 1):
        j = index.get(tuple(row[2] for row in old[i:i+window]))
        if j is not None:
            for k in range(window):
                instruction_candidates[i+k].add(j+k)
    votes = defaultdict(Counter)
    evidence = defaultdict(list)
    for i, destinations in instruction_candidates.items():
        if len(destinations) != 1:
            continue
        j, = destinations
        left, right = old[i], new[j]
        a = re.findall(r"0x[0-9a-f]{6,}", left[1])
        b = re.findall(r"0x[0-9a-f]{6,}", right[1])
        if len(a) != len(b):
            continue
        for x, y in zip(a, b):
            votes[x][y] += 1
            evidence[x, y].append(dict(old_va=hex(left[0]), new_va=hex(right[0]),
                                       old_instruction=left[1], new_instruction=right[1]))
    return dict(
        instruction_candidates={hex(old[i][0]): [hex(new[j][0]) for j in sorted(js)]
                                for i, js in instruction_candidates.items()},
        operand_candidates={x: [dict(address=y, count=n, evidence=evidence[x, y])
                                for y, n in counts.most_common()] for x, counts in votes.items()})


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("legacy", type=Path)
    p.add_argument("steam", type=Path)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--window", type=int, default=12)
    p.add_argument("--relaxed-registers", action="store_true")
    args = p.parse_args()
    if not 5 <= args.window <= 64:
        p.error("windowは5〜64")
    old_info, new_info = inspect(args.legacy), inspect(args.steam)
    if old_info["edition"] != "carnival_140" or new_info["edition"] != "steam_20170105":
        p.error("今回SHA256を照合した2版だけが対象です")
    result = compare(instructions(args.legacy, args.relaxed_registers),
                     instructions(args.steam, args.relaxed_registers), args.window)
    result.update(legacy=old_info, steam=new_info, window=args.window,
                  relaxed_registers=args.relaxed_registers, runtime_approved=False)
    with args.output.open("x", encoding="utf-8") as out:
        json.dump(result, out, ensure_ascii=False, indent=2)
    print(f"候補命令: {len(result['instruction_candidates'])} / 参照候補: {len(result['operand_candidates'])}")


if __name__ == "__main__":
    main()
