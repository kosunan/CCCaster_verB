#pragma once

namespace cccaster {
struct FrameAdvantageResult;
}

namespace cccaster::domain::ui {

// 標準の入力変化・継続F履歴はゲーム自身に任せる。
// この描画はゲームメモリも入力履歴も変更しない。
class LearningOverlay {
  public:
    // 戦闘画面の通常描画から呼ぶ。1=トレーニング、2=観戦用モード。
    // mode=2 の表示対応は観戦通信の実装を意味しない。
    static void Draw(int appMode, const cccaster::FrameAdvantageResult &result);
};

} // namespace cccaster::domain::ui
