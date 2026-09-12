#pragma once
// ============================================================================
// SceneInputFilter — 画面ごとの入力制約
//
// 【適用位置は「送信前」】
//   ローカル入力をバッファに書く直前に適用する。読み出し時ではない。
//
//   読み出し時に適用すると、フィルタの引数になる phase がローカルのものになる。
//   ロード時間が左右で違うとフェーズは実際にずれるため、同じフレームに対して
//   両者が違うフィルタをかけ、決定性が壊れる。
//   送信前に適用すればフィルタ済みの値が回線を通るので、両者は必ず同じ値を
//   受け取る。フェーズの一致を前提にしなくてよい。
//
// 【状態を持つ】
//   「カーソル移動直後は決定を封印」のように直近の履歴を見る制約があるため、
//   フレームをまたぐ状態を保持する。呼び出しは1フレームにつき1回で、
//   バッファへ実際に書き込むフレームとだけ対応させること
//   （背圧で書き込みを止めたフレームでは呼ばない）。
//
// 【由来】
//   旧CCCaster の historyCheck 相当。ISSUE_TRACKER A-3 / A-4 に対応する。
// ============================================================================

#include <cstdint>
#include "core_dll/mbaa_mem/GamePhaseDetector.hpp"

namespace cccaster::domain::scene {

class SceneInputFilter {
  public:
    /// カーソル移動後、決定・キャンセルを封印するフレーム数
    static constexpr uint32_t DIR_SEAL_FRAMES = 2;
    /// 決定を受け付けてから次の決定を無視するフレーム数（連打での項目スキップ防止）
    static constexpr uint32_t CONFIRM_GUARD_FRAMES = 3;

    /// フェーズが変わったら履歴を捨てる
    static void Reset();

    /// @param phase 現在の画面
    /// @param input ローカルの生入力 (direction << 16 | buttons)
    /// @return 制約適用後の入力
    static uint32_t Apply(cccaster::game_interface::GamePhase phase, uint32_t input);
};

} // namespace cccaster::domain::scene
