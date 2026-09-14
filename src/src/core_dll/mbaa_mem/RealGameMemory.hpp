#pragma once
/**
 * @file RealGameMemory.hpp
 * @brief MBAA プロセスの実メモリに読み書きする IGameMemory 実装
 *
 * DLL 側でのみ使う。DLL 初期化時に InstallRealGameMemory() を1回呼ぶ。
 */

#include "core_dll/mbaa_mem/IGameMemory.hpp"

namespace cccaster::game_interface {

class RealGameMemory final : public IGameMemory {
  public:
    bool IsAvailable() const override;
    uint32_t GameMode() const override;
    uint8_t IntroState() const override;
    uint32_t WorldTimer() const override;
    uint32_t RealTimer() const override;
    uint32_t MenuStateCounter() const override;
    domain::session::MatchResultFacts ReadMatchResult() const override;
    TrainingFrameSample ReadTrainingFrame() const override;
    void WriteInput(GameInput p1, GameInput p2) override;
    void SetTrainingHold(bool) override;
    bool ConfigureNetplayMenu() override;
    std::string ReplayFilePath() const override;
    bool SaveReplay(const char *p1, const char *p2, int winner) override;
    void SetRetryTarget(int) override;
    bool HasIndependentRetry() const override { return true; }
    void BeginIndependentRetry() override;
    int ReadRetryChoice() const override;
    bool HasIndependentSelect() const override { return true; }
    bool BeginIndependentSelect(bool) override;
    bool ReadLocalSelection(bool, core::sync::SelectionState &) override;
    GameInput DriveRemoteSelection(bool, const core::sync::SelectionState &, uint32_t) override;
    void SetSelectionRelease(bool, uint32_t) override;
    std::array<uint32_t, 3> SpectatorRules() const override;
    bool SetSpectatorRules(const std::array<uint32_t, 3> &) override;
    bool BeginReplay(uint32_t, uint32_t) override;
    void EndReplay() override;
    void BeginSimulation(uint32_t) override;
    bool PrepareBattleAudio() override;
    bool CanPredict() const override;
    bool CanRollback() const override;
    void AlignIntroRng() override;
    bool SupportsSnapshots() const override {
        return true;
    }
    size_t SnapshotSize() const override;
    bool SaveSnapshot(std::span<char>) override;
    bool LoadSnapshot(std::span<char>) override;
    bool ReadRng(RngState &state) const override;
    bool WriteRng(const RngState &state) override;
  private:
    bool trainingHold_ = false;
    uint32_t trainingFreezeBefore_ = 0, trainingFreezeActiveBefore_ = 0;
};

/// 実メモリ実装を設置する。DLL 初期化の最初期に1回だけ呼ぶ。
void InstallRealGameMemory();

} // namespace cccaster::game_interface
