"""MBAA.exeの版識別と、Steam版候補アドレスの読取り専用観測。ゲームを起動・変更しない。"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import time

BUILDS = {
    "04b5bbd582fd795ea2fd27acb5beb2c4e958c6840b1054cda4cb0b70481d949d": "carnival_140",
    "6d1415ca9573100e86a779ac2f81e9bedd322664e3daeae0229a67d13720310a": "carnival_140_community",
    "11270cf2da851054aa6c1bff309d50b042429abd936dca5df833bcd1de5e6c46": "steam_20170105",
}

# 基準VA（ImageBase=0x400000）。実メモリにはロード差分を加える。
# この表は観測用。フック・書込み・ロールバック保存範囲を許可する表ではない。
STEAM_SYMBOLS = {
    "mode": (0x5B391C, 4), "pending_mode": (0x5CA9B4, 4),
    "world_timer": (0x5C4418, 4), "intro": (0x5CA79A, 1),
    "state": (0x7B40A4, 4), "input_pointer": (0x5CA9B8, 4),
    "rng_value": (0x5CA9BC, 4), "rng_count": (0x5CA9C0, 4),
    "rng_index": (0x5CB2B8, 4),
    "real_timer": (0x5C9C54, 4), "round_timer": (0x5C9C58, 4),
    "training_pause": (0x5C9C7C, 4), "dummy_status": (0x7B4324, 4),
    "p1_enabled": (0x5BC370, 1), "p1_sequence": (0x5BC380, 4),
    "p1_health": (0x5BC42C, 4), "p1_x": (0x5BC478, 4), "p1_y": (0x5BC47C, 4),
    "p2_enabled": (0x5BC370 + 0xAFC, 1), "p2_sequence": (0x5BC380 + 0xAFC, 4),
    "p2_health": (0x5BC42C + 0xAFC, 4), "p2_x": (0x5BC478 + 0xAFC, 4),
    "p2_y": (0x5BC47C + 0xAFC, 4),
    "camera_x": (0x5CB63C, 4), "camera_y": (0x5CB640, 4),
}


def inspect(path):
    data = Path(path).read_bytes()
    def unpack(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(data):
            raise ValueError("PEの範囲外です")
        return struct.unpack_from(fmt, data, offset)
    if data[:2] != b"MZ":
        raise ValueError("MZ形式ではありません")
    nt, = unpack("<I", 0x3C)
    if data[nt:nt+4] != b"PE\0\0":
        raise ValueError("PE署名がありません")
    machine, section_count, timestamp = unpack("<HHI", nt + 4)
    optional_size, = unpack("<H", nt + 20)
    optional = nt + 24
    magic, = unpack("<H", optional)
    if magic != 0x10B or machine != 0x14C or optional_size < 96 or not 1 <= section_count <= 96:
        raise ValueError("対象の32bit PE形式ではありません")
    entry, = unpack("<I", optional + 16)
    base, = unpack("<I", optional + 28)
    size, = unpack("<I", optional + 56)
    characteristics, = unpack("<H", optional + 70)
    sections = []
    for i in range(section_count):
        name, vsize, rva, raw_size, raw_offset = unpack("<8sIIII", optional + optional_size + 40*i)
        if rva + vsize > size or raw_offset + raw_size > len(data):
            raise ValueError("セクションの範囲が不正です")
        sections.append(dict(name=name.rstrip(b"\0").decode("ascii"), rva=rva,
                             virtual_size=vsize, raw_size=raw_size, raw_offset=raw_offset))
    digest = hashlib.sha256(data).hexdigest()
    return dict(path=str(Path(path).resolve()), edition=BUILDS.get(digest, "unknown"),
                sha256=digest, machine=machine, timestamp=timestamp, image_base=base,
                image_size=size, entry_rva=entry, aslr=bool(characteristics & 0x40), sections=sections)


class Reader:
    def __init__(self, pid, expected):
        if os.name != "nt":
            raise ValueError("実プロセス観測はWindows専用です")
        from ctypes import wintypes as w
        c = ctypes
        self.k = c.WinDLL("kernel32", use_last_error=True)
        self.k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.k.OpenProcess.restype = w.HANDLE
        self.k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
        self.k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
        self.k.CloseHandle.argtypes = [w.HANDLE]
        self.h = self.k.OpenProcess(0x410, False, pid)  # QUERY_INFORMATION | VM_READのみ
        if not self.h:
            raise c.WinError(c.get_last_error())
        try:
            path = c.create_unicode_buffer(32768)
            length = w.DWORD(len(path))
            if not self.k.QueryFullProcessImageNameW(self.h, 0, path, c.byref(length)):
                raise c.WinError(c.get_last_error())
            if os.path.normcase(os.path.realpath(path.value)) != os.path.normcase(expected["path"]):
                raise ValueError("PIDの実行ファイルが指定パスと一致しません")
            live = inspect(path.value)
            if live["sha256"] != expected["sha256"] or live["edition"] != "steam_20170105":
                raise ValueError("実プロセス観測は今回照合済みのSteam実行ファイルだけに限定します")
            psapi = c.WinDLL("psapi", use_last_error=True)
            psapi.EnumProcessModulesEx.argtypes = [w.HANDLE, c.POINTER(c.c_void_p), w.DWORD, c.POINTER(w.DWORD), w.DWORD]
            modules = (c.c_void_p * 1024)()
            needed = w.DWORD()
            if not psapi.EnumProcessModulesEx(self.h, modules, c.sizeof(modules), c.byref(needed), 3) or not needed.value:
                raise c.WinError(c.get_last_error())
            self.base = modules[0]
            if self.read(self.base, 2) != b"MZ":
                raise ValueError("ロード先の実行イメージを確認できません")
            self.pid = pid
        except BaseException:
            self.close()
            raise

    def close(self):
        if self.h:
            self.k.CloseHandle(self.h)
            self.h = None

    def read(self, address, size):
        data = ctypes.create_string_buffer(size)
        count = ctypes.c_size_t()
        if not self.k.ReadProcessMemory(self.h, address, data, size, ctypes.byref(count)) or count.value != size:
            raise ctypes.WinError(ctypes.get_last_error())
        return data.raw

    def sample(self):
        values = {name: int.from_bytes(self.read(self.base + va - 0x400000, size), "little")
                  for name, (va, size) in STEAM_SYMBOLS.items()}
        pointer = values["input_pointer"]
        if pointer:
            raw = self.read(pointer + 0x18, 0x24)
            for name, off, fmt in [("p1_direction", 0, "<I"), ("p1_buttons", 0xC, "<H"),
                                   ("p2_direction", 0x14, "<I"), ("p2_buttons", 0x20, "<H")]:
                values[name] = struct.unpack_from(fmt, raw, off)[0]
        values["rng_state_hex"] = self.read(self.base + 0x5CB2C0 - 0x400000, 220).hex()
        return dict(pid=self.pid, base=self.base, monotonic_ns=time.monotonic_ns(), values=values,
                    note="read-only asynchronous sample; not a coherent rollback snapshot")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--seconds", type=float, default=0)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--output", type=Path, help="新しいJSONLファイル。既存ファイルへ上書きしない")
    args = parser.parse_args()
    if not 0 <= args.seconds <= 300 or not 0.01 <= args.interval <= 10:
        parser.error("secondsは0〜300、intervalは0.01〜10")
    info = inspect(args.exe)
    output = args.output.open("x", encoding="utf-8") if args.output else None
    def emit(value):
        line = json.dumps(value, ensure_ascii=False)
        if output:
            print(line, file=output, flush=True)
        else:
            print(line, flush=True)
    try:
        emit(info)
        if args.pid is not None:
            reader = Reader(args.pid, info)
            try:
                end = time.monotonic() + args.seconds
                while True:
                    emit(reader.sample())
                    if time.monotonic() >= end:
                        break
                    time.sleep(min(args.interval, max(0, end - time.monotonic())))
            finally:
                reader.close()
    finally:
        if output:
            output.close()


if __name__ == "__main__":
    main()
