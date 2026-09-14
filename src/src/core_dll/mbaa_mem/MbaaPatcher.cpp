// ============================================================================
// MbaaPatcher.cpp — MBAA 固有メモリパッチの実装
// ============================================================================

#include "core_dll/mbaa_mem/MbaaPatcher.hpp"
#include "core_dll/mbaa_mem/MemoryPatcher.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/common/DebugLog.hpp"

namespace cccaster::game_memory {

using MP = cccaster::core::memory::MemoryPatcher;

// ============================================================================
// ApplyStartupPatches — DLL起動時に1回だけ呼び出す
// ============================================================================
void MbaaPatcher::ApplyStartupPatches(bool training) {

    // 旧版 disableTrainingMusicReset と同じ分岐。BGM停止CALLだけを飛ばし、
    // トレーニングのラウンド初期化はゲーム本来の処理を継続する。
    // DLL入口の対応版照合に加え、対象命令も照合する。ネット対戦には適用しない。
    if (training) {
        constexpr uint8_t expected[] = {0x75, 0x05, 0xE8, 0xCC, 0xB2, 0x06, 0x00};
        constexpr uint8_t replacement[] = {0xEB, 0x05};
        const bool applied = std::memcmp(reinterpret_cast<void *>(0x472C6D), expected, sizeof(expected)) == 0 &&
            MP::WritePatch(0x472C6D, replacement, sizeof(replacement));
        cccaster::domain::session::DebugLog("[TrainingMusic] preserve_on_reset=%d", int(applied));
    }

    // ─────────────────────────────────────────────────────────
    // [1] NOP パッチ: ゲームエンジンの入力クリアループ無効化
    // ─────────────────────────────────────────────────────────
    // MBAA のゲームエンジンは毎フレーム DirectInput を読み取り、
    // 内部の入力バッファをクリアするループを持っている。
    // CCCaster は DirectInputHook で入力を横取りするため、
    // このクリアループを NOP で潰す必要がある。
    struct NopTarget {
        uintptr_t addr;
        size_t size;
    };
    constexpr NopTarget nopTargets[] = {{0x41F098, 2}, {0x41F0A0, 3}, {0x4A024E, 2},
                                        {0x4A027F, 3}, {0x4A0291, 3}, {0x4A02A2, 3},
                                        {0x4A02B4, 3}, {0x4A02E9, 2}, {0x4A02F2, 3}};
    for (const auto &t : nopTargets) {
        MP::NopPatch(t.addr, t.size);
    }

    // ─────────────────────────────────────────────────────────
    // [2] キーボードマップのゼロクリア
    // ─────────────────────────────────────────────────────────
    // ゲームエンジンが物理キーボードから入力を読み取るのを防ぐ。
    constexpr uintptr_t KEYBOARD_MAP_ADDR = 0x54D2C0;
    MP::ZeroMemoryRegion(KEYBOARD_MAP_ADDR, 20);

    // ─────────────────────────────────────────────────────────
    // [3] ウィンドウ非アクティブ判定の無効化
    // ─────────────────────────────────────────────────────────
    // ゲームが非アクティブ時に一時停止するのを防ぐ。
    MP::NopPatch(reinterpret_cast<uintptr_t>(CC_AUTO_ACTIVATE_ADDR), 11);

    const uint8_t jmp1[] = {0xEB, 0x0E};
    MP::WritePatch(0x04A1D42, jmp1, sizeof(jmp1));

    const uint8_t jmp2[] = {0xEB};
    MP::WritePatch(0x04A1D4A, jmp2, sizeof(jmp2));

    cccaster::domain::session::DebugLog("[MbaaPatcher] 起動パッチ適用完了");
}

} // namespace cccaster::game_memory
