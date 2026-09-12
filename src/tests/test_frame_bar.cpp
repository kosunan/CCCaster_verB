#include "test_support.hpp"
#include "core_dll/mbaa_mem/FrameBar.hpp"
#include "core_dll/ui/HudDisplay.hpp"
using namespace cccaster;
int main() {
    FrameBarHistory h;
    TrainingFrameSample s;
    s.valid = true;
    auto step = [&] { ++s.trueFrame; ++s.simulationFrame; h.Update(1, s); };
    CC_CASE("行動不能と攻撃判定を独立記録し停止を重ねる");
    step(); CC_CHECK_EQ(h.Size(), 0u);
    s.inactionable[0] = 8; step();
    CC_CHECK_EQ(h.At(0).players[0].state, FrameBarState::Busy);
    CC_CHECK_EQ(h.At(0).players[0].runFrame, 1u);
    s.attacking[0] = true; s.playerStopped[0] = true; step();
    CC_CHECK_EQ(h.At(1).players[0].state, FrameBarState::Active);
    CC_CHECK_EQ(h.At(1).players[0].runFrame, 1u);
    CC_CHECK(h.At(1).players[0].busy && h.At(1).players[0].stopped);
    CC_CHECK(!h.At(1).players[1].stopped);
    s.blockstun[1] = true; step();
    CC_CHECK_EQ(h.At(2).players[1].state, FrameBarState::Blockstun);
    CC_CHECK_EQ(h.At(2).players[0].runFrame, 2u);
    CC_CHECK_EQ(h.At(2).players[1].runFrame, 1u);
    s.blockstun[1] = false; s.pattern[1] = 350; step();
    CC_CHECK_EQ(h.At(3).players[1].state, FrameBarState::Hitstun);
    s.globalFreeze = true; step(); CC_CHECK(h.At(4).players[1].stopped);

    CC_CASE("描画重複は追加せず、メニュー停止も記録しない");
    h.Update(1, s); CC_CHECK_EQ(h.Size(), 5u);
    s.paused = true; step(); CC_CHECK_EQ(h.Size(), 5u);
    CC_CHECK_EQ(h.At(4).players[0].runFrame, 4u);
    s.paused = false;
    CC_CASE("45Fを超えた履歴は古い側から捨てる");
    s.globalFreeze = false; s.playerStopped[0] = false;
    for (unsigned i = 0; i < 100; ++i) step();
    CC_CHECK_EQ(h.Size(), 45u);
    CC_CHECK_EQ(h.At(0).players[0].state, FrameBarState::Active);
    CC_CHECK_EQ(h.At(0).players[0].runFrame, 60u);
    CC_CHECK_EQ(h.At(44).players[0].runFrame, 104u);

    CC_CASE("双方ニュートラル15Fで保持し次動作で新規記録");
    s.inactionable[0] = 0; s.pattern[1] = 0; s.attacking[0] = false;
    for (unsigned i = 0; i < 14; ++i) step();
    CC_CHECK(!h.Holding());
    step();
    CC_CHECK(h.Holding());
    for (unsigned i = 0; i < 100; ++i) step();
    CC_CHECK_EQ(h.At(29).players[0].state, FrameBarState::Active);
    CC_CHECK_EQ(h.At(30).players[0].state, FrameBarState::Ready);
    s.inactionable[0] = 1; step(); CC_CHECK_EQ(h.Size(), 1u);
    CC_CHECK_EQ(h.At(0).players[0].runFrame, 1u);

    CC_CASE("欠測、巻戻し、ラウンド、交代、モード変更で履歴破棄");
    s.trueFrame += 2; h.Update(1, s); CC_CHECK_EQ(h.Size(), 1u);
    CC_CHECK_EQ(h.At(0).players[0].runFrame, 1u);
    step(); CC_CHECK_EQ(h.Size(), 2u);
    s.trueFrame = s.simulationFrame = 1; h.Update(1, s); CC_CHECK_EQ(h.Size(), 1u);
    step(); ++s.round; step(); CC_CHECK_EQ(h.Size(), 1u);
    step(); s.activeCharacter[0] = 2; step(); CC_CHECK_EQ(h.Size(), 1u);
    step(); h.Update(2, s); CC_CHECK_EQ(h.Size(), 1u);
    h.Update(0, s); CC_CHECK_EQ(h.Size(), 0u);
    h.Update(2, s); CC_CHECK_EQ(h.Size(), 1u);
    h.Update(2, {}); CC_CHECK_EQ(h.Size(), 0u);
    CC_CASE("F1の表示状態はトレーニング観戦専用で履歴から独立");
    using domain::ui::FrameBarDisplay;
    CC_CHECK(!FrameBarDisplay::Available(0));
    CC_CHECK(FrameBarDisplay::Available(1) && FrameBarDisplay::Available(2));
    CC_CHECK(!FrameBarDisplay::Available(255));
    CC_CHECK(FrameBarDisplay::Visible()); FrameBarDisplay::Toggle();
    CC_CHECK(!FrameBarDisplay::Visible()); FrameBarDisplay::Toggle();
    CC_CHECK(FrameBarDisplay::Visible());
    return cccaster::test::Summarize("frame_bar");
}
