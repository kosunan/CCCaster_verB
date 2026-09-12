#pragma once
// ============================================================================
// FakeGame — スクリプトされた MBAA
//
// 【役割】
//   実ゲームの代わりに IGameMemory として振る舞い、決められたフレーム数で
//   画面を進める。入力に反応はしない（タイムライン駆動）。
//   ゲーム側の挙動を再現することが目的ではなく、
//   「同期ロジックが画面遷移にどう反応するか」を再現可能にすることが目的。
//
// 【なぜタイムライン駆動か】
//   入力に反応する忠実な模倣を作ると、それ自体が検証対象のない新しいコードに
//   なってしまう。ロード時間を左右で変える等の操作が効けば症状は再現できる。
//
// 【記録】
//   WriteInput() で書き込まれた入力を全フレーム記録する。
//   この記録列を2プロセス間で突き合わせるのが決定性テスト(B-4)の判定材料。
// ============================================================================

#include "core_dll/mbaa_mem/IGameMemory.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cccaster::harness {

class FakeGame final : public cccaster::game_interface::IGameMemory {
  public:
    using GameInput = cccaster::game_interface::GameInput;

    /// 各画面の滞在フレーム数。既定値は 2026-07-27 の実機トレース
    /// (CCCASTER_MEM_TRACE) から measured した値。机上の値だと harness が
    /// 実機と違う挙動を示し、嘘の安心を与える。
    ///   CharaSelect 456〜592F / Loading 55〜60F / intro=2 138F /
    ///   intro=1 224F / ラウンド 1761F
    /// intro=1 の 224F は MbaaAddresses.hpp の CC_PRE_GAME_INTRO_FRAMES と一致。
    /// loadingFrames を左右で変えるとロード時間のばらつきを再現できる。
    struct Script {
        uint32_t mainMenuFrames = 45;
        uint32_t charaSelectFrames = 456;
        uint32_t loadingFrames = 55;
        uint32_t introPlayFrames = 138; ///< introState=2 の長さ
        uint32_t introPreFrames = 224;  ///< introState=1 の長さ
        uint32_t roundFrames = 1761;    ///< introState=0 の長さ
        uint32_t rematchFrames = 120;
        int rounds = 2;
    };

    enum class Stage { Boot, CharaSelect, Loading, InGame, Rematch, Finished };

    struct Record {
        uint32_t frame;    ///< FakeGame 自身のフレーム（プロセスごとに異なる）
        uint32_t netFrame; ///< 配信されたネットプレイフレーム（両者で一致すべき）
        uint32_t gameMode;
        uint8_t introState;
        GameInput p1;
        GameInput p2;
    };

    explicit FakeGame(const Script &script);
    // harnessは乱数で戦闘を模擬しない。転送経路用の状態のみ保持する。
    bool ReadRng(cccaster::game_interface::RngState &state) const override {
        state = _rng;
        return true;
    }
    bool WriteRng(const cccaster::game_interface::RngState &state) override {
        _rng = state;
        return true;
    }

    /// 1フレーム進める。SceneRunner::Step() の前に呼ぶ。
    void Advance();

    bool IsFinished() const {
        return _stage == Stage::Finished;
    }
    Stage CurrentStage() const {
        return _stage;
    }
    const char *StageName() const;
    uint32_t Frame() const {
        return _frame;
    }

    const std::vector<Record> &Written() const {
        return _written;
    }

    /// 実機の [MEM] トレースと同じ形でゲーム状態を記録する。
    /// 同じ netFrame で両プロセスの状態が一致するかを判定するために使う。
    struct StateSample {
        uint32_t netFrame;
        uint32_t gameMode;
        uint8_t introState;
        uint32_t worldTimer;
        uint32_t realTimer;
    };
    void SampleState(uint32_t netFrame);
    const std::vector<StateSample> &States() const {
        return _states;
    }
    bool DumpStatesTo(const std::string &path) const;

    /// 記録を1行1フレームのテキストで書き出す。突き合わせはこのファイルで行う。
    bool DumpTo(const std::string &path) const;

    // ── IGameMemory ──
    bool IsAvailable() const override {
        return true;
    }
    uint32_t GameMode() const override {
        return _gameMode;
    }
    uint8_t IntroState() const override {
        return _introState;
    }
    uint32_t WorldTimer() const override {
        return _worldTimer;
    }
    uint32_t RealTimer() const override {
        return _realTimer;
    }
    uint32_t MenuStateCounter() const override {
        return _menuStateCounter;
    }
    void WriteInput(GameInput p1, GameInput p2) override;

  private:
    cccaster::game_interface::RngState _rng{};
    void EnterStage(Stage next);

    Script _script;
    Stage _stage = Stage::Boot;
    uint32_t _framesInStage = 0;
    int _roundsPlayed = 0;

    uint32_t _frame = 0;
    uint32_t _gameMode = 0;
    uint8_t _introState = 0;
    uint32_t _worldTimer = 0;
    uint32_t _realTimer = 0;
    uint32_t _menuStateCounter = 0;

    std::vector<Record> _written;
    std::vector<StateSample> _states;
};

} // namespace cccaster::harness
