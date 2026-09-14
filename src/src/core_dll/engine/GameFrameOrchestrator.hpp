#pragma once
// ============================================================================
// GameFrameOrchestrator — DxHook コールバック統合（フレーム制御の司令塔）
// ============================================================================
//
// 【責務】
//   DxHook (infra層) のコールバックとして登録され、
//   以下の3つの処理を管理する:
//
//   1. DLLロジック実行   (AfterPresent コールバック)
//      - SceneRunner::Step()
//      - DirectInputHook::Poll()
//
//   2. ImGui 描画        (EndScene コールバック)
//      - ImGui 遅延初期化（初回のみ）
//      - バックバッファ判定 → 一致時のみ ImGui 描画
//      - GameMode → UiPhase 変換 → UIManager::Render()
//
//   3. 高速スキップ      (EndScene コールバック / Present スキップ)
//      - RenderSkip=true → EndScene 内の ImGui 描画をスキップ
//      - SkipMode=true   → Present の元関数をスキップ
//
// 【設計上の位置づけ】
//   domain_session 層に属する。DxHook(infra) からコールバック経由で
//   呼ばれることで、infra→domain の直接依存を排除する。
//
// 【スレッド安全性】
//   ゲームスレッド (D3D9 コールバック) からのみ呼ばれる。
// ============================================================================

#include <d3d9.h>

namespace cccaster::domain::session {

class GameFrameOrchestrator {
  public:
    /// DxHook にコールバックを登録する。
    /// dllmain.cpp の InitThread 末尾（SceneRunner::Init 完了後）で呼ぶ。
    static void Register();

    /// DxHook コールバック解除 + ImGui 破棄。
    /// Shutdown は DxHook::Shutdown() の前に呼ぶこと。
    static void Shutdown();

    // ── DxHook コールバック ──

    /// EndScene コールバック: ImGui描画（高速モード時はスキップ）
    static void OnEndScene(LPDIRECT3DDEVICE9 pDevice);

    /// Present コールバック: DLLロジック実行
    static void OnPresent(LPDIRECT3DDEVICE9 pDevice);
    static void OnAfterPresent(LPDIRECT3DDEVICE9 pDevice);

    /// Present スキップ判定: 高速モード時は true
    static bool OnPresentSkip(LPDIRECT3DDEVICE9 pDevice);

    /// Reset 前コールバック: ImGui リソース解放
    static void OnPreReset(LPDIRECT3DDEVICE9 pDevice);

    /// Reset 後コールバック: ImGui リソース再生成
    static void OnPostReset(LPDIRECT3DDEVICE9 pDevice);
};

} // namespace cccaster::domain::session
