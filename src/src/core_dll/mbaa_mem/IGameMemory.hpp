#pragma once
/**
 * @file IGameMemory.hpp
 * @brief ゲームメモリへの読み書き口
 *
 * 【なぜ挟むか】
 *   同期ロジックが `*CC_XXX_ADDR` を直接触っている限り、MBAA を起動しないと
 *   一行も検証できない。フェーズ遷移・IntroBarrier・Rematch の状態機械は
 *   すべてこの読み書きの上に乗っているため、ここを差し替え可能にすると
 *   ゲーム無しで回せるようになる。
 *
 * 【実装は2つだけ】
 *   RealGameMemory (core_dll)  — 実アドレスへの読み書き
 *   FakeGameMemory (src/tests) — 値を明示指定し、書き込まれた入力を記録する
 *
 * 【呼び出しコスト】
 *   仮想呼び出し1回あたり約1-2ns。毎フレーム10回でも 1F(16.7ms) の 0.0001%。
 *   本プロジェクトが禁じている「無駄なコピー・ロック待機」には当たらない。
 *
 * 【seam に含めないもの】
 *   FastBoot のコード書換 (CC_FORCE_GOTO_ADDR) と SFX 配列クリア、
 *   起動時1回の MbaaPatcher、ステート保存 (DumpEntryList) は対象外。
 *   テストでは FastBoot 自体をスキップする。
 */

#include <cstdint>
#include <span>
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/RngState.hpp"
#include "core_dll/sync/SelectionState.hpp"
#include "core_dll/engine/SessionScore.hpp"
#include "core_dll/mbaa_mem/TrainingMetrics.hpp"

namespace cccaster::game_interface {

class IGameMemory {
  public:
    virtual ~IGameMemory() = default;

    /// ゲームのメモリがまだマップされておらず読めない状態を区別する。
    /// 起動直後やプロセス終了間際に false になりうる。
    virtual bool IsAvailable() const = 0;

    // ── 読み取り ──
    virtual uint32_t GameMode() const = 0;         ///< CC_GAME_MODE_*
    virtual uint8_t IntroState() const = 0;        ///< 2=イントロ演出中 1=pre-game 0=対戦進行中
    virtual uint32_t WorldTimer() const = 0;       ///< 常時カウントアップ
    virtual uint32_t RealTimer() const = 0;        ///< ラウンド開始後にカウントアップ
    virtual uint32_t MenuStateCounter() const = 0; ///< メニュー階層のスタック深度
    virtual domain::session::MatchResultFacts ReadMatchResult() const { return {}; }
    virtual TrainingFrameSample ReadTrainingFrame() const { return {}; }

    // ── 書き込み ──
    virtual void WriteInput(GameInput p1, GameInput p2) = 0;
    // Training保存ボタンの押下中だけ。所有した停止は必ずfalseで解除する。
    virtual void SetTrainingHold(bool) {}
    virtual bool ConfigureNetplayMenu() {
        return true;
    }
    virtual void SetRetryTarget(int) {}
    virtual bool HasIndependentRetry() const { return false; }
    virtual void BeginIndependentRetry() {}
    virtual int ReadRetryChoice() const { return -1; }
    virtual bool HasIndependentSelect() const { return false; }
    virtual bool BeginIndependentSelect(bool) { return false; }
    virtual bool ReadLocalSelection(bool, core::sync::SelectionState &) { return false; }
    virtual GameInput DriveRemoteSelection(bool, const core::sync::SelectionState &, uint32_t) { return {}; }
    virtual void SetSelectionRelease(bool, uint32_t) {}
    virtual std::array<uint32_t, 3> SpectatorRules() const { return {2, 2, 2}; }
    virtual bool SetSpectatorRules(const std::array<uint32_t, 3> &rules) {
        return rules[0] >= 1 && rules[0] <= 5 && rules[1] <= 4 && rules[2] <= 4;
    }
    virtual bool BeginReplay(uint32_t, uint32_t) {
        return true;
    }
    virtual void EndReplay() {}
    virtual void BeginSimulation(uint32_t) {}
    virtual bool PrepareBattleAudio() { return true; }
    virtual bool CanPredict() const {
        return false;
    }
    // 攻撃可能な戦闘判定と分離。実ゲームではイントロの保存・再計算も許す。
    virtual bool CanRollback() const { return CanPredict(); }
    virtual void AlignIntroRng() {}
    virtual bool SupportsSnapshots() const {
        return false;
    }
    virtual size_t SnapshotSize() const {
        return 0;
    }
    virtual bool SaveSnapshot(std::span<char>) {
        return false;
    }
    virtual bool LoadSnapshot(std::span<char>) {
        return false;
    }
    virtual bool ReadRng(RngState &) const {
        return false;
    }
    virtual bool WriteRng(const RngState &) {
        return false;
    }
};

/// プロセス起動時に1回だけ差し込む。nullptr を渡すと未設置状態に戻る。
void InstallGameMemory(IGameMemory *impl);

/// 現在設置されている実装。未設置なら値を返さず書込みも捨てる実装が返る。
IGameMemory &GameMem();

} // namespace cccaster::game_interface
