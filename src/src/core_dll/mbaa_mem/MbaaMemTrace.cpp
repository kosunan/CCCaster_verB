// ============================================================================
// MbaaMemTrace.cpp — 実機ゲームメモリの毎フレーム記録（実装）
//
// 出力形式（1行1フレーム、空白区切り）:
//   [MEM] netFrame mode intro state WT RT roundTimer menuCtr
//         rng0 rng1 p1seq p2seq p1hp p2hp roundCnt p1win p2win
//   [INTRO] netFrame intro state fadeCounter p1Transition p2Transition
//           fadeBits speedTargetBits speedBits canPredict canRollback
//
// netFrame を先頭に置くのは、両プロセスをこの番号で突き合わせるため。
// 同じ netFrame で rng や seq が違えば、その時点で状態が分岐している。
// ============================================================================

#include "core_dll/mbaa_mem/MbaaMemTrace.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/DeferredNumericLog.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"

#include <windows.h>
#include <cstdlib>

namespace cccaster::game_memory {

using cccaster::domain::session::DebugLog;

namespace {

/// 読めないアドレスに触れてクラッシュしないようにする。
/// ゲーム起動直後はまだマップされていない領域がある。
template <typename T> T SafeRead(const T *addr, T fallback = T{}) {
    if (IsBadReadPtr(addr, sizeof(T)))
        return fallback;
    return *addr;
}

} // namespace

bool MbaaMemTrace::IsEnabled() {
    static const bool enabled = [] {
        const char *v = std::getenv("CCCASTER_MEM_TRACE");
        return v && v[0] == '1';
    }();
    return enabled;
}

void MbaaMemTrace::Sample(uint32_t netFrame) {
    if (!IsEnabled())
        return;

    if (SafeRead(CC_INTRO_STATE_ADDR) == 2)
        cccaster::diagnostics::DeferredNumericLog::Log("[IntroAnim] %u %u %u %u %u", netFrame,
            SafeRead(reinterpret_cast<const uint32_t *>(0x555144)), SafeRead(reinterpret_cast<const uint32_t *>(0x555464)),
            SafeRead(reinterpret_cast<const uint32_t *>(0x555C40)), SafeRead(reinterpret_cast<const uint32_t *>(0x555F60)));

    cccaster::diagnostics::DeferredNumericLog::Log("[INTRO] %u %u %u %u %u %u %u %u %u %u %u %u %u", netFrame,
        SafeRead(CC_INTRO_STATE_ADDR), SafeRead(CC_GAME_STATE_ADDR),
        SafeRead(reinterpret_cast<const uint32_t *>(0x74605C)),
        SafeRead(reinterpret_cast<const uint32_t *>(0x74D99C)),
        SafeRead(reinterpret_cast<const uint32_t *>(0x74D9B8)),
        SafeRead(reinterpret_cast<const uint32_t *>(0x558600)),
        SafeRead(reinterpret_cast<const uint32_t *>(0x55DF24)),
        SafeRead(reinterpret_cast<const uint32_t *>(0x55DEF0)),
        SafeRead(reinterpret_cast<const uint32_t *>(0x54CFC8)),
        SafeRead(reinterpret_cast<const uint32_t *>(0x54CFCC)),
        cccaster::game_interface::GameMem().CanPredict(), cccaster::game_interface::GameMem().CanRollback());

    cccaster::game_interface::RngState rng{};
    uint32_t hash = 2166136261u;
    if (cccaster::game_interface::GameMem().ReadRng(rng)) {
        for (auto value : rng) {
            hash ^= value;
            hash *= 16777619u;
        }
    } else {
        hash = 0;
    }
    cccaster::diagnostics::DeferredNumericLog::Log("[STATE] %u %u %d %d %d %d", netFrame, hash, SafeRead(CC_P1_X_POSITION_ADDR),
             SafeRead(CC_P1_Y_POSITION_ADDR), SafeRead(CC_P2_X_POSITION_ADDR),
             SafeRead(CC_P2_Y_POSITION_ADDR));

    cccaster::diagnostics::DeferredNumericLog::Log("[MEM] %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u", netFrame,
             SafeRead(CC_GAME_MODE_ADDR), static_cast<uint32_t>(SafeRead(CC_INTRO_STATE_ADDR)),
             SafeRead(CC_GAME_STATE_ADDR), SafeRead(CC_WORLD_TIMER_ADDR), SafeRead(CC_REAL_TIMER_ADDR),
             SafeRead(CC_ROUND_TIMER_ADDR), SafeRead(CC_MENU_STATE_COUNTER_ADDR),
             // ── デシンク指標 ──
             SafeRead(CC_RNG_STATE0_ADDR), SafeRead(CC_RNG_STATE1_ADDR), SafeRead(CC_P1_SEQUENCE_ADDR),
             SafeRead(CC_P2_SEQUENCE_ADDR), SafeRead(CC_P1_HEALTH_ADDR), SafeRead(CC_P2_HEALTH_ADDR),
             SafeRead(CC_ROUND_COUNT_ADDR), SafeRead(CC_P1_WINS_ADDR), SafeRead(CC_P2_WINS_ADDR));
}

} // namespace cccaster::game_memory
