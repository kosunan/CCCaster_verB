#pragma once
#include <windows.h>
#include <dsound.h>
#include <MinHook.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"

namespace cccaster::diagnostics::sound_api {
inline bool Enabled() {
    static const bool enabled = std::getenv("CCCASTER_SOUND_API_PROBE") != nullptr;
    return enabled;
}
inline std::array<std::array<void *, 4>, 21> targets{}, originals{};
inline const char *Name(unsigned i) {
    switch (i) {
    case 9: return "GetStatus"; case 11: return "Lock"; case 12: return "Play";
    case 13: return "SetCurrentPosition"; case 15: return "SetVolume";
    case 16: return "SetPan"; case 18: return "Stop"; case 19: return "Unlock";
    case 20: return "Restore"; default: return "unknown";
    }
}
struct Entry {
    int64_t begin, end;
    uint32_t serial, frame, pid, tid, sound, api, depth, caller, buffer, result, a, b, c, status;
    bool replay;
};
inline std::array<Entry, 2048> entries{};
inline unsigned used = 0, drops = 0, depth = 0, serial = 0;
inline uint32_t frame = 0, process = 0, sound = 1500;
inline std::atomic<uint32_t> thread{0};
inline bool active = false, replay = false, attempted = false;
using WaitFn = DWORD (WINAPI *)(DWORD, const HANDLE *, BOOL, DWORD);
inline WaitFn originalWait = nullptr;
inline HMODULE soundModule = nullptr;
inline void *ImportedWait() {
    if (!soundModule) return nullptr;
    auto base = reinterpret_cast<uintptr_t>(soundModule);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return nullptr;
    auto imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    for (; imports->Name; ++imports) {
        if (!imports->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imports->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto item = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (!std::strcmp(reinterpret_cast<const char *>(item->Name), "WaitForMultipleObjects"))
                return reinterpret_cast<void *>(slots->u1.Function);
        }
    }
    return nullptr;
}
__attribute__((force_align_arg_pointer)) inline DWORD WINAPI Wait(DWORD count, const HANDLE *handles, BOOL all, DWORD timeout) {
    if (GetCurrentThreadId() != thread.load(std::memory_order_relaxed) || !active || !depth)
        return originalWait(count, handles, all, timeout);
    Entry e{};
    e.serial = serial; e.frame = frame; e.pid = process; e.tid = thread.load(std::memory_order_relaxed);
    e.sound = sound; e.api = 21; e.depth = depth++; e.replay = replay;
    e.caller = uint32_t(uintptr_t(__builtin_return_address(0)));
    e.a = count; e.b = uint32_t(all); e.c = timeout;
    e.buffer = count && handles ? uint32_t(uintptr_t(handles[0])) : 0;
    e.begin = platform::RealMonotonicTicks();
    const auto result = originalWait(count, handles, all, timeout);
    e.end = platform::RealMonotonicTicks(); e.result = result;
    --depth;
    if (used < entries.size()) entries[used++] = e; else ++drops;
    return result;
}

template<unsigned Index, unsigned Slot, class... Args>
__attribute__((force_align_arg_pointer)) HRESULT WINAPI Call(IDirectSoundBuffer *buffer, Args... args) {
    using Fn = HRESULT (WINAPI *)(IDirectSoundBuffer *, Args...);
    const auto original = reinterpret_cast<Fn>(originals[Index][Slot]);
    if (GetCurrentThreadId() != thread.load(std::memory_order_relaxed) || !active) return original(buffer, args...);
    uint32_t values[] = {uint32_t(uintptr_t(args))..., 0, 0, 0};
    Entry e{};
    e.serial = serial; e.frame = frame; e.pid = process; e.tid = thread.load(std::memory_order_relaxed);
    e.sound = sound; e.api = Index; e.depth = depth++;
    e.caller = uint32_t(uintptr_t(__builtin_return_address(0)));
    e.buffer = uint32_t(uintptr_t(buffer)); e.replay = replay;
    e.a = values[0]; e.b = values[1]; e.c = values[2];
    e.begin = platform::RealMonotonicTicks();
    const HRESULT result = original(buffer, args...);
    e.end = platform::RealMonotonicTicks();
    e.result = uint32_t(result);
    if constexpr (Index == 9) {
        if (SUCCEEDED(result) && e.a) e.status = *reinterpret_cast<DWORD *>(e.a);
    }
    --depth;
    if (used < entries.size()) entries[used++] = e;
    else ++drops;
    return result;
}
template<unsigned Index, class... Args>
void Install(void **table) {
    void *address = table[Index];
    for (auto target : targets[Index]) if (target == address) return;
    unsigned slot = 0;
    while (slot < 4 && targets[Index][slot]) ++slot;
    if (slot == 4) return;
    void *wrappers[] = {reinterpret_cast<void *>(&Call<Index, 0, Args...>),
        reinterpret_cast<void *>(&Call<Index, 1, Args...>), reinterpret_cast<void *>(&Call<Index, 2, Args...>),
        reinterpret_cast<void *>(&Call<Index, 3, Args...>)};
    const auto created = MH_CreateHook(address, wrappers[slot], &originals[Index][slot]);
    const auto enabled = created == MH_OK ? MH_EnableHook(address) : created;
    // 失敗したアドレスも記録し、同じ共有実装への再試行を繰り返さない。
    targets[Index][slot] = address;
    HMODULE module = nullptr;
    char path[MAX_PATH]{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCSTR>(address), &module);
    if (module) GetModuleFileNameA(module, path, MAX_PATH);
    if constexpr (Index == 12) soundModule = module;
    domain::session::DebugLog("[SoundApiHook] api=%s slot=%u target=%08X module=%s rva=%08X create=%d enable=%d",
        Name(Index), slot, unsigned(uintptr_t(address)), path,
        unsigned(uintptr_t(address) - uintptr_t(module)), int(created), int(enabled));
}
inline void Prepare() {
    if (!Enabled() || attempted) return;
    // 初期化は親の効果音計測区間より前。未生成なら後の更新で再試行。
    unsigned buffers = 0;
    auto sounds = reinterpret_cast<uintptr_t *>(0x76c6f8);
    for (unsigned i = 0; i < 1500; ++i) {
        auto obj = sounds[i];
        if (!obj) continue;
        auto list = *reinterpret_cast<IDirectSoundBuffer ***>(obj + 4);
        auto count = *reinterpret_cast<int *>(obj + 0x10);
        if (!list || count < 1 || count > 64) continue;
        for (int j = 0; j < count; ++j) {
            if (!list[j]) continue;
            auto table = *reinterpret_cast<void ***>(list[j]);
            Install<9, DWORD *>(table);
            Install<11, DWORD, DWORD, void **, DWORD *, void **, DWORD *, DWORD>(table);
            Install<12, DWORD, DWORD, DWORD>(table);
            Install<13, DWORD>(table);
            Install<15, LONG>(table);
            Install<16, LONG>(table);
            Install<18>(table);
            Install<19, void *, DWORD, void *, DWORD>(table);
            Install<20>(table);
            ++buffers;
        }
    }
    if (buffers) {
        attempted = true;
        auto address = ImportedWait();
        const auto created = MH_CreateHook(address, reinterpret_cast<void *>(Wait), reinterpret_cast<void **>(&originalWait));
        const auto enabled = created == MH_OK ? MH_EnableHook(address) : created;
        domain::session::DebugLog("[SoundApiWaitHook] target=%08X create=%d enable=%d", unsigned(uintptr_t(address)), int(created), int(enabled));
        entries.fill({});
        process = GetCurrentProcessId(); thread = GetCurrentThreadId();
        domain::session::DebugLog("[SoundApiReady] buffers=%u pid=%u tid=%u", buffers, process, thread.load());
    }
}
inline void Begin(uint32_t f, bool r) {
    if (!Enabled()) return;
    ++serial; frame = f; replay = r; sound = 1500; active = true;
}
inline void End() { active = false; }
inline void SetSound(uint32_t s) { if (active) sound = s; }
inline void Flush() {
    for (unsigned i = 0; i < used; ++i) {
        const auto &e = entries[i];
        domain::session::DebugLog("[SoundApi] seq=%u f=%u replay=%d sound=%u api=%s depth=%u caller=%u buffer=%u result=%u a=%u b=%u c=%u status=%u begin=%lld end=%lld pid=%u tid=%u",
            e.serial, e.frame, e.replay, e.sound, e.api == 21 ? "WaitForMultipleObjects" : Name(e.api), e.depth, e.caller, e.buffer,
            e.result, e.a, e.b, e.c, e.status, e.begin, e.end, e.pid, e.tid);
    }
    if (drops) domain::session::DebugLog("[SoundApiDrop] count=%u", drops);
    used = drops = 0;
}
}
