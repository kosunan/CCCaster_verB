#pragma once
#include "core_dll/mbaa_mem/TrainingMetrics.hpp"
#include <array>
#include <cstddef>
#include <limits>

namespace cccaster {
enum class FrameBarState : uint8_t { Ready, Busy, Active, Blockstun, Hitstun };
struct FrameBarCell {
    FrameBarState state = FrameBarState::Ready;
    bool stopped = false;
    bool busy = false;
    uint32_t runFrame = 0; // 同じ主状態が連続している何F目か。リングを越えて保持する。
};
struct FrameBarColumn { std::array<FrameBarCell, 2> players{}; };

// kosunan版のattack_status (attackDataPtr != 0)と動作番号を参照。
// 発生前/後隙は動作番号だけでは断定できないため、行動不能としてまとめる。
inline FrameBarCell ClassifyFrameBar(const TrainingFrameSample &s, unsigned side) {
    const auto pattern = s.pattern[side];
    const bool hit = pattern == 26 || pattern == 29 || pattern == 30 ||
        pattern == 350 || pattern == 354 || (pattern >= 900 && pattern <= 908);
    FrameBarCell cell;
    cell.busy = s.inactionable[side] != 0 || pattern == 350;
    cell.stopped = s.globalFreeze || s.playerStopped[side];
    cell.state = s.attacking[side] ? FrameBarState::Active
        : s.blockstun[side] ? FrameBarState::Blockstun
        : cell.busy && hit ? FrameBarState::Hitstun
        : cell.busy ? FrameBarState::Busy : FrameBarState::Ready;
    return cell;
}

class FrameBarHistory {
  public:
    static constexpr size_t Capacity = 45;
    void Reset() { *this = FrameBarHistory{}; }
    size_t Size() const { return size_; }
    bool Holding() const { return idle_ >= 15; }
    const FrameBarColumn &At(size_t i) const { return columns_[(head_ + Capacity - size_ + i) % Capacity]; }

    // 1セル=実際に観測したtrueFrameの1更新。停止も独立した印を付けて残す。
    // 描画や壁時計からの補間は禁止。ロード/欠測/操作交代は履歴を破棄する。
    void Update(int mode, const TrainingFrameSample &s) {
        if ((mode != 1 && mode != 2) || !s.valid) { Reset(); return; }
        if (havePrevious_) {
            const bool identity = mode == mode_ && s.round == previous_.round &&
                s.activeCharacter[0] == previous_.activeCharacter[0] &&
                s.activeCharacter[1] == previous_.activeCharacter[1];
            const bool duplicate = s.trueFrame == previous_.trueFrame &&
                s.simulationFrame == previous_.simulationFrame;
            const bool continuous = s.trueFrame > previous_.trueFrame &&
                s.trueFrame - previous_.trueFrame == 1 &&
                s.simulationFrame >= previous_.simulationFrame &&
                s.simulationFrame - previous_.simulationFrame <= 1;
            if (!identity || (!duplicate && !continuous)) Reset();
            else if (duplicate) return;
        }
        mode_ = mode;
        previous_ = s;
        havePrevious_ = true;
        if (s.paused) return;
        FrameBarColumn column;
        bool active = false;
        for (unsigned side = 0; side < 2; ++side) {
            column.players[side] = ClassifyFrameBar(s, side);
            active |= column.players[side].state != FrameBarState::Ready || column.players[side].stopped;
        }
        if (active) {
            if (Holding()) { size_ = head_ = 0; }
            idle_ = 0;
        } else {
            if (!size_ || Holding()) return;
            ++idle_;
        }
        for (unsigned side = 0; side < 2; ++side) {
            auto &cell = column.players[side];
            const auto previous = size_ ? At(size_ - 1).players[side] : FrameBarCell{};
            cell.runFrame = size_ && previous.state == cell.state
                ? (previous.runFrame == std::numeric_limits<uint32_t>::max()
                    ? previous.runFrame : previous.runFrame + 1)
                : 1;
        }
        columns_[head_] = column;
        head_ = (head_ + 1) % Capacity;
        if (size_ < Capacity) ++size_;
    }
  private:
    std::array<FrameBarColumn, Capacity> columns_{};
    TrainingFrameSample previous_{};
    size_t head_ = 0, size_ = 0;
    unsigned idle_ = 0;
    int mode_ = 0;
    bool havePrevious_ = false;
};
} // namespace cccaster
