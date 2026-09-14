#pragma once
#include <cstdint>

namespace cccaster::domain::session {

// ラウンド勝数。試合終了画面に到達し入力訂正が完了した時だけ勝敗判定に使う。
struct MatchResultFacts {
    uint32_t p1Rounds = 0, p2Rounds = 0, roundsToWin = 0;
    bool available = false;
};

struct SessionScoreSnapshot {
    uint32_t p1Wins = 0, p2Wins = 0, unresolved = 0;
    uint64_t revision = 0, lastMatchGeneration = 0;
    bool active = false;
    bool operator==(const SessionScoreSnapshot &) const = default;
};

enum class ScoreScene { Battle, Result, CharacterSelect, Other };

class SessionScore {
  public:
    void Reset(bool enabled) {
        state_ = {};
        state_.active = enabled;
        armed_ = false;
        startedGeneration_ = closedGeneration_ = 0;
    }

    // ゲームスレッド専用。confirmed は境界の全入力到達・訂正再計算完了を指す。
    // 再計算中の中間画面や、未確定の予測結果からは呼ばない。
    bool Observe(ScoreScene scene, uint64_t generation, const MatchResultFacts &facts,
                 bool confirmed) {
        if (!state_.active || !confirmed || generation == 0 || generation < closedGeneration_)
            return false;
        if (scene == ScoreScene::CharacterSelect) {
            armed_ = false; // 中断・選び直しを勝利にしない。セッション勝数は維持。
            return false;
        }
        if (scene == ScoreScene::Battle) {
            if (!armed_ && generation > closedGeneration_) {
                armed_ = true;
                startedGeneration_ = generation;
            }
            return false; // ラウンド勝数の増加自体では集計しない。
        }
        if (scene != ScoreScene::Result || !armed_)
            return false;
        armed_ = false;
        closedGeneration_ = generation;
        state_.lastMatchGeneration = startedGeneration_;
        // 正規の最大ラウンド設定を越える不正値や両者到達は未裁定。
        // 同時KO、引分、切断をHPや大小比較から推測しない。
        const bool valid = facts.available && facts.roundsToWin >= 1 && facts.roundsToWin <= 5 &&
            facts.p1Rounds <= facts.roundsToWin && facts.p2Rounds <= facts.roundsToWin;
        const bool p1 = valid && facts.p1Rounds == facts.roundsToWin;
        const bool p2 = valid && facts.p2Rounds == facts.roundsToWin;
        if (p1 != p2) {
            if (p1) ++state_.p1Wins; else ++state_.p2Wins;
        } else {
            ++state_.unresolved;
        }
        ++state_.revision;
        return true;
    }

    const SessionScoreSnapshot &Snapshot() const { return state_; }

  private:
    SessionScoreSnapshot state_{};
    bool armed_ = false;
    uint64_t startedGeneration_ = 0, closedGeneration_ = 0;
};

} // namespace cccaster::domain::session
