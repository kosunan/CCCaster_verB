#pragma once
#include "shared_contracts/ProcessMemory.hpp"
#include "core_dll/common/DebugLog.hpp"

namespace cccaster::game_memory::stage_rematch {
inline bool patched = false;

// MBAACC 1.07 Rev.1.4.0。ゲームスレッドで、再抽選ONCEの合意後だけ使用する。
// 通常ONCEの解放・ロード完了判定は残し、再戦メニューの暗転だけを短縮する。
inline bool Set(bool enable) {
    if (patched == enable) return true;
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return false;
    // 再戦画面を閉じる30F暗転。元の末尾が不透明度255と完了状態を設定する。
    constexpr std::array<uint8_t, 13> retry = {0x83,0xFF,0x1E,0xC7,0x86,0x24,0x18,0,0,0,0,0,0};
    auto fastRetry = retry;
    fastRetry[2] = 0;
    const patch::Spec sites[] = {
        {"retry_fade", 0x43B38F, enable ? retry : fastRetry, enable ? fastRetry : retry},
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
