#pragma once
#include "shared_contracts/ProcessMemory.hpp"
#include "core_dll/common/DebugLog.hpp"

namespace cccaster::game_memory::stage_rematch {
inline bool patched = false;
inline const double FadeStep = 1.0;

// MBAACC 1.07 Rev.1.4.0。ゲームスレッドで、再抽選ONCEの合意後だけ使用する。
// 通常の解放・選択確定・ロード完了判定は残し、メニューの待機だけを短縮する。
inline bool Set(bool enable) {
    if (patched == enable) return true;
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return false;
    // 再戦画面を閉じる30F暗転。元の末尾が不透明度255と完了状態を設定する。
    constexpr std::array<uint8_t, 13> retry = {0x83,0xFF,0x1E,0xC7,0x86,0x24,0x18,0,0,0,0,0,0};
    auto fastRetry = retry;
    fastRetry[2] = 0;
    // キャラ決定→ムーン、ムーン決定→カラーの30F入力受付待機。
    // 決定入力・派生入力は従来のGameMem経路とゲームの計算を通す。
    constexpr std::array<uint8_t, 6> character = {0xC7,0,0x1E,0,0,0};
    constexpr std::array<uint8_t, 6> moon = {0xC7,6,0x1E,0,0,0};
    auto fastCharacter = character, fastMoon = moon;
    fastCharacter[2] = fastMoon[2] = 0;
    // 全キャラの一覧を順番に出す演出。0x48B870は配置済みセルのalpha更新だけ。
    // 30F後の開始・セルごとの開始時刻・alpha減算を短縮し、完了フラグは
    // 元の0x48BAB0に計算させる。画像の確保／解放やselector初期化は省略しない。
    constexpr std::array<uint8_t, 4> gridWait = {0x83,0x7E,0x34,0x1E};
    auto fastGridWait = gridWait;
    fastGridWait[3] = 0;
    constexpr std::array<uint8_t, 5> gridTime = {0x8B,0xF0,0xC1,0xFE,2};
    constexpr std::array<uint8_t, 5> fastGridTime = {0xBE,0xFF,0xFF,0xFF,0x7F};
    constexpr std::array<uint8_t, 6> gridFade = {0xDD,5,0x98,0xD6,0x53,0};
    auto fastGridFade = gridFade;
    // 最終選択をコピーする0x4288D0の直前にある30F暗転だけを1更新にする。
    constexpr std::array<uint8_t, 6> fade = {0xDC,5,0x78,0xD6,0x53,0};
    auto fastFade = fade;
    const auto step = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&FadeStep));
    std::memcpy(fastFade.data() + 2, &step, sizeof(step));
    std::memcpy(fastGridFade.data() + 2, &step, sizeof(step));
    const patch::Spec sites[] = {
        {"retry_fade", 0x43B38F, enable ? retry : fastRetry, enable ? fastRetry : retry},
        {"character_wait", 0x428010, enable ? character : fastCharacter, enable ? fastCharacter : character},
        {"moon_wait", 0x42813E, enable ? moon : fastMoon, enable ? fastMoon : moon},
        {"grid_wait", 0x42865F, enable ? gridWait : fastGridWait, enable ? fastGridWait : gridWait},
        {"grid_stagger", 0x48B897, enable ? gridTime : fastGridTime, enable ? fastGridTime : gridTime},
        {"grid_fade", 0x48B888, enable ? gridFade : fastGridFade, enable ? fastGridFade : gridFade},
        {"selection_fade", 0x4276BD, enable ? fade : fastFade, enable ? fastFade : fade},
    };
    const auto result = patch::Apply(sites);
    if (!result) {
        domain::session::DebugLog("[StageRematchAsm] FAILED enabled=%u site=%s address=%08X error=%s rollbackFailed=%u",
            unsigned(enable), result.name, unsigned(result.address), patch::Name(result.error), unsigned(result.rollbackFailed));
        if (result.rollbackFailed) ExitProcess(ERROR_WRITE_FAULT);
        return false;
    }
    patched = enable;
    domain::session::DebugLog("[StageRematchAsm] enabled=%u patches=%u", unsigned(enable), unsigned(std::size(sites)));
    return true;
}
}
