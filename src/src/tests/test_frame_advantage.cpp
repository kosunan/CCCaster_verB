#include "test_support.hpp"
#include "core_dll/mbaa_mem/TrainingMetrics.hpp"

using namespace cccaster;

namespace {
TrainingFrameSample Frame(uint32_t frame, int p1, int p2, bool stopped = false) {
    TrainingFrameSample s;
    s.valid = true;
    s.trueFrame = s.simulationFrame = frame;
    s.inactionable[0] = p1;
    s.inactionable[1] = p2;
    s.stopped = stopped;
    return s;
}
void Check(const FrameAdvantageTracker &tracker, FrameAdvantageState state, int frames = 0) {
    CC_CHECK_EQ(tracker.Result().state, state);
    CC_CHECK_EQ(tracker.Result().p1Frames, frames);
}
}

int main() {
    FrameAdvantageTracker tracker;
    CC_CASE("未測定と双方同時終了の確定0Fを分離");
    tracker.Update(1, Frame(1, 0, 0));
    Check(tracker, FrameAdvantageState::Unmeasured);
    tracker.Update(1, Frame(2, 5, 4));
    Check(tracker, FrameAdvantageState::Measuring);
    tracker.Update(1, Frame(3, 0, 0));
    Check(tracker, FrameAdvantageState::Confirmed, 0);

    CC_CASE("P1先行は双方終了まで途中値を公開せず3F確定");
    tracker.Update(1, Frame(4, 3, 5));
    for (unsigned f = 5; f < 8; ++f) {
        tracker.Update(1, Frame(f, 0, 8 - f));
        Check(tracker, FrameAdvantageState::Measuring);
    }
    tracker.Update(1, Frame(8, 0, 0));
    Check(tracker, FrameAdvantageState::Confirmed, 3);
    tracker.Update(1, Frame(9, 0, 0));
    Check(tracker, FrameAdvantageState::Confirmed, 3);

    CC_CASE("P2先行と負の行動不能値は符号付きで扱う");
    tracker.Update(1, Frame(10, -2, 3));
    tracker.Update(1, Frame(11, -2, 0));
    tracker.Update(1, Frame(12, -2, 0));
    tracker.Update(1, Frame(13, 0, 0));
    Check(tracker, FrameAdvantageState::Confirmed, -2);

    CC_CASE("描画の重複サンプルと片側/共通停止で加算しない");
    tracker.Reset();
    tracker.Update(1, Frame(1, 5, 8));
    tracker.Update(1, Frame(2, 0, 5));
    tracker.Update(1, Frame(2, 0, 5));
    tracker.Update(1, Frame(3, 0, 4, true));
    tracker.Update(1, Frame(4, 0, 3, true));
    tracker.Update(1, Frame(5, 0, 1));
    tracker.Update(1, Frame(6, 0, 0, true));
    Check(tracker, FrameAdvantageState::Measuring);
    tracker.Update(1, Frame(7, 0, 0));
    Check(tracker, FrameAdvantageState::Confirmed, 2);

    CC_CASE("スローの実進行0Fは計数しない");
    tracker.Reset();
    tracker.Update(1, Frame(1, 5, 8));
    auto slow = Frame(2, 0, 4);
    slow.simulationFrame = 1;
    tracker.Update(1, slow);
    slow.trueFrame = 3;
    slow.simulationFrame = 2;
    tracker.Update(1, slow);
    slow.trueFrame = 4;
    slow.simulationFrame = 3;
    slow.inactionable[1] = 0;
    tracker.Update(1, slow);
    Check(tracker, FrameAdvantageState::Confirmed, 1);

    CC_CASE("再接触で以前の片側先行期間を破棄");
    tracker.Reset();
    tracker.Update(1, Frame(1, 4, 6));
    tracker.Update(1, Frame(2, 0, 4));
    tracker.Update(1, Frame(3, 3, 5));
    tracker.Update(1, Frame(4, 2, 0));
    tracker.Update(1, Frame(5, 0, 0));
    Check(tracker, FrameAdvantageState::Confirmed, -1);

    CC_CASE("新しい片側動作に前の確定値を残さない");
    tracker.Update(1, Frame(6, 4, 0));
    Check(tracker, FrameAdvantageState::Unmeasured);
    tracker.Update(1, Frame(7, 0, 0));
    Check(tracker, FrameAdvantageState::Unmeasured);

    CC_CASE("欠測/巻戻し/ラウンド/操作交代は過去計測を破棄");
    for (int change = 0; change < 5; ++change) {
        tracker.Reset();
        tracker.Update(1, Frame(100, 4, 5));
        tracker.Update(1, Frame(101, 0, 2));
        auto changed = Frame(102, 0, 0);
        if (change == 0) changed.trueFrame = changed.simulationFrame = 104;
        if (change == 1) changed.trueFrame = changed.simulationFrame = 99;
        if (change == 2) changed.round = 2;
        if (change == 3) changed.activeCharacter[0] = 2;
        if (change == 4) changed.simulationFrame = 99;
        tracker.Update(1, changed);
        Check(tracker, FrameAdvantageState::Unmeasured);
    }

    CC_CASE("モード0で無効、観戦モード2で有効、読取失敗で消去");
    tracker.Reset();
    tracker.Update(0, Frame(1, 4, 4));
    tracker.Update(0, Frame(2, 0, 0));
    Check(tracker, FrameAdvantageState::Unmeasured);
    tracker.Update(2, Frame(3, 4, 4));
    tracker.Update(2, Frame(4, 0, 1));
    tracker.Update(2, Frame(5, 0, 0));
    Check(tracker, FrameAdvantageState::Confirmed, 1);
    tracker.Update(1, Frame(6, 0, 0));
    Check(tracker, FrameAdvantageState::Unmeasured);
    tracker.Update(2, Frame(7, 4, 4));
    tracker.Update(2, {});
    Check(tracker, FrameAdvantageState::Unmeasured);

    CC_CASE("大きい有利値を剰余100で切り捨てない");
    tracker.Update(1, Frame(1, 1, 200));
    for (unsigned f = 2; f <= 124; ++f) tracker.Update(1, Frame(f, 0, 200 - f));
    tracker.Update(1, Frame(125, 0, 0));
    Check(tracker, FrameAdvantageState::Confirmed, 123);
    tracker.Reset();
    Check(tracker, FrameAdvantageState::Unmeasured);
    return cccaster::test::Summarize("frame_advantage");
}
