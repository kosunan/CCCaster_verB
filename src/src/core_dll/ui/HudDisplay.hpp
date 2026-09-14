#pragma once
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdio>

namespace cccaster::domain::ui {
enum class HudDisplayMode { Compact, Detailed, Hidden };

class FrameBarDisplay {
  public:
    static constexpr bool Available(int appMode) { return appMode == 1 || appMode == 2; }
    static bool Visible(int appMode = 1) { return (appMode == 2 ? spectatorVisible_ : visible_).load(std::memory_order_relaxed); }
    static void Toggle(int appMode = 1) { (appMode == 2 ? spectatorVisible_ : visible_).store(!Visible(appMode), std::memory_order_relaxed); }
  private:
    inline static std::atomic<bool> visible_{true};
    inline static std::atomic<bool> spectatorVisible_{false};
};

// キーを離す順番に依存せず、HUD操作で使ったキーだけ解放まで抑止する。
struct HudShortcutLatch {
    bool control = false, f3 = false;
    void Update(bool ctrlDown, bool f3Down, bool altDown, bool focused) {
        const bool chord = focused && ctrlDown && f3Down && !altDown;
        control = ctrlDown && (control || chord);
        f3 = f3Down && (f3 || chord);
    }
};

struct HudFixedValues {
    char ping[8]{};     // 最大 "99999ms"
    char jitter[9]{};   // 最大 "9999.9ms"
    char frame[10]{};   // 最大 "9999999us"
    char delay[3]{};    // "0F".."8F"、異常値は "?F"
    char rollback[3]{};
};

// 各値を固定幅へ収める。繰り上がり、未計測、失効で後続項目を動かさない。
inline HudFixedValues FormatHudFixedValues(bool pingAvailable, bool stale, float pingMs,
                                            bool jitterAvailable, float jitterMs,
                                            int delay, int rollback, std::int64_t frameUs) {
    HudFixedValues value;
    if (!pingAvailable)
        std::snprintf(value.ping, sizeof(value.ping), "--");
    else if (stale)
        std::snprintf(value.ping, sizeof(value.ping), "STALE");
    else {
        const float bounded = std::isfinite(pingMs) ? std::clamp(pingMs, 0.0f, 99999.0f) : 0.0f;
        std::snprintf(value.ping, sizeof(value.ping), "%.0fms", bounded);
    }

    if (!jitterAvailable)
        std::snprintf(value.jitter, sizeof(value.jitter), "--");
    else if (stale)
        std::snprintf(value.jitter, sizeof(value.jitter), "STALE");
    else {
        const float bounded = std::isfinite(jitterMs) ? std::clamp(jitterMs, 0.0f, 9999.9f) : 0.0f;
        std::snprintf(value.jitter, sizeof(value.jitter), "%.1fms", bounded);
    }

    if (frameUs > 0) {
        const auto bounded = std::min<std::int64_t>(frameUs, 9999999);
        std::snprintf(value.frame, sizeof(value.frame), "%lldus", static_cast<long long>(bounded));
    } else
        std::snprintf(value.frame, sizeof(value.frame), "--");

    if (delay >= 0 && delay <= 8)
        std::snprintf(value.delay, sizeof(value.delay), "%dF", delay);
    else
        std::snprintf(value.delay, sizeof(value.delay), "?F");
    if (rollback >= 0 && rollback <= 8)
        std::snprintf(value.rollback, sizeof(value.rollback), "%dF", rollback);
    else
        std::snprintf(value.rollback, sizeof(value.rollback), "?F");
    return value;
}

// 通常HUDは描画モードにかかわらずこの1行だけを表示する。
// ASCIIだけを使い、ゲーム同梱フォントや言語設定へ依存させない。
inline void FormatNetplayHudLine(char *output, std::size_t outputSize,
                                 bool pingAvailable, bool stale, float pingMs,
                                 bool jitterAvailable, float jitterMs,
                                 int delay, int rollback, std::int64_t frameUs) {
    const auto value = FormatHudFixedValues(pingAvailable, stale, pingMs, jitterAvailable,
                                             jitterMs, delay, rollback, frameUs);
    std::snprintf(output, outputSize,
                  "RTT %7s   JITTER %8s   D %2s   RB LIMIT %2s   1F %9s",
                  value.ping, value.jitter, value.delay, value.rollback, value.frame);
}

// 対戦中の中央下端は通信・提示時間のみ。D/Rは勝数の右隣へ表示する。
inline void FormatBattleHudLine(char *output, std::size_t outputSize,
                                bool pingAvailable, bool stale, float pingMs,
                                bool jitterAvailable, float jitterMs,
                                int delay, int rollback, std::int64_t frameUs,
                                bool qpcFallback) {
    const auto value = FormatHudFixedValues(pingAvailable, stale, pingMs, jitterAvailable,
                                             jitterMs, delay, rollback, frameUs);
    std::snprintf(output, outputSize,
                  "RTT %7s  JIT %8s  1F %9s%s",
                  value.ping, value.jitter, value.frame,
                  qpcFallback ? "  QPC" : "");
}

inline const char *ControllerSetupGuidance(bool settingsKept) {
    return settingsKept
        ? "CONTROLLER SETTINGS KEPT  >  TEST MOVEMENT / CONFIRM NOW  |  F4: REOPEN"
        : "IMPORTANT  SET YOUR CONTROLLER HERE BEFORE SELECTING  >  PRESS F4";
}

// 表示設定は同期状態に含めない。WndProc と描画から安全に参照する。
class HudDisplay {
  public:
    static HudDisplayMode Get() { return mode_.load(std::memory_order_relaxed); }
    static void Cycle() {
        auto current = mode_.load(std::memory_order_relaxed);
        while (!mode_.compare_exchange_weak(current, Next(current), std::memory_order_relaxed)) {}
    }
    static constexpr HudDisplayMode Next(HudDisplayMode mode) {
        return mode == HudDisplayMode::Compact ? HudDisplayMode::Detailed
             : mode == HudDisplayMode::Detailed ? HudDisplayMode::Hidden : HudDisplayMode::Compact;
    }
  private:
    inline static std::atomic<HudDisplayMode> mode_{HudDisplayMode::Compact};
};
} // namespace cccaster::domain::ui
