#include "core_dll/timing/ClockProjection.hpp"
#include <mutex>
#include <cstdlib>
#include "core_dll/hook/TimeHooks.hpp"
#include <vector>
#include <cstring>
#include "core_dll/common/DebugLog.hpp"
#include <atomic>
#include <iostream>

#pragma comment(lib, "winmm.lib")

namespace cccaster::core::hooks {

bool TimeHooks::s_initialized = false;
std::atomic<uint32_t> TimeHooks::s_multiplier{1};
std::atomic<bool> TimeHooks::s_sleepBypass{false};

// Time Hack State
static std::atomic<LONGLONG> g_addedQPC{0};
static std::atomic<DWORD> g_addedGTC{0};
static std::atomic<DWORD> g_addedTGT{0};

static LARGE_INTEGER g_prevQPC{0};
static DWORD g_prevGTC = 0;

// Function pointers for original APIs
typedef void(WINAPI *Sleep_t)(DWORD);
static Sleep_t pOrigSleep = nullptr;

typedef BOOL(WINAPI *QueryPerformanceCounter_t)(LARGE_INTEGER *);
static QueryPerformanceCounter_t pOrigQPC = nullptr;

typedef DWORD(WINAPI *GetTickCount_t)(VOID);
static GetTickCount_t pOrigGTC = nullptr;

typedef DWORD(WINAPI *timeGetTime_t)(VOID);
static timeGetTime_t pOrigTGT = nullptr;

// =========================================================
// Hooked Functions
// =========================================================

// ゲーム用仮想時計だけをQPCから直接投影する。入力/WASAPI/通信の時計には適用しない。
static timer::ClockProjection gameProjection;
static std::mutex gameProjectionWriter;
static std::atomic<bool> gameProjectionReady{false};
static int64_t gameOriginQpc = 0, gameQpcHz = 1;
static DWORD gameOriginGtc = 0, gameOriginTgt = 0;
static bool legacyGameTimer = false;
static int64_t ProjectGameQpc(int64_t real) {
    thread_local timer::ClockAnchor cached;
    gameProjection.Read(cached);
    return cached.AtTicks(real);
}
static DWORD ProjectGameMilliseconds(DWORD origin) {
    LARGE_INTEGER real{};
    pOrigQPC(&real);
    const auto elapsed = std::max<int64_t>(0, ProjectGameQpc(real.QuadPart) - gameOriginQpc);
    return origin + static_cast<DWORD>(elapsed / gameQpcHz * 1000 + elapsed % gameQpcHz * 1000 / gameQpcHz);
}
void WINAPI Hooked_Sleep(DWORD dwMilliseconds) {
    if (TimeHooks::s_sleepBypass) {
        if (!legacyGameTimer) return; // Sleep(0)もOSへ譲るため、無効化中は呼ばない。
        if (pOrigSleep)
            pOrigSleep(0);
        else
            Sleep(0);
        return;
    }
    if (pOrigSleep)
        pOrigSleep(dwMilliseconds);
    else
        Sleep(dwMilliseconds);
}

BOOL WINAPI Hooked_QueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount) {
    BOOL ret = pOrigQPC ? pOrigQPC(lpPerformanceCount) : QueryPerformanceCounter(lpPerformanceCount);
    if (ret) {
        if (gameProjectionReady.load(std::memory_order_acquire))
            lpPerformanceCount->QuadPart = ProjectGameQpc(lpPerformanceCount->QuadPart);
        else
            lpPerformanceCount->QuadPart += g_addedQPC.load(std::memory_order_relaxed);
    }
    return ret;
}

DWORD WINAPI Hooked_GetTickCount(VOID) {
    if (gameProjectionReady.load(std::memory_order_acquire)) return ProjectGameMilliseconds(gameOriginGtc);
    DWORD ret = pOrigGTC ? pOrigGTC() : GetTickCount();
    return ret + g_addedGTC.load(std::memory_order_relaxed);
}

DWORD WINAPI Hooked_timeGetTime(VOID) {
    if (gameProjectionReady.load(std::memory_order_acquire)) return ProjectGameMilliseconds(gameOriginTgt);
    DWORD ret = pOrigTGT ? pOrigTGT() : timeGetTime();
    return ret + g_addedTGT.load(std::memory_order_relaxed);
}

// =========================================================
// Core Update Thread (To maintain monotonic increasing time)
// =========================================================
static std::atomic<bool> g_timeThreadRunning{false};
static HANDLE g_hTimeThread = nullptr;

static DWORD WINAPI TimeUpdateThread(LPVOID) {
    timeBeginPeriod(1);

    while (g_timeThreadRunning) {
        LARGE_INTEGER nowQPC;
        if (pOrigQPC)
            pOrigQPC(&nowQPC);
        else
            QueryPerformanceCounter(&nowQPC);
        DWORD nowGTC;
        if (pOrigGTC)
            nowGTC = pOrigGTC();
        else
            nowGTC = GetTickCount();

        LONGLONG elapsedQPC = nowQPC.QuadPart - g_prevQPC.QuadPart;
        DWORD elapsedGTC = nowGTC - g_prevGTC;

        g_prevQPC = nowQPC;
        g_prevGTC = nowGTC;

        uint32_t currentMult = TimeHooks::s_multiplier;
        if (currentMult > 1) {
            g_addedQPC.fetch_add(elapsedQPC * (currentMult - 1), std::memory_order_relaxed);
            g_addedGTC.fetch_add(elapsedGTC * (currentMult - 1), std::memory_order_relaxed);
            g_addedTGT.fetch_add(elapsedGTC * (currentMult - 1), std::memory_order_relaxed);
        }

        if (pOrigSleep)
            pOrigSleep(1);
        else
            Sleep(1);
    }
    timeEndPeriod(1);
    return 0;
}

// =========================================================
// Public Methods
// =========================================================

namespace {
struct ImportPatch {
    DWORD *slot;
    DWORD original;
    DWORD replacement;
};
std::vector<ImportPatch> imports;
bool ReplaceImport(DWORD *slot, DWORD replacement) {
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
        return false;
    const DWORD old = *slot;
    *slot = replacement;
    DWORD ignored;
    VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    imports.push_back({slot, old, replacement});
    return true;
}
void RestoreImports() {
    for (auto p : imports) {
        DWORD protection = 0;
        if (*p.slot == p.replacement && VirtualProtect(p.slot, 4, PAGE_READWRITE, &protection)) {
            *p.slot = p.original;
            DWORD ignored;
            VirtualProtect(p.slot, 4, protection, &ignored);
        }
    }
    imports.clear();
}
} // namespace
void TimeHooks::Initialize() {
    if (s_initialized)
        return;
    auto kernel = GetModuleHandleA("kernel32.dll");
    auto winmm = GetModuleHandleA("winmm.dll");
    if (!winmm)
        winmm = LoadLibraryA("winmm.dll");
    pOrigSleep = reinterpret_cast<Sleep_t>(GetProcAddress(kernel, "Sleep"));
    pOrigQPC = reinterpret_cast<QueryPerformanceCounter_t>(GetProcAddress(kernel, "QueryPerformanceCounter"));
    pOrigGTC = reinterpret_cast<GetTickCount_t>(GetProcAddress(kernel, "GetTickCount"));
    pOrigTGT = winmm ? reinterpret_cast<timeGetTime_t>(GetProcAddress(winmm, "timeGetTime")) : nullptr;
    if (!pOrigSleep || !pOrigQPC || !pOrigGTC || !pOrigTGT)
        return;
    auto base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return;
    const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress)
        return;
    unsigned found = 0;
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress); d->Name;
         ++d) {
        if (!d->OriginalFirstThunk)
            continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA32 *>(base + d->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA32 *>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal))
                continue;
            const auto name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData)->Name;
            DWORD target = 0;
            unsigned bit = 0;
            if (!std::strcmp(name, "Sleep")) {
                target = reinterpret_cast<DWORD>(&Hooked_Sleep);
                bit = 1;
            } else if (!std::strcmp(name, "QueryPerformanceCounter")) {
                target = reinterpret_cast<DWORD>(&Hooked_QueryPerformanceCounter);
                bit = 2;
            } else if (!std::strcmp(name, "GetTickCount")) {
                target = reinterpret_cast<DWORD>(&Hooked_GetTickCount);
                bit = 4;
            } else if (!std::strcmp(name, "timeGetTime")) {
                target = reinterpret_cast<DWORD>(&Hooked_timeGetTime);
                bit = 8;
            }
            if (target && ReplaceImport(&slots->u1.Function, target))
                found |= bit;
        }
    }
    if (found != 15) {
        RestoreImports();
        cccaster::domain::session::DebugLog("[TimeHooks] FAILED game IAT mask=%u", found);
        return;
    }
    pOrigQPC(&g_prevQPC);
    g_prevGTC = pOrigGTC();
    legacyGameTimer = std::getenv("CCCASTER_LEGACY_GAME_TIMER") != nullptr;
    if (legacyGameTimer) {
        g_timeThreadRunning = true;
        g_hTimeThread = CreateThread(nullptr, 0, TimeUpdateThread, nullptr, 0, nullptr);
        if (!g_hTimeThread) {
            g_timeThreadRunning = false;
            RestoreImports();
            return;
        }
    } else {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        gameQpcHz = frequency.QuadPart;
        gameOriginQpc = g_prevQPC.QuadPart;
        gameOriginGtc = g_prevGTC;
        gameOriginTgt = pOrigTGT();
        gameProjection.Publish({gameOriginQpc, gameOriginQpc, 0,
                                (int64_t(s_multiplier.load()) - 1) * 1000000});
        gameProjectionReady.store(true, std::memory_order_release);
    }
    cccaster::domain::session::DebugLog("[GameTimer] directProjection=%d sleepYield=%d", !legacyGameTimer, legacyGameTimer);
    s_initialized = true;
    cccaster::domain::session::DebugLog(
        "[TimeHooks] Game IAT only: Sleep/QPC/GetTickCount/timeGetTime. DLL clocks untouched.");
}
void TimeHooks::Shutdown() {
    if (!s_initialized)
        return;
    g_timeThreadRunning = false;
    if (g_hTimeThread) {
        WaitForSingleObject(g_hTimeThread, 1000);
        CloseHandle(g_hTimeThread);
        g_hTimeThread = nullptr;
    }
    RestoreImports();
    gameProjectionReady.store(false, std::memory_order_release);
    s_initialized = false;
}

void TimeHooks::SetTimeMultiplier(uint32_t multiplier) {
    if (multiplier == 0)
        multiplier = 1;
    if (s_multiplier.load(std::memory_order_acquire) == multiplier) return;
    std::lock_guard lock(gameProjectionWriter);
    if (gameProjectionReady.load(std::memory_order_acquire)) {
        LARGE_INTEGER real{};
        pOrigQPC(&real);
        timer::ClockAnchor old;
        gameProjection.Read(old);
        gameProjection.Publish({real.QuadPart, old.AtTicks(real.QuadPart), 0,
                                (int64_t(multiplier) - 1) * 1000000});
    }
    s_multiplier.store(multiplier, std::memory_order_release);
}

void TimeHooks::SetSleepBypass(bool bypass) {
    s_sleepBypass = bypass;
}

void TimeHooks::RealQueryPerformanceCounter(LARGE_INTEGER *lpPerformanceCount) {
    if (pOrigQPC) {
        pOrigQPC(lpPerformanceCount);
    } else {
        QueryPerformanceCounter(lpPerformanceCount);
    }
}

DWORD TimeHooks::RealGetTickCount() {
    return pOrigGTC ? pOrigGTC() : GetTickCount();
}

DWORD TimeHooks::RealTimeGetTime() {
    return pOrigTGT ? pOrigTGT() : timeGetTime();
}

void TimeHooks::RealSleep(DWORD dwMilliseconds) {
    if (pOrigSleep) {
        pOrigSleep(dwMilliseconds);
    } else {
        Sleep(dwMilliseconds);
    }
}

} // namespace cccaster::core::hooks
