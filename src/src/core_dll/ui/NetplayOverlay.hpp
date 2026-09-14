// ============================================================================
// NetplayOverlay.hpp — ネット対戦UI オーバーレイ描画管理
// ============================================================================
//
// 【設計思想】
//   DLLインジェクションによりゲームプロセス内で動作する
//   ネット対戦UIオーバーレイの表示制御を担う。
//   全メソッド・全メンバーが static のシングルトン的設計。
//
// 【3層分離】
//   core/overlay/OverlayRenderer : スタイル・描画ユーティリティ基盤
//   domain/ui/NetplayOverlay     : 表示ロジック (本クラス)
//   domain/ui/ControllerMapper   : コントローラーマッピング業務ロジック
//
// 【表示条件】
//   呼び出し元が isCharaSelect=true を渡した時のみ描画。
//   ゲームメモリへの直接アクセスは行わない（呼び出し元の責務）。
//
// 【呼び出し元】
//   - DxHook.cpp      → Render(isCharaSelect)
//   - InputHook.cpp   → OnDelayInput() / OnRollbackInput() / OnMappingInput()
//   - net_Versus_main → UpdateNetworkMetrics()
// ============================================================================
#pragma once
#include <cstdint>

#ifdef NETPLAY_OVERLAY_TEST
class NetplayOverlayTestHelper;
#endif

namespace cccaster::domain::ui {

class NetplayOverlay {
#ifdef NETPLAY_OVERLAY_TEST
    friend class ::NetplayOverlayTestHelper;
#endif
  public:
    // ================================================================
    // 定数定義
    // ================================================================

    static constexpr int METRICS_HISTORY_SIZE = 60;
    static constexpr uint64_t OVERLAY_DISPLAY_DURATION_MS = 800;

    // ================================================================
    // パブリックAPI
    // ================================================================

    /// @brief 毎フレーム呼び出される描画エントリポイント。
    /// @param isCharaSelect キャラセレ画面かどうか。trueの時のみUI描画。
    static void Render(bool isCharaSelect);

    /// Delay値変更通知 (Ctrl+0-9)
    static void OnDelayInput(int num);

    /// Rollback値変更通知 (Alt+0-9)
    static void OnRollbackInput(int num);

    /// マッピングウィンドウ開閉トグル (F4)
    static void OnMappingInput();

    /// ネットワークメトリクス更新
    static void UpdateNetworkMetrics(float pingMs, float jitterMs);

  private:
    // ================================================================
    // 描画サブルーチン
    // ================================================================

    static void DrawConstantGuide();
    static void DrawDelaySetting();
    static void DrawRollbackSetting();

    // ================================================================
    // メンバー変数 (全て static)
    // ================================================================

    static uint64_t delayDisplayTimeEndTime;
    static uint64_t rollbackDisplayTimeEndTime;
    static bool showMappingWindow;
    static int currentDelay;
    static int currentRollback;
    static float pingHistory[METRICS_HISTORY_SIZE];
    static float jitterHistory[METRICS_HISTORY_SIZE];
    static int historyIndex;
    static float worstPingCache;
    static float worstJitterCache;
};

} // namespace cccaster::domain::ui
