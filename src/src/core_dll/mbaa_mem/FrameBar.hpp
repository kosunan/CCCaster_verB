#pragma once
#include "core_dll/mbaa_mem/TrainingMetrics.hpp"
#include <array>
#include <cstddef>
#include <limits>

namespace cccaster {
// 主状態と攻撃/ヒットストップはkosunan/MBAACC_Training b9a74cbの独立した2層。
enum class FrameBarState : uint8_t { Ready, Busy, Stun, Jump, Shield, Clash, Invulnerable, StartWait };
enum class FrameBarSignal : uint8_t { None, Attack, Hitstop };
inline const char *FrameBarStateName(FrameBarState state) {
    switch (state) {
    case FrameBarState::Ready: return "READY";
    case FrameBarState::Busy: return "BUSY";
    case FrameBarState::Stun: return "STUN";
    case FrameBarState::Jump: return "JUMP";
    case FrameBarState::Shield: return "SHIELD";
    case FrameBarState::Clash: return "CLASH";
    case FrameBarState::Invulnerable: return "INVULN";
    case FrameBarState::StartWait: return "START";
    }
    return "?";
}
inline const char *FrameBarSignalName(FrameBarSignal signal) {
    return signal == FrameBarSignal::Hitstop ? "STOP" : signal == FrameBarSignal::Attack ? "ATK" : "NONE";
}
struct FrameBarCell {
    FrameBarState state = FrameBarState::Ready;
    FrameBarSignal signal = FrameBarSignal::None;
    bool stopped = false;
    bool busy = false;
    bool strikeInvulnerable = false;
    bool activeBoxes = false;
    uint32_t runFrame = 0, signalFrame = 0; // 独立した色の区間番号。
    bool boundary = false, timerSuppressed = false, slow = false;
    uint32_t pattern = 0;
    int32_t busyCounter = 0;
    TrainingActorDetail detail{};
};
struct FrameBarColumn {
    std::array<FrameBarCell, 2> players{};
    uint32_t trueFrame = 0;
};

// Fn_04_Cui_cnt.get_color_code / Cf_02_state_numの主状態と優先順。
// remaining_motion_framesは残りFでなく補助側+0x208の行動不能経過カウンター。
// attack_dataはActor+0x31C -> animation+0x42。10/12はシールド/相殺の配列形式。
// 矩形の接触成功・無敵資格の完全判定ではない。未知の読取りを0件に変換しない。
inline FrameBarState ReferenceFrameBarState(int32_t inactionable, uint32_t pattern,
                                           const TrainingActorDetail &detail) {
    if (!inactionable) return pattern == 350 ? FrameBarState::Stun : FrameBarState::Ready;
    if (detail.animationKnown && detail.defenseSlotCount == 10) return FrameBarState::Shield;
    if (detail.animationKnown && detail.defenseSlotCount == 12) return FrameBarState::Clash;
    switch (pattern) {
    case 17: case 18: case 19: case 26: case 29: case 30: case 350: case 354:
    case 900: case 901: case 902: case 903: case 904: case 905: case 906: case 907: case 908:
        return FrameBarState::Stun;
    case 34: case 35: case 36: case 37: case 38: case 39: case 40: case 54: case 476:
        return FrameBarState::Jump;
    }
    if ((detail.animationKnown && detail.defenseSlotCount <= 1) || detail.strikeProtected)
        return FrameBarState::Invulnerable;
    switch (pattern) {
    case 0: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 20: case 594:
        return FrameBarState::Ready;
    default: return FrameBarState::Busy;
    }
}
inline bool FrameBarRecoveryEligible(const TrainingFrameSample &s, unsigned side) {
    const auto &d = s.detail[side];
    return !s.inactionable[side] && !s.attacking[side] && !s.blockstun[side] &&
        d.valid && !d.thrown && !d.stunRemaining && d.stanceKnown && d.canMove == 1;
}
inline FrameBarCell ClassifyFrameBar(const TrainingFrameSample &s, unsigned side) {
    const auto &detail = s.detail[side];
    FrameBarCell cell;
    cell.detail = detail;
    cell.pattern = s.pattern[side];
    cell.busyCounter = s.inactionable[side];
    cell.busy = s.inactionable[side] != 0;
    cell.state = ReferenceFrameBarState(cell.busyCounter, cell.pattern, detail);
    cell.strikeInvulnerable = detail.strikeProtected ||
        (detail.hurtBoxesKnown && detail.hurtBoxCount == 0);
    // 参照のcolor2と同じ。属性と実矩形は別々に残し、STOP中も矩形を失わない。
    cell.signal = detail.hitstop ? FrameBarSignal::Hitstop :
        s.attacking[side] ? FrameBarSignal::Attack : FrameBarSignal::None;
    cell.activeBoxes = s.attacking[side] && detail.attackBoxesKnown && detail.attackBoxCount != 0;
    cell.stopped = s.globalFreeze || s.playerStopped[side];
    cell.timerSuppressed = s.timerSuppressed;
    // 行動開始前の予約だけでは濃い緑を付けない。復帰境界はHistoryで判定する。
    return cell;
}

class FrameBarHistory {
  public:
    static constexpr size_t Capacity = 45;
    static constexpr unsigned IdleTailFrames = 5;
    void Reset() { *this = FrameBarHistory{}; }
    size_t Size() const { return size_; }
    bool Holding() const { return idle_ >= IdleTailFrames; }
    const FrameBarColumn &At(size_t i) const { return columns_[(head_ + Capacity - size_ + i) % Capacity]; }

    // 1セル=実際に観測したtrueFrameの1更新。停止も独立した印を付けて残す。
    // 描画や壁時計からの補間は禁止。ロード/欠測/操作交代は履歴を破棄する。
    void Update(int mode, const TrainingFrameSample &s) {
        if ((mode != 1 && mode != 2 && mode != 4) || !s.valid) { Reset(); return; }
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
        const bool slow = havePrevious_ && s.trueFrame > previous_.trueFrame &&
            s.simulationFrame == previous_.simulationFrame;
        bool recovered[2]{};
        for (unsigned side = 0; side < 2; ++side)
            recovered[side] = havePrevious_ && !previous_.paused && !s.paused && !slow &&
                previous_.inactionable[side] != 0 && s.inactionable[side] == 0;
        mode_ = mode;
        previous_ = s;
        havePrevious_ = true;
        if (s.paused) return;
        FrameBarColumn column;
        column.trueFrame = s.trueFrame;
        bool active = false;
        for (unsigned side = 0; side < 2; ++side) {
            column.players[side] = ClassifyFrameBar(s, side);
            auto &cell = column.players[side];
            cell.slow = slow;
            // 0x461830で硬直解消→コマンド予約→次の0x4618C0で動作開始。
            // 入力受付が戻った境界1Fだけ濃い緑にする。行動の初めの予約は対象外。
            // 予約の有無はホバーに残す。欠測や停止から復帰境界を推定しない。
            if (recovered[side] && cell.state == FrameBarState::Ready && !cell.stopped &&
                FrameBarRecoveryEligible(s, side))
                cell.state = FrameBarState::StartWait;
            active |= cell.state != FrameBarState::Ready || cell.signal != FrameBarSignal::None || cell.stopped ||
                cell.strikeInvulnerable || cell.detail.throwProtected || cell.detail.airborne;
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
            // READY内の歩行では区切らず、同色でも次の動作なら番号を再始動する。
            const bool actionChanged = cell.state != FrameBarState::Ready &&
                (cell.pattern != previous.pattern || (cell.detail.valid && previous.detail.valid &&
                 cell.detail.patternFrame < previous.detail.patternFrame));
            cell.boundary = !size_ || previous.state != cell.state || actionChanged ||
                previous.strikeInvulnerable != cell.strikeInvulnerable;
            cell.signalFrame = cell.signal == FrameBarSignal::None ? 0 :
                size_ && previous.signal == cell.signal
                    ? (previous.signalFrame == std::numeric_limits<uint32_t>::max()
                        ? previous.signalFrame : previous.signalFrame + 1) : 1;
            cell.runFrame = !cell.boundary
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
