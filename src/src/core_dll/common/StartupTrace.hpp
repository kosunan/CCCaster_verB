#pragma once
#include "shared_contracts/StartupTrace.hpp"
#include "core_dll/common/DebugLog.hpp"

namespace cccaster::diagnostics::startup {
inline void Mark(const char *event) {
    if (Enabled()) cccaster::domain::session::DebugLog("[Startup] event=%s qpcUs=%lld", event, QpcUs());
}
// ゲームスレッドだけで使用。通常戦闘ではログも時計取得も行わない。
inline bool inputRecorded = false;
inline bool presentRecorded = false;
inline void InputReady() {
    if (!inputRecorded) { inputRecorded = true; Mark("chara_input"); }
}
}
