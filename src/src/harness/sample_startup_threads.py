"""明示したテストMBAAの主スレッドだけを短時間採取する（起動・終了なし）。

スタック候補はアンワインド結果ではなく、ゲーム実行セクション内を指すDWORD。
Suspendによる観測負荷を含むため、この実行を起動時間の基準値にしない。
"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import struct
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", required=True, type=int)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=5.0)
    args = parser.parse_args()
    if sys.platform != "win32" or C.sizeof(C.c_void_p) != 8:
        parser.error("64bit Windows Pythonで実行してください")
    if not 0 < args.seconds <= 5 or args.pid <= 0:
        parser.error("pidは正数、secondsは0より大きく5以下にしてください")
    k = C.WinDLL("kernel32", use_last_error=True)
    H = W.HANDLE
    D = W.DWORD
    P = C.c_void_p
    Z = C.c_size_t

    class ThreadEntry(C.Structure):
        _fields_ = [("size", D), ("usage", D), ("tid", D), ("pid", D),
                    ("base", W.LONG), ("delta", W.LONG), ("flags", D)]

    class ModuleEntry(C.Structure):
        _fields_ = [("size", D), ("id", D), ("pid", D), ("globalUsage", D),
                    ("usage", D), ("base", P), ("length", D), ("module", H),
                    ("name", W.WCHAR * 256), ("path", W.WCHAR * 260)]

    class Context(C.Structure):
        _fields_ = [("flags", D), ("debug", D * 6), ("floatSave", C.c_byte * 112),
                    ("segGs", D), ("segFs", D), ("segEs", D), ("segDs", D),
                    ("edi", D), ("esi", D), ("ebx", D), ("edx", D), ("ecx", D),
                    ("eax", D), ("ebp", D), ("eip", D), ("segCs", D), ("eflags", D),
                    ("esp", D), ("segSs", D), ("extended", C.c_byte * 512)]

    if C.sizeof(Context) != 716 or Context.eip.offset != 184 or Context.esp.offset != 196:
        raise RuntimeError("WOW64_CONTEXT layout mismatch")

    signatures = {
        "OpenProcess": ([D, W.BOOL, D], H), "OpenThread": ([D, W.BOOL, D], H),
        "CloseHandle": ([H], W.BOOL), "CreateToolhelp32Snapshot": ([D, D], H),
        "Thread32First": ([H, C.POINTER(ThreadEntry)], W.BOOL),
        "Thread32Next": ([H, C.POINTER(ThreadEntry)], W.BOOL),
        "Module32FirstW": ([H, C.POINTER(ModuleEntry)], W.BOOL),
        "Module32NextW": ([H, C.POINTER(ModuleEntry)], W.BOOL),
        "GetThreadTimes": ([H] + [C.POINTER(W.FILETIME)] * 4, W.BOOL),
        "QueryFullProcessImageNameW": ([H, D, W.LPWSTR, C.POINTER(D)], W.BOOL),
        "IsWow64Process": ([H, C.POINTER(W.BOOL)], W.BOOL),
        "ReadProcessMemory": ([H, P, P, Z, C.POINTER(Z)], W.BOOL),
        "SuspendThread": ([H], D), "ResumeThread": ([H], D),
        "Wow64GetThreadContext": ([H, C.POINTER(Context)], W.BOOL),
        "WaitForSingleObject": ([H, D], D),
    }
    for name, (params, result) in signatures.items():
        fn = getattr(k, name)
        fn.argtypes, fn.restype = params, result

    def checked(value):
        if not value or value == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        return value

    process = checked(k.OpenProcess(0x1000 | 0x10 | 0x100000, False, args.pid))
    thread = None
    report = {"pid": args.pid, "interval_ms": 50, "samples": [],
              "note": "stack_candidatesは戻りアドレス未検証。採取時のSuspend負荷あり。"}
    try:
        path_buffer = C.create_unicode_buffer(32768)
        size = D(len(path_buffer))
        checked(k.QueryFullProcessImageNameW(process, 0, path_buffer, C.byref(size)))
        path = Path(path_buffer.value)
        if path.name.lower() != "mbaa.exe" or "_test_mbaacc" not in [p.lower() for p in path.parts]:
            raise ValueError("対象は_TEST_MBAACC配下のMBAA.exeだけです")
        wow = W.BOOL()
        checked(k.IsWow64Process(process, C.byref(wow)))
        if not wow.value:
            raise ValueError("32bit WOW64プロセスではありません")
        report["path"] = str(path)
        snapshot = checked(k.CreateToolhelp32Snapshot(0x4, 0))
        oldest = None
        try:
            entry = ThreadEntry(size=C.sizeof(ThreadEntry))
            more = k.Thread32First(snapshot, C.byref(entry))
            while more:
                if entry.pid == args.pid:
                    candidate = checked(k.OpenThread(0x40 | 0x8 | 0x2, False, entry.tid))
                    try:
                        times = [W.FILETIME() for _ in range(4)]
                        checked(k.GetThreadTimes(candidate, *[C.byref(t) for t in times]))
                        created = (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime
                        if oldest is None or (created, entry.tid) < oldest:
                            if thread:
                                k.CloseHandle(thread)
                            thread, candidate = candidate, None
                            oldest = (created, entry.tid)
                    finally:
                        if candidate:
                            k.CloseHandle(candidate)
                more = k.Thread32Next(snapshot, C.byref(entry))
        finally:
            k.CloseHandle(snapshot)
        if thread is None:
            raise ValueError("対象スレッドがありません")
        report["thread_id"] = oldest[1]
        report["thread_creation_filetime"] = oldest[0]
        snapshot = checked(k.CreateToolhelp32Snapshot(0x8 | 0x10, args.pid))
        report["modules"] = []
        try:
            module = ModuleEntry(size=C.sizeof(ModuleEntry))
            more = k.Module32FirstW(snapshot, C.byref(module))
            while more:
                report["modules"].append({"name": module.name, "path": module.path,
                                          "base": module.base, "size": module.length})
                more = k.Module32NextW(snapshot, C.byref(module))
        finally:
            k.CloseHandle(snapshot)
        game = next(m for m in report["modules"] if m["name"].lower() == "mbaa.exe")
        # PEの実行可能セクションをファイルから読む。固定アドレスは仮定しない。
        with path.open("rb") as source:
            dos = source.read(64)
            if dos[:2] != b"MZ":
                raise ValueError("DOS signature mismatch")
            source.seek(struct.unpack_from("<I", dos, 60)[0])
            pe = source.read(24)
            machine, sections = struct.unpack_from("<HH", pe, 4)
            if pe[:4] != b"PE\0\0" or machine != 0x14c:
                raise ValueError("PE32 i386 signature mismatch")
            source.seek(struct.unpack_from("<H", pe, 20)[0], 1)
            ranges = []
            for _ in range(sections):
                sec = source.read(40)
                length, rva = struct.unpack_from("<II", sec, 8)
                if struct.unpack_from("<I", sec, 36)[0] & 0x20000000:
                    ranges.append((game["base"] + rva, game["base"] + rva + length))
        report["game_code_ranges"] = ranges
        started = time.perf_counter()
        report["started_perf_counter_ns"] = time.perf_counter_ns()
        report["started_unix_ns"] = time.time_ns()
        deadline = started + args.seconds
        next_sample = started
        while time.perf_counter() < deadline:
            if k.WaitForSingleObject(process, 0) == 0:
                report["process_exited"] = True
                break
            sample = {"elapsed_ms": (time.perf_counter() - started) * 1000,
                      "perf_counter_ns": time.perf_counter_ns()}
            suspended = False
            begin = time.perf_counter()
            try:
                previous = k.SuspendThread(thread)
                if previous == 0xffffffff:
                    raise C.WinError(C.get_last_error())
                suspended = True
                sample["previous_suspend_count"] = previous
                ctx = Context(flags=0x10001)
                checked(k.Wow64GetThreadContext(thread, C.byref(ctx)))
                sample.update(eip=ctx.eip, esp=ctx.esp)
                stack = (D * 128)()
                count = Z()
                ok = k.ReadProcessMemory(process, ctx.esp, stack, C.sizeof(stack), C.byref(count))
                sample["stack_bytes_read"] = count.value
                if not ok:
                    sample["stack_read_error"] = C.get_last_error()
                sample["stack_candidates"] = [
                    {"offset": i * 4, "address": stack[i]}
                    for i in range(min(128, count.value // 4))
                    if any(lo <= stack[i] < hi for lo, hi in ranges)]
            except OSError as error:
                sample["error"] = str(error)
            finally:
                if suspended:
                    resumed = k.ResumeThread(thread)
                    if resumed == 0xffffffff:
                        sample["resume_error"] = C.get_last_error()
                sample["pause_ms"] = (time.perf_counter() - begin) * 1000
            report["samples"].append(sample)
            if "resume_error" in sample or "error" in sample:
                break
            # 遅い採取のあとに連続Suspendして追いつこうとはしない。
            next_sample = max(next_sample + 0.05, begin + 0.05)
            time.sleep(max(0, min(next_sample, deadline) - time.perf_counter()))
    finally:
        if thread:
            k.CloseHandle(thread)
        k.CloseHandle(process)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
