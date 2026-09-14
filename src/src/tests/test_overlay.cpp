// ============================================================================
// test_overlay.cpp — Overlay 3層 単体テスト
// ============================================================================
//
// ゲーム本体を起動せずに、Overlay のロジック部分を検証する。
// ControllerMapper はスタブで置換し DirectInputHook 依存を排除。
//
// ビルド: cmake --build build -j12
// 実行:   build/bin/test_overlay.exe
// ============================================================================
// NETPLAY_OVERLAY_TEST は CMake の target_compile_definitions で定義済み

#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>

// テストヘルパー（friend 経由で private static にアクセス）
class NetplayOverlayTestHelper;

#include "core_dll/ui/OverlayRenderer.hpp"
#include "core_dll/ui/NetplayOverlay.hpp"

using cccaster::domain::ui::NetplayOverlay;
using cccaster::overlay::OverlayRenderer;

/// friend を通じて private static メンバーにアクセスするヘルパー
class NetplayOverlayTestHelper {
  public:
    static int getDelay() {
        return NetplayOverlay::currentDelay;
    }
    static int getRollback() {
        return NetplayOverlay::currentRollback;
    }
    static uint64_t getDelayEndTime() {
        return NetplayOverlay::delayDisplayTimeEndTime;
    }
    static uint64_t getRollbackEndTime() {
        return NetplayOverlay::rollbackDisplayTimeEndTime;
    }
    static float getWorstPing() {
        return NetplayOverlay::worstPingCache;
    }
    static float getWorstJitter() {
        return NetplayOverlay::worstJitterCache;
    }
    static int getHistoryIndex() {
        return NetplayOverlay::historyIndex;
    }
    static bool getShowMapping() {
        return NetplayOverlay::showMappingWindow;
    }

    /// 状態をリセット（テスト間の独立性確保）
    static void reset() {
        NetplayOverlay::currentDelay = 0;
        NetplayOverlay::currentRollback = 0;
        NetplayOverlay::delayDisplayTimeEndTime = 0;
        NetplayOverlay::rollbackDisplayTimeEndTime = 0;
        NetplayOverlay::showMappingWindow = false;
        NetplayOverlay::historyIndex = 0;
        NetplayOverlay::worstPingCache = 0.0f;
        NetplayOverlay::worstJitterCache = 0.0f;
        memset(NetplayOverlay::pingHistory, 0, sizeof(NetplayOverlay::pingHistory));
        memset(NetplayOverlay::jitterHistory, 0, sizeof(NetplayOverlay::jitterHistory));
    }
};

// ============================================================================
// テストランナー
// ============================================================================

static int g_passed = 0;
static int g_failed = 0;

static void check(const char *name, bool condition) {
    if (condition) {
        std::cout << "  [PASS] " << name << "\n";
        g_passed++;
    } else {
        std::cout << "  [FAIL] " << name << "\n";
        g_failed++;
    }
}

// ============================================================================
// テストケース
// ============================================================================

void test_GetTimeMs() {
    std::cout << "\n--- OverlayRenderer::GetTimeMs ---\n";

    uint64_t t1 = OverlayRenderer::GetTimeMs();
    check("GetTimeMs returns > 0", t1 > 0);

    // 10ms スリープ後に再取得して単調増加を確認
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t t2 = OverlayRenderer::GetTimeMs();
    check("GetTimeMs monotonic (t2 >= t1)", t2 >= t1);
    check("GetTimeMs advances after sleep (t2 > t1)", t2 > t1);
}

void test_OnDelayInput() {
    std::cout << "\n--- NetplayOverlay::OnDelayInput ---\n";
    NetplayOverlayTestHelper::reset();

    NetplayOverlay::OnDelayInput(5);
    check("OnDelayInput(5) -> currentDelay == 5", NetplayOverlayTestHelper::getDelay() == 5);
    check("OnDelayInput(5) -> timer set (> 0)", NetplayOverlayTestHelper::getDelayEndTime() > 0);

    uint64_t prevEndTime = NetplayOverlayTestHelper::getDelayEndTime();

    NetplayOverlay::OnDelayInput(9);
    check("OnDelayInput(9) -> currentDelay == 9 (overwrite)", NetplayOverlayTestHelper::getDelay() == 9);
    check("OnDelayInput(9) -> timer updated", NetplayOverlayTestHelper::getDelayEndTime() >= prevEndTime);

    NetplayOverlay::OnDelayInput(0);
    check("OnDelayInput(0) -> currentDelay == 0 (min value)", NetplayOverlayTestHelper::getDelay() == 0);
}

void test_OnRollbackInput() {
    std::cout << "\n--- NetplayOverlay::OnRollbackInput ---\n";
    NetplayOverlayTestHelper::reset();

    NetplayOverlay::OnRollbackInput(3);
    check("OnRollbackInput(3) -> currentRollback == 3", NetplayOverlayTestHelper::getRollback() == 3);
    check("OnRollbackInput(3) -> timer set (> 0)", NetplayOverlayTestHelper::getRollbackEndTime() > 0);

    NetplayOverlay::OnRollbackInput(7);
    check("OnRollbackInput(7) -> currentRollback == 7", NetplayOverlayTestHelper::getRollback() == 7);
}

void test_OnMappingInput() {
    std::cout << "\n--- NetplayOverlay::OnMappingInput ---\n";
    NetplayOverlayTestHelper::reset();

    check("Initial: showMappingWindow == false", !NetplayOverlayTestHelper::getShowMapping());

    NetplayOverlay::OnMappingInput();
    check("After toggle: showMappingWindow == true", NetplayOverlayTestHelper::getShowMapping());

    NetplayOverlay::OnMappingInput();
    check("After 2nd toggle: showMappingWindow == false", !NetplayOverlayTestHelper::getShowMapping());
}

void test_UpdateNetworkMetrics_basic() {
    std::cout << "\n--- NetplayOverlay::UpdateNetworkMetrics (基本) ---\n";
    NetplayOverlayTestHelper::reset();

    NetplayOverlay::UpdateNetworkMetrics(50.0f, 5.0f);
    check("After 1 sample: worstPing == 50", NetplayOverlayTestHelper::getWorstPing() == 50.0f);
    check("After 1 sample: worstJitter == 5", NetplayOverlayTestHelper::getWorstJitter() == 5.0f);

    NetplayOverlay::UpdateNetworkMetrics(30.0f, 8.0f);
    check("After 2 samples: worstPing still 50 (max)", NetplayOverlayTestHelper::getWorstPing() == 50.0f);
    check("After 2 samples: worstJitter now 8 (new max)", NetplayOverlayTestHelper::getWorstJitter() == 8.0f);

    NetplayOverlay::UpdateNetworkMetrics(100.0f, 2.0f);
    check("After 3 samples: worstPing now 100", NetplayOverlayTestHelper::getWorstPing() == 100.0f);
    check("After 3 samples: worstJitter still 8", NetplayOverlayTestHelper::getWorstJitter() == 8.0f);
}

void test_UpdateNetworkMetrics_ringBuffer() {
    std::cout << "\n--- NetplayOverlay::UpdateNetworkMetrics (リングバッファ回転) ---\n";
    NetplayOverlayTestHelper::reset();

    // METRICS_HISTORY_SIZE (60) 回、全て ping=10, jitter=1 で埋める
    for (int i = 0; i < 60; ++i) {
        NetplayOverlay::UpdateNetworkMetrics(10.0f, 1.0f);
    }
    check("After 60 samples: historyIndex == 0 (wrapped)", NetplayOverlayTestHelper::getHistoryIndex() == 0);
    check("After 60 uniform samples: worstPing == 10", NetplayOverlayTestHelper::getWorstPing() == 10.0f);

    // 1つだけ高い値を上書き
    NetplayOverlay::UpdateNetworkMetrics(200.0f, 20.0f);
    check("After spike: worstPing == 200", NetplayOverlayTestHelper::getWorstPing() == 200.0f);
    check("After spike: worstJitter == 20", NetplayOverlayTestHelper::getWorstJitter() == 20.0f);
    check("After spike: historyIndex == 1", NetplayOverlayTestHelper::getHistoryIndex() == 1);

    // さらに59回上書きして古いスパイクを押し出す
    for (int i = 0; i < 59; ++i) {
        NetplayOverlay::UpdateNetworkMetrics(5.0f, 0.5f);
    }
    check("After flush: worstPing == 200 (spike still in buffer)",
          NetplayOverlayTestHelper::getWorstPing() == 200.0f);

    // 最後の1回で完全に上書き（スパイクがindex 0にあったので、index 0に書く）
    NetplayOverlay::UpdateNetworkMetrics(5.0f, 0.5f);
    check("After full rotation: worstPing == 5 (spike evicted)",
          NetplayOverlayTestHelper::getWorstPing() == 5.0f);
    check("After full rotation: worstJitter == 0.5", NetplayOverlayTestHelper::getWorstJitter() == 0.5f);
}

void test_DisplayDuration() {
    std::cout << "\n--- 表示タイマー整合性 ---\n";
    NetplayOverlayTestHelper::reset();

    uint64_t before = OverlayRenderer::GetTimeMs();
    NetplayOverlay::OnDelayInput(1);
    uint64_t endTime = NetplayOverlayTestHelper::getDelayEndTime();
    uint64_t after = OverlayRenderer::GetTimeMs();

    uint64_t duration = 800; // OVERLAY_DISPLAY_DURATION_MS
    check("Timer in valid range: endTime >= before + 800", endTime >= before + duration);
    check("Timer in valid range: endTime <= after + 800 + 1ms margin", endTime <= after + duration + 1);
}

// ============================================================================
// メイン
// ============================================================================

int main() {
    std::cout << "\n=== Overlay Unit Test ===\n";

    test_GetTimeMs();
    test_OnDelayInput();
    test_OnRollbackInput();
    test_OnMappingInput();
    test_UpdateNetworkMetrics_basic();
    test_UpdateNetworkMetrics_ringBuffer();
    test_DisplayDuration();

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n\n";
    return g_failed > 0 ? 1 : 0;
}
