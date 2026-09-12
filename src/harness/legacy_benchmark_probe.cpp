// 旧版/v10共通の実ゲーム計測器。ゲームのフレーム末尾だけを観測する。
// 固定長共有メモリへ数値を記録し、ゲーム状態・入力・速度設定は変更しない。
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "MinHook.h"
#include "shared_contracts/GameBuild.hpp"

struct Record {
    int64_t tick, end;
    uint32_t world, mode, real, round, skip, intro, ordinal, thread;
};
static_assert(sizeof(Record) == 48);
struct Header {
    uint32_t magic, version, count, capacity;
    int64_t frequency;
    uint32_t status, recordSize;
};
constexpr uint32_t capacity = 262144;
constexpr uint32_t mapSize = 4096 + capacity * sizeof(Record);
static Header *header;
static Record *records;
extern "C" { void *benchTrampoline = nullptr; }
extern "C" __attribute__((force_align_arg_pointer, noinline)) void benchCapture() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint32_t n = header->count;
    if (n >= capacity) { header->status = 3; return; }
    auto &r = records[n];
    r.tick = now.QuadPart;
    r.world = *reinterpret_cast<volatile uint32_t *>(0x55d1d4);
    r.mode = *reinterpret_cast<volatile uint32_t *>(0x54eee8);
    r.real = *reinterpret_cast<volatile uint32_t *>(0x562a40);
    r.round = *reinterpret_cast<volatile uint32_t *>(0x562a3c);
    r.skip = *reinterpret_cast<volatile uint32_t *>(0x55d25c); // 読取りのみ。
    r.intro = *reinterpret_cast<volatile uint8_t *>(0x55d20b);
    r.ordinal = n;
    r.thread = GetCurrentThreadId();
    QueryPerformanceCounter(&now);
    r.end = now.QuadPart;
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG *>(&header->count), n + 1);
}
extern "C" __attribute__((naked)) void benchGate() {
    asm volatile("pushfl\n\tpushal\n\tcall _benchCapture\n\tpopal\n\tpopfl\n\tjmp *_benchTrampoline");
}
static DWORD WINAPI initialize(void *) {
    char name[96];
    std::snprintf(name, sizeof(name), "Local\\CCCasterLegacyBench_%lu", GetCurrentProcessId());
    HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, mapSize, name);
    if (!mapping) return 1;
    header = static_cast<Header *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, mapSize));
    if (!header) return 2;
    // ページフォルトをゲーム計測ループに持ち込まない。
    std::memset(header, 0, mapSize);
    records = reinterpret_cast<Record *>(reinterpret_cast<char *>(header) + 4096);
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    *header = {0x42434343, 1, 0, capacity, frequency.QuadPart, 0, sizeof(Record)};
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    cccaster::game_build::PeIdentity identity;
    // 外部脚本で元EXEのSHA256を記録し、注入元で版を照合。
    // ここは実ロードヘッダーと命令を照合する。
    constexpr unsigned char expected[] = {0x83,0x3d,0x50,0xd2,0x55,0x00,0x00};
    if (base != 0x400000 ||
        !cccaster::game_build::ReadHeaders({reinterpret_cast<const uint8_t *>(base),4096},identity) ||
        cccaster::game_build::IdentifyHeaders(identity) != cccaster::game_build::Edition::Carnival140 ||
        std::memcmp(reinterpret_cast<void *>(0x433401),expected,sizeof(expected))) {
        header->status = 4; return 4;
    }
    if (MH_Initialize() != MH_OK ||
        MH_CreateHook(reinterpret_cast<void *>(0x433401),reinterpret_cast<void *>(&benchGate),&benchTrampoline) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void *>(0x433401)) != MH_OK) {
        header->status = 5; return 5;
    }
    header->status = 1;
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        HANDLE worker = CreateThread(nullptr,0,initialize,nullptr,0,nullptr);
        if (worker) CloseHandle(worker);
    }
    return TRUE;
}
