#include "StartupDirectEntry.hpp"
#include "core_dll/mbaa_mem/StartupPatch.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include <cstdlib>

namespace {
uint64_t Hash(uintptr_t address, size_t size) {
    uint64_t value = 14695981039346656037ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(address);
    for (size_t i = 0; i < size; ++i) value = (value ^ bytes[i]) * 1099511628211ull;
    return value;
}
bool Compatible(bool training) {
    constexpr uint8_t menuReset[]{0xC7,0x05,0xF4,0x85,0x55,0,0,0,0,0,0xE8,0xA0,0xFE,0xFF,0xFF};
    return cccaster::game_memory::startup::MatchesMenuCode() &&
        (training ? Hash(0x42A160, 70) == 0xada8a411ba956e3eull
                  : Hash(0x42A1B0, 76) == 0x6718fca7d61c3c99ull) && // 選んだモードの正規初期化
        Hash(0x42B6B0, 302) == 0x860cf44ca2ba6a75ull && // 旧シーンの解放・共通リセット
        Hash(0x42B870, 15) == 0xbd7777fe3cd3c94cull &&
        Hash(0x42B519, 26) == 0x9cd8c2f5c3115e58ull && // 次モード予約とcallee-saved復帰
        std::memcmp(reinterpret_cast<void*>(0x42B801), menuReset, sizeof(menuReset)) == 0;
}
// 42B860/42B7E0の共通準備のみ実行し、CTitleMenuManagerの生成は省略。
// 42B499/42B4B6から元のTraining/Versus初期化→42B519の正規遷移予約を実行する。
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
}
namespace cccaster::game_memory::startup_direct_entry {
static bool Try(uint32_t currentMode, bool training, uint32_t side) {
    static bool attempted = false;
    if (attempted || currentMode != 2 || !diagnostics::startup::HasGate() || diagnostics::startup::Baseline() ||
        std::getenv("CCCASTER_STARTUP_ENTRY_BASELINE")) return false;
    // タイトル側の正規処理が完了して、まだメニューが一度も作られていない境界に限定。
    if (*reinterpret_cast<const uint32_t*>(0x55D1D0) != 25 ||
        *reinterpret_cast<const uint8_t*>(0x55DEC3) != 1 ||
        *reinterpret_cast<const uint32_t*>(0x76E6D4) != 0) return false;
    attempted = true;
    if (!Compatible(training)) {
        domain::session::DebugLog("[StartupDirectEntry] rejected=signature; normal menu retained");
        return false;
    }
    Enter(training ? 0x42B499 : 0x42B4B6, side);
    domain::session::DebugLog("[StartupDirectEntry] training=%u current=%u next=%u kind=%u side=%u menu=%08X versus=%u",
        unsigned(training), currentMode, *reinterpret_cast<const uint32_t*>(0x55D1D0),
        *reinterpret_cast<const uint32_t*>(0x562A74), *reinterpret_cast<const uint32_t*>(0x77BFF4),
        *reinterpret_cast<const uint32_t*>(0x76E6D4), *reinterpret_cast<const uint32_t*>(0x77BF2C));
    diagnostics::startup::Mark(training ? "direct_training_entry" : "direct_versus_entry");
    return true;
}
bool TryTraining(uint32_t currentMode) { return Try(currentMode, true, 0); }
bool TryVersus(uint32_t currentMode, bool isHost) { return Try(currentMode, false, isHost ? 0 : 1); }
}
