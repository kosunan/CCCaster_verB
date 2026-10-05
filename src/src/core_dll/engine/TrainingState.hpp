#pragma once
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include <vector>
#include <cfenv>
#include <new>

namespace cccaster::domain::session {
enum class TrainingStateEvent { None, Saved, Loaded, Empty, SaveFailed, LoadFailed, Holding, Cleared,
                                RecordingRestarted, RecordingRestartFailed, CornerReset };
// ゲームスレッド専用。通信入力・リプレイ保存・ディスクとは独立した1スロット。
class TrainingState {
  public:
    void Reset() {
        bytes_.clear(); scratch_.clear(); haveState_ = armed_ = holding_ = pending_ = transitioned_ = false;
        previous_ = 0; event_ = {}; noticeUntil_ = 0; resetInput_ = true;
        corner_ = 0; cornerHeld_ = false;
    }
    bool HasState() const { return haveState_; }
    bool Holding() const { return holding_; }
    bool AllowResetInput() const { return resetInput_; }
    bool CornerResetActive() const { return cornerHeld_ || (pending_ && corner_ != 0); }
    TrainingStateEvent Notice(int64_t now) const {
        return holding_ ? TrainingStateEvent::Holding : now < noticeUntil_ ? event_ : TrainingStateEvent::None;
    }
    TrainingStateEvent Step(int appMode, bool battle, bool configuring, uint8_t buttons,
                            const TrainingFrameSample &sample, game_interface::IGameMemory &mem, int64_t now,
                            uint16_t resetDirection = 0, int resetPlayer = 0) {
        const bool directLoad = mem.IsTrainingDummy();
        if (!(buttons & 2)) cornerHeld_ = false;
        resetInput_ = !haveState_ && !directLoad && !cornerHeld_ && !pending_;
        if (directLoad) pending_ = false;
        if (appMode != 1 || !battle) {
            const bool cleared = haveState_;
            Reset();
            return cleared ? Notify(TrainingStateEvent::Cleared, now) : TrainingStateEvent::None;
        }
        // ラウンドリセットのイントロ中は計測サンプルが無効になるが、
        // 同じ戦闘の保存スロットは維持する。遷移中の押しっぱなしは発火させない。
        if (!sample.valid) {
            if (pending_ && mem.IntroState() != 0) transitioned_ = true;
            holding_ = armed_ = false; previous_ = buttons; return {};
        }
        // 同じ戦闘内の操作キャラ交代やFN2による初期キャラ復帰では消去しない。
        // リソースを入れ替えるキャラセレクト等、戦闘外への遷移でのみ消去する。
        // ヒットストップ・技の暗転も保存できる戦闘状態。メニューの一時停止と
        // F4中だけ操作を遮断し、画面遷移前からの押しっぱなしを発火させない。
        if (configuring || sample.paused) {
            // 通常FN2のリセット自身も一時停止を挟むため、そこで予約を捨てない。
            if (configuring) pending_ = false;
            holding_ = armed_ = false; previous_ = buttons; return {};
        }
        // 所有する一時停止は呼出元がサンプル読取り前に解除する。
        // FN1を離す・FN2を押す・機器切断・F4で確実に停止を解除する。
        if (!(buttons & 1) || (buttons & 2)) holding_ = false;
        if (pending_) {
            if (now > resetUntil_) pending_ = false;
            else if (transitioned_ || sample.trueFrame < resetTrueFrame_ ||
                     sample.simulationFrame < resetSimulationFrame_) {
                pending_ = false;
                previous_ = buttons; armed_ = buttons == 0;
                if (corner_) {
                    mem.PlaceTrainingCorner(corner_, resetPlayer_);
                    return Notify(TrainingStateEvent::CornerReset, now);
                }
                if (mem.LoadTrainingSnapshot(bytes_)) {
                    std::fesetenv(&fp_);
                    return Notify(TrainingStateEvent::Loaded, now);
                }
                return Notify(TrainingStateEvent::LoadFailed, now);
            }
        }
        if (!buttons) armed_ = true;
        const auto edge = buttons & ~previous_;
        previous_ = buttons;
        if (!armed_ || !edge) return {};
        TrainingStateEvent result = TrainingStateEvent::None;
        if ((edge & 2) && directLoad) { // 敵の設定がDUMMYの時だけ、押下時に直接ロードする。
            armed_ = false;
            if (!haveState_) {
                // 保存がなくても、録画中なら現在位置から録画をやり直せる。
                if (mem.IsTrainingRecording())
                    result = mem.RestartTrainingRecording() ? TrainingStateEvent::RecordingRestarted
                                                           : TrainingStateEvent::RecordingRestartFailed;
                else result = TrainingStateEvent::Empty;
            }
            else if (mem.LoadTrainingSnapshot(bytes_)) {
                std::fesetenv(&fp_);
                result = TrainingStateEvent::Loaded;
            } else {
                result = TrainingStateEvent::LoadFailed;
            }
        } else if (edge & 2) {
            resetInput_ = true;
            if (!pending_) {
                // 水平方向はFN2を押した本人の入力を採用し、リセット完了まで固定する。
                // DUMMY/録画は従来の直接ロードを優先する。
                corner_ = resetDirection == 4 ? -1 : resetDirection == 6 ? 1 : 0;
                resetPlayer_ = resetPlayer == 1 ? 1 : 0;
                cornerHeld_ = corner_ != 0;
            }
            if ((haveState_ || corner_) && !pending_) {
                pending_ = true; transitioned_ = false;
                resetTrueFrame_ = sample.trueFrame; resetSimulationFrame_ = sample.simulationFrame;
                resetUntil_ = now + 10000000;
            }
        } else if ((edge & 1) && !pending_) {
            const auto size = mem.SupportsSnapshots() ? mem.TrainingSnapshotSize() : 0;
            try {
                if (!size || size > 16 * 1024 * 1024) result = TrainingStateEvent::SaveFailed;
                else {
                    scratch_.resize(size);
                    if (mem.SaveTrainingSnapshot(scratch_)) {
                        bytes_.swap(scratch_); std::fegetenv(&fp_);
                        haveState_ = holding_ = true; result = TrainingStateEvent::Saved;
                    } else result = TrainingStateEvent::SaveFailed;
                }
            } catch (const std::bad_alloc &) { result = TrainingStateEvent::SaveFailed; }
        }
        return Notify(result, now);
    }
    static const char *Text(TrainingStateEvent event) {
        switch (event) {
        case TrainingStateEvent::Saved: return "STATE SAVED";
        case TrainingStateEvent::Holding: return "STATE SAVED - RELEASE FN1 TO RESUME";
        case TrainingStateEvent::Cleared: return "SAVED STATE CLEARED";
        case TrainingStateEvent::Loaded: return "STATE LOADED";
        case TrainingStateEvent::Empty: return "NO SAVED STATE - SAVE FIRST";
        case TrainingStateEvent::SaveFailed: return "SAVE FAILED - PREVIOUS STATE KEPT";
        case TrainingStateEvent::LoadFailed: return "LOAD FAILED - SAVED STATE KEPT";
        case TrainingStateEvent::RecordingRestarted: return "RECORDING RESTARTED";
        case TrainingStateEvent::RecordingRestartFailed: return "RECORDING RESTART FAILED";
        case TrainingStateEvent::CornerReset: return "CORNER RESET";
        default: return "";
        }
    }
  private:
    TrainingStateEvent Notify(TrainingStateEvent event, int64_t now) {
        if (event != TrainingStateEvent::None) { event_ = event; noticeUntil_ = now + 4000000; }
        return event;
    }
    std::vector<char> bytes_, scratch_;
    std::fenv_t fp_{};
    bool haveState_ = false, armed_ = false;
    bool holding_ = false, pending_ = false, transitioned_ = false, resetInput_ = true;
    uint32_t resetTrueFrame_ = 0, resetSimulationFrame_ = 0;
    int64_t resetUntil_ = 0;
    uint8_t previous_ = 0;
    int corner_ = 0, resetPlayer_ = 0;
    bool cornerHeld_ = false;
    TrainingStateEvent event_{};
    int64_t noticeUntil_ = 0;
};
} // namespace cccaster::domain::session
