#pragma once
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/rollback/PresentationRollback.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/Platform.hpp"
#include <cstdlib>

namespace cccaster::domain::session::input_runahead {
// 検証専用。確定入力/履歴には触れず、保持予測による1更新の画像だけを提示する。
// 次のSceneRunner::Stepより前に戻す。標準の入力変換・予約・実行は全て元の順序。
enum class Phase { Normal, Save, Preview };
inline Phase phase = Phase::Normal;
inline sync::PresentationRollback snapshot;
inline uint32_t previews = 0;
inline int64_t started = 0;
inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("CCCASTER_TEST_INPUT_RUNAHEAD");
        return value && value[0] == '1';
    }();
    return enabled;
}
inline bool Previewing() { return phase == Phase::Preview; }
inline bool BeforePresent(bool eligible) {
    if (phase == Phase::Normal && Enabled() && eligible) phase = Phase::Save;
    return phase == Phase::Save;
}
inline bool SkipPresent() {
    auto& mem = game_interface::GameMem();
    return phase == Phase::Save || (Previewing() &&
        (mem.GameMode() != CC_GAME_MODE_IN_GAME || mem.IntroState() != 0));
}
// true: 保存後、標準のメインループへ戻って1回だけ余分に更新する。
// false: 復元完了後、通常の入力取得・同期準備を続ける。
inline bool AfterPresent() {
    if (phase == Phase::Normal) return false;
    auto& mem = game_interface::GameMem();
    if (phase == Phase::Save) {
        if (!snapshot.Begin(mem)) {
            DebugLog("[InputRunahead] FAIL save");
            platform::TerminateSelf(); return true;
        }
        phase = Phase::Preview;
        started = platform::RealMonotonicUs();
        return true;
    }
    const auto elapsed = platform::RealMonotonicUs() - started;
    if (!snapshot.Restore(mem)) {
        DebugLog("[InputRunahead] FAIL restore");
        platform::TerminateSelf(); return true;
    }
    phase = Phase::Normal;
    ++previews;
    if (previews == 1 || previews % 120 == 0)
        DebugLog("[InputRunahead] restored=%u equal=1 workUs=%lld world=%u", previews, elapsed, mem.WorldTimer());
    return false;
}
}
