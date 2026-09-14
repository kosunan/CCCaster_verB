#pragma once
#include <cstdint>
namespace cccaster::public_api {
enum class SessionExitReason : uint8_t { Unknown = 0, CloseButton = 1, Escape = 2, F12 = 3 };
inline bool ValidExitReason(uint32_t value) { return value <= 3; }
inline const char *ExitReasonText(SessionExitReason reason) {
    switch (reason) {
    case SessionExitReason::CloseButton: return "ゲームの閉じるボタンが押されました。";
    case SessionExitReason::Escape: return "ゲームでESCキーが押されました。";
    case SessionExitReason::F12: return "ゲームでF12キーが押されました。";
    default: return "ゲームプロセスが終了しました（操作理由は不明）。";
    }
}
}
