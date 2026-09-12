#include "core_dll/mbaa_mem/CombatStress.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/common/ScriptedInput.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstring>

extern "C" {
unsigned char cccaster_stress_active = 0;
void *cccaster_stress_original = nullptr;
uintptr_t cccaster_stress_continue = 0x45f4fb;
// 非ゼロの攻撃回数がアニメーションから設定される箇所だけを対象にする。
// EAXとフラグは保存。攻撃データ・判定形状・アニメーション寿命は変更しない。
__attribute__((naked)) void cccaster_stress_hook() {
    __asm__ __volatile__("pushfl\n\tcmpb $0,_cccaster_stress_active\n\tje 1f\n\t"
        "popfl\n\tmovb $255,0x176(%edi)\n\tjmp *_cccaster_stress_continue\n\t"
        "1: popfl\n\tjmp *_cccaster_stress_original\n\t");
}
}
namespace cccaster::testing::combat_stress {
namespace {
unsigned mode = 0;
uint32_t current = 0;
bool pending = false;
}
bool Install() {
    static bool attempted = false;
    if (attempted) return mode != 0;
    attempted = true;
    const char *v = std::getenv("CCCASTER_COMBAT_STRESS");
    if (!IsScriptedInputEnabled() || !v || (v[0] != '1' && v[0] != '2')) return false;
    auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(module + dos->e_lfanew);
    constexpr unsigned char setter[] = {0x8a,0x43,0x10,0x84,0xc0,0x74,0x0f,0x88,0x87,0x76,0x01,0,0};
    if (module != 0x400000 || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.TimeDateStamp != 0x4fe44444 || nt->OptionalHeader.SizeOfImage != 0x3b4000 ||
        std::memcmp(reinterpret_cast<void *>(0x45f4ee), setter, sizeof(setter))) return false;
    if (v[0] == '2') {
        auto r = MH_Initialize();
        if (r != MH_OK && r != MH_ERROR_ALREADY_INITIALIZED) return false;
        if (MH_CreateHook(reinterpret_cast<void *>(0x45f4f5), reinterpret_cast<void *>(cccaster_stress_hook),
                          &cccaster_stress_original) != MH_OK ||
            MH_EnableHook(reinterpret_cast<void *>(0x45f4f5)) != MH_OK) return false;
    }
    mode = unsigned(v[0] - '0');
    domain::session::DebugLog("[CombatStressInstall] mode=%u distance=12000 rehit=%d", mode, mode == 2);
    return true;
}
bool Active() { return mode && cccaster_stress_active; }
void Begin(bool combat, uint32_t frame) {
    cccaster_stress_active = mode && combat;
    if (!Active()) return;
    current = frame;
    pending = true;
    // 近距離へ毎回戻し、押し戻し・吹き飛びの蓄積を防ぐ。
    for (unsigned i = 0; i < 2; ++i) {
        auto base = reinterpret_cast<uintptr_t>(CC_P1_ENABLED_FLAG_ADDR) + i * CC_PLR_STRUCT_SIZE;
        const int32_t x = i ? 6000 : -6000;
        *reinterpret_cast<int32_t *>(base + 0x108) = x;
        *reinterpret_cast<int32_t *>(base + 0x114) = x;
        *reinterpret_cast<int32_t *>(base + 0x10c) = 0;
        *reinterpret_cast<int32_t *>(base + 0x118) = 0;
        *reinterpret_cast<int32_t *>(base + 0x11c) = 0;
        *reinterpret_cast<int32_t *>(base + 0x120) = 0;
        // KOで測定を打ち切らないため、試験中だけ体力を補充する。
        *reinterpret_cast<uint32_t *>(base + 0xbc) = 11400;
        *reinterpret_cast<uint32_t *>(base + 0xc0) = 11400;
    }
}
void Flush() {
    if (!pending) return;
    pending = false;
    domain::session::DebugLog("[CombatStress] f=%u mode=%u hp1=%u hp2=%u hits1=%u hits2=%u seq1=%u seq2=%u",
        current, mode, *CC_P1_HEALTH_ADDR, *CC_P2_HEALTH_ADDR,
        unsigned(*reinterpret_cast<uint8_t *>(0x5552aa)), unsigned(*reinterpret_cast<uint8_t *>(0x555da6)),
        *CC_P1_SEQUENCE_ADDR, *CC_P2_SEQUENCE_ADDR);
}
}
