#include "StartupDirectEntry.hpp"
#include "core_dll/mbaa_mem/StartupPatch.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "shared_contracts/ProcessMemory.hpp"
#include <cstdlib>

namespace {
uint64_t Hash(uintptr_t address, size_t size) {
    uint64_t value = 14695981039346656037ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(address);
    for (size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 1099511628211ull;
    return value;
}
enum class EntryMode { Versus, Training, Replay, CPU };
bool MenuBoundary() {
    return *reinterpret_cast<const uint32_t*>(0x55D1D0) == 25 &&
        *reinterpret_cast<const uint8_t*>(0x55DEC3) == 1 &&
        *reinterpret_cast<const uint32_t*>(0x76E6D4) == 0;
}
bool Compatible(EntryMode mode) {
    constexpr uint8_t menuReset[]{0xC7,0x05,0xF4,0x85,0x55,0,0,0,0,0,0xE8,0xA0,0xFE,0xFF,0xFF};
    constexpr uint8_t cpuEntry[]{0x0F,0xB6,0x05,0x0F,0xDF,0x55,0,0x50,0xE8,0x20,0xED,0xFF,0xFF,0xEB,0x37};
    return cccaster::game_memory::startup::MatchesMenuCode() &&
        (mode == EntryMode::Replay
            ? Hash(0x42A300, 76) == 0xc2f50ab875389d2aull &&
              Hash(0x42B541, 42) == 0xd941fede749860b6ull // ReplayVS初期化・mode26予約・復帰
            : mode == EntryMode::CPU ? Hash(0x42A200, 83) == 0xb1676b4d15034537ull &&
              std::memcmp(reinterpret_cast<void*>(0x42B4D3),cpuEntry,sizeof(cpuEntry)) == 0
            : mode == EntryMode::Training ? Hash(0x42A160, 70) == 0xada8a411ba956e3eull
                                         : Hash(0x42A1B0, 76) == 0x6718fca7d61c3c99ull) &&
        Hash(0x42B6B0, 302) == 0x860cf44ca2ba6a75ull && // 旧シーンの解放・共通リセット
        Hash(0x42B870, 15) == 0xbd7777fe3cd3c94cull &&
        Hash(0x42B519, 26) == 0x9cd8c2f5c3115e58ull && // 次モード予約とcallee-saved復帰
        std::memcmp(reinterpret_cast<void*>(0x42B801), menuReset, sizeof(menuReset)) == 0;
}
// 42B860/42B7E0の共通準備のみ実行し、CTitleMenuManagerの生成は省略。
// 42B499/42B4B6/42B541から元のTraining/Versus/Replay初期化と正規遷移予約を実行する。
// 現在mode(54EEE8)を直書きせず、次回のゲーム更新に反映を任せる。
__attribute__((naked, cdecl)) void Enter(uintptr_t, uint32_t) {
    __asm__ __volatile__(
        "movl $3,0x74D99C\n\tmovl $3,0x74D9B8\n\t"
        "movl $0,0x5585F4\n\t"
        "movl $0x42B6B0,%eax\n\tcall *%eax\n\t"
        // cdeclの引数を共通リセット後に取り直す。ホスト/P1とクライアント/P2を保持。
        "movl 8(%esp),%eax\n\tmovb %al,0x55DF0F\n\t"
        "movl 4(%esp),%eax\n\t"
        // 42B460のprologueと同じstackを用意して、選択項目の検査より後へ接続。
        "pushl %ecx\n\tpushl %ebx\n\tpushl %edi\n\t"
        "jmp *%eax\n\t");
}
uintptr_t Branch(EntryMode mode) {
    return mode == EntryMode::Replay ? 0x42B541 : mode == EntryMode::Training ? 0x42B499 :
        mode == EntryMode::CPU ? 0x42B4D3 : 0x42B4B6;
}
EntryMode bootMode = EntryMode::Versus;
uint32_t bootSide = 0;
constexpr std::array<uint8_t, 5> BootCall{0xE8,0xAD,0xFE,0xFF,0xFF};
std::array<uint8_t, 5> bootReplacement{};
__attribute__((force_align_arg_pointer)) uint32_t __cdecl BootEntry() {
    const cccaster::patch::Spec restore{"startup_boot_entry", 0x42F5EE, bootReplacement, BootCall};
    if (!cccaster::patch::Apply(std::span(&restore, 1))) ExitProcess(ERROR_WRITE_FAULT);
    // 42F5C0の起動検査は既に実行済み。初回以外は元のロゴ処理へ戻す。
    if (*reinterpret_cast<const uint8_t*>(0x55DEC3) != 1 ||
        *reinterpret_cast<const uint32_t*>(0x76E6D4) != 0)
        return reinterpret_cast<uint32_t (__cdecl*)()>(0x42F4A0)();
    // ロゴ初期化の管理状態・音声停止を維持し、ロゴ素材を読む42F4A0は通らない。
    reinterpret_cast<void (__cdecl*)()>(0x42F440)();
    *reinterpret_cast<uint8_t*>(0x55DEC3) = 1;
    *reinterpret_cast<uint32_t*>(0x54D3C0) = 1; // タイトル初期化末尾の描画有効化
    Enter(Branch(bootMode), bootSide);
    cccaster::domain::session::DebugLog("[StartupBootEntry] mode=%u next=%u kind=%u side=%u menu=%08X",
        unsigned(bootMode), *reinterpret_cast<const uint32_t*>(0x55D1D0),
        *reinterpret_cast<const uint32_t*>(0x562A74), bootSide,
        *reinterpret_cast<const uint32_t*>(0x76E6D4));
    cccaster::diagnostics::startup::Mark("boot_direct_entry");
    return 1;
}
}
namespace cccaster::game_memory::startup_direct_entry {
void Initialize(uint8_t appMode, bool isHost) {
    if (appMode > 5 || !diagnostics::startup::HasGate() || diagnostics::startup::Baseline() ||
        std::getenv("CCCASTER_STARTUP_MINIMAL_BASELINE") ||
        std::getenv("CCCASTER_STARTUP_ENTRY_BASELINE") || (appMode == 4 && !ReplayEnabled())) return;
    bootMode = appMode == 1 ? EntryMode::Training : appMode == 4 ? EntryMode::Replay :
        appMode == 3 ? EntryMode::CPU : EntryMode::Versus;
    bootSide = appMode == 0 && !isHost ? 1 : 0;
    if (!Compatible(bootMode) || Hash(0x42F5C0, 53) != 0x5b73fbe16be7b85eull ||
        Hash(0x42F440, 85) != 0x5f62d985d9796962ull) {
        domain::session::DebugLog("[StartupBootEntry] rejected=signature; title entry retained");
        return;
    }
    bootReplacement = {0xE8,0,0,0,0};
    const uint32_t relative = uint32_t(uintptr_t(&BootEntry) - 0x42F5F3);
    std::memcpy(bootReplacement.data() + 1, &relative, 4);
    const patch::Spec spec{"startup_boot_entry", 0x42F5EE, BootCall, bootReplacement};
    const auto result = patch::Apply(std::span(&spec, 1));
    if (result.rollbackFailed) ExitProcess(ERROR_WRITE_FAULT);
    domain::session::DebugLog("[StartupBootEntry] installed=%u target=%u side=%u", unsigned(bool(result)), appMode, bootSide);
}
static bool Try(uint32_t currentMode, EntryMode mode, uint32_t side) {
    static bool attempted = false;
    if (attempted || currentMode != 2 || !diagnostics::startup::HasGate() || diagnostics::startup::Baseline() ||
        std::getenv("CCCASTER_STARTUP_ENTRY_BASELINE")) return false;
    // タイトル側の正規処理が完了して、まだメニューが一度も作られていない境界に限定。
    if (!MenuBoundary()) return false;
    attempted = true;
    if (!Compatible(mode)) {
        domain::session::DebugLog("[StartupDirectEntry] rejected=signature; normal menu retained");
        return false;
    }
    Enter(Branch(mode), side);
    domain::session::DebugLog("[StartupDirectEntry] training=%u current=%u next=%u kind=%u side=%u menu=%08X versus=%u replay=%u",
        unsigned(mode == EntryMode::Training), currentMode, *reinterpret_cast<const uint32_t*>(0x55D1D0),
        *reinterpret_cast<const uint32_t*>(0x562A74), *reinterpret_cast<const uint32_t*>(0x77BFF4),
        *reinterpret_cast<const uint32_t*>(0x76E6D4), *reinterpret_cast<const uint32_t*>(0x77BF2C),
        unsigned(mode == EntryMode::Replay));
    diagnostics::startup::Mark(mode == EntryMode::Replay ? "direct_replay_entry" :
        mode == EntryMode::Training ? "direct_training_entry" : "direct_versus_entry");
    return true;
}
bool TryTraining(uint32_t currentMode) { return Try(currentMode, EntryMode::Training, 0); }
bool TryVersus(uint32_t currentMode, bool isHost) { return Try(currentMode, EntryMode::Versus, isHost ? 0 : 1); }
bool ReplayEnabled() {
    return diagnostics::startup::HasGate() && !diagnostics::startup::Baseline() &&
        !std::getenv("CCCASTER_STARTUP_REPLAY_BASELINE");
}
bool TryReplay(uint32_t currentMode) {
    return ReplayEnabled() && Try(currentMode, EntryMode::Replay, 0);
}
bool ReplayEntryPending(uint32_t currentMode) {
    return ReplayEnabled() && (currentMode != 2 || !MenuBoundary());
}
}
