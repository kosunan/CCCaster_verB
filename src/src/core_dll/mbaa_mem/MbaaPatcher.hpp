#pragma once
// ============================================================================
// MbaaPatcher — MBAA 固有のメモリパッチ（起動時1回適用）
//
// 【責務】
//   DLL インジェクション直後に、ゲームエンジンの動作を CCCaster 向けに
//   書き換えるためのパッチ群をまとめて適用する。
//
// 【適用するパッチ】
//   1. 入力クリアループの NOP 化（DirectInputHook での横取りのため）
//   2. キーボードマップのゼロクリア（物理キーボード入力の遮断）
//   3. ウィンドウ非アクティブ判定の無効化（裏画面でも動作継続）
//
// 【依存関係】
//   - MemoryPatcher  : 汎用メモリ操作プリミティブ（NopPatch / WritePatch 等）
//   - MbaaConstants  : MBAA 固有アドレス定義
//
// 【Core 層の位置づけ】
//   game_memory_accessor 層に属する。ゲーム固有アドレスの知識を持つが、
//   ネットワーク・UI・同期ロジックには一切依存しない。
// ============================================================================

namespace cccaster::game_memory {

class MbaaPatcher {
  public:
    /// DLL 起動時に1回だけ呼び出す。全パッチを順次適用する。
    static void ApplyStartupPatches(bool training);
};

} // namespace cccaster::game_memory
