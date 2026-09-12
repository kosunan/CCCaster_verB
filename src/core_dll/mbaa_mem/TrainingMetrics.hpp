#pragma once
#include <cstdint>

namespace cccaster {

enum class FrameAdvantageState : uint8_t { Unmeasured, Measuring, Confirmed };

struct FrameAdvantageResult {
    FrameAdvantageState state = FrameAdvantageState::Unmeasured;
    // 確定時だけ有効。P1が先に動けた場合が正、P2視点では符号を反転する。
    int p1Frames = 0;
};

struct TrainingFrameSample {
    bool valid = false;
    uint32_t trueFrame = 0;
    uint32_t simulationFrame = 0;
    uint32_t round = 0;
    int activeCharacter[2] = {0, 1};
    int inactionable[2] = {};
    bool stopped = false;
};

// ゲーム更新後の連続サンプル専用。描画回数・予定周期からF数を推定しない。
// ETMの行動不能カウンタと停止条件を参照し、双方終了まで値を公開しない。
// ロード/再計算開始/世代変更では呼出元もResetする。不連続列は補間しない。
class FrameAdvantageTracker {
  public:
    void Reset() { *this = FrameAdvantageTracker{}; }
    FrameAdvantageResult Result() const { return result_; }

    void Update(int appMode, const TrainingFrameSample &sample) {
        if ((appMode != 1 && appMode != 2) || !sample.valid) {
            Reset();
            return;
        }
        if (havePrevious_ && appMode != appMode_) Reset();
        appMode_ = appMode;
        if (havePrevious_) {
            const bool identityChanged = sample.round != previous_.round ||
                sample.activeCharacter[0] != previous_.activeCharacter[0] ||
                sample.activeCharacter[1] != previous_.activeCharacter[1];
            const bool duplicate = sample.trueFrame == previous_.trueFrame &&
                sample.simulationFrame == previous_.simulationFrame;
            const bool continuous = sample.trueFrame > previous_.trueFrame &&
                sample.trueFrame - previous_.trueFrame == 1 &&
                sample.simulationFrame >= previous_.simulationFrame &&
                sample.simulationFrame - previous_.simulationFrame <= 1;
            if (identityChanged || (!duplicate && !continuous)) {
                Reset();
                previous_ = sample;
                havePrevious_ = true;
                appMode_ = appMode;
                return;
            }
            if (duplicate) return;
        }
        const uint32_t delta = havePrevious_ ? sample.simulationFrame - previous_.simulationFrame : 0;
        previous_ = sample;
        havePrevious_ = true;

        const bool p1Busy = sample.inactionable[0] != 0;
        const bool p2Busy = sample.inactionable[1] != 0;
        if (p1Busy && p2Busy) {
            // 再接触・連係で両側が再度硬直したら、前の片側先行分を破棄する。
            result_ = {FrameAdvantageState::Measuring, 0};
            pending_ = 0;
            return;
        }
        if (result_.state != FrameAdvantageState::Measuring) {
            // 新しい片側動作中に前の確定値を残さない。双方硬直まで未測定。
            if (p1Busy || p2Busy) result_ = {};
            return;
        }
        // 画面停止/片側停止/スローでゲームが進まない間は計数も確定もしない。
        if (sample.stopped || delta == 0) return;
        if (!p1Busy && !p2Busy) {
            result_ = {FrameAdvantageState::Confirmed, pending_};
        } else {
            // 有利期間が異常に長い列を丸めて確定値として出さない。
            if (pending_ <= -1000000 || pending_ >= 1000000) { Reset(); return; }
            pending_ += !p1Busy ? 1 : -1;
        }
    }

  private:
    FrameAdvantageResult result_{};
    TrainingFrameSample previous_{};
    bool havePrevious_ = false;
    int appMode_ = 0;
    int pending_ = 0;
};

} // namespace cccaster
