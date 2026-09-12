#pragma once
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include <vector>
#include <cfenv>
#include <new>

namespace cccaster::domain::session {
enum class TrainingStateEvent { None, Saved, Loaded, Empty, SaveFailed, LoadFailed };
// ゲームスレッド専用。通信入力・リプレイ保存・ディスクとは独立した1スロット。
class TrainingState {
  public:
    void Reset() { bytes_.clear(); scratch_.clear(); haveState_ = armed_ = false; previous_ = 0; event_ = {}; }
    bool HasState() const { return haveState_; }
    TrainingStateEvent Notice(int64_t now) const { return now < noticeUntil_ ? event_ : TrainingStateEvent::None; }
    TrainingStateEvent Step(int appMode, bool battle, bool configuring, uint8_t buttons,
                            const TrainingFrameSample &sample, game_interface::IGameMemory &mem, int64_t now) {
        if (appMode != 1 || !battle || !sample.valid) { Reset(); return {}; }
        if (haveState_ && (sample.round != identity_.round ||
            sample.activeCharacter[0] != identity_.activeCharacter[0] ||
            sample.activeCharacter[1] != identity_.activeCharacter[1])) {
            haveState_ = false; bytes_.clear();
        }
        // F4中・停止中・画面遷移前からの押しっぱなしを発火させない。
        if (configuring || sample.stopped) { armed_ = false; previous_ = buttons; return {}; }
        if (!buttons) armed_ = true;
        const auto edge = buttons & ~previous_;
        previous_ = buttons;
        if (!armed_ || !edge) return {};
        TrainingStateEvent result = TrainingStateEvent::None;
        if (edge & 2) { // 重複した保存・読込は読込優先。
            if (!haveState_) result = TrainingStateEvent::Empty;
            else if (mem.LoadSnapshot(bytes_)) {
                std::fesetenv(&fp_);
                result = TrainingStateEvent::Loaded;
            } else {
                // 無効になったリソースの古い保存状態を繰り返し適用しない。
                haveState_ = false; result = TrainingStateEvent::LoadFailed;
            }
        } else if (edge & 1) {
            const auto size = mem.SupportsSnapshots() ? mem.SnapshotSize() : 0;
            try {
                if (!size || size > 16 * 1024 * 1024) result = TrainingStateEvent::SaveFailed;
                else {
                    scratch_.resize(size);
                    if (mem.SaveSnapshot(scratch_)) {
                        bytes_.swap(scratch_); std::fegetenv(&fp_);
                        identity_ = sample; haveState_ = true; result = TrainingStateEvent::Saved;
                    } else result = TrainingStateEvent::SaveFailed;
                }
            } catch (const std::bad_alloc &) { result = TrainingStateEvent::SaveFailed; }
        }
        if (result != TrainingStateEvent::None) { event_ = result; noticeUntil_ = now + 4000000; }
        return result;
    }
    static const char *Text(TrainingStateEvent event) {
        switch (event) {
        case TrainingStateEvent::Saved: return "STATE SAVED";
        case TrainingStateEvent::Loaded: return "STATE LOADED";
        case TrainingStateEvent::Empty: return "NO SAVED STATE - SAVE FIRST";
        case TrainingStateEvent::SaveFailed: return "SAVE FAILED - PREVIOUS STATE KEPT";
        case TrainingStateEvent::LoadFailed: return "LOAD FAILED - SAVE A NEW STATE";
        default: return "";
        }
    }
  private:
    std::vector<char> bytes_, scratch_;
    std::fenv_t fp_{};
    TrainingFrameSample identity_{};
    bool haveState_ = false, armed_ = false;
    uint8_t previous_ = 0;
    TrainingStateEvent event_{};
    int64_t noticeUntil_ = 0;
};
} // namespace cccaster::domain::session
