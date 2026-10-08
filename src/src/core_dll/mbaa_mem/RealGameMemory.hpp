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
    bool IsPauseMenuOpen() const override;
    bool IsTrainingDummy() const override;
    bool IsTrainingRecording() const override;
    bool RestartTrainingRecording() override;
    bool ConfigureTrainingMenu() override;
    bool StepTrainingMenu(GameInput&, GameInput&, bool) override;
    int StageAnimation() const override;
    bool SetStageAnimation(bool) override;
    int DisplayOption(NativeDisplayOption) const override;
    bool SetDisplayOption(NativeDisplayOption, int) override;
    ScreenResolution RenderResolution() const override;
    bool ChangeRenderResolution(int) override;
    bool SetRenderResolution(int, int) override;
    bool SelectionDelayEditable(bool) const override;
    void WriteInput(GameInput p1, GameInput p2) override;
    void SetTrainingHold(bool) override;
    void PlaceTrainingCorner(int direction, int player) override;
    bool ConfigureNetplayMenu() override;
    bool ConfigureRandomStages() override;
    uint32_t DrawRandomStage(uint32_t previousStage) override;
    std::string ReplayFilePath() const override;
    bool SaveReplay(const char *p1, const char *p2, int winner) override;
    void SetRetryTarget(int) override;
    bool SetStageRematchFastPath(bool) override;
    bool CommitStageRematch(uint32_t) override;
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
    bool ConfigureInputWriteMonitor(bool) override;
    cccaster::sync::InputWriteHistory* InputWrites() override;
    bool PrepareBattleAudio() override;
    bool SetIntroPreview(bool) override;
    bool CanPredict() const override;
    bool CanRollback() const override;
    void AlignIntroRng() override;
    bool SupportsSnapshots() const override {
        return true;
    }
    size_t SnapshotSize() const override;
    bool SaveSnapshot(std::span<char>) override;
    bool LoadSnapshot(std::span<char>) override;
    size_t PresentationSnapshotSize() const override;
    bool SavePresentationSnapshot(std::span<char>) override;
    bool LoadPresentationSnapshot(std::span<char>) override;
    void SetPresentationPreview(bool) override;
    size_t TrainingSnapshotSize() const override;
    bool SaveTrainingSnapshot(std::span<char>) override;
    bool LoadTrainingSnapshot(std::span<char>) override;
    bool ReadRng(RngState &state) const override;
    bool WriteRng(const RngState &state) override;
  private:
    bool trainingHold_ = false;
    uint32_t trainingFreezeBefore_ = 0, trainingFreezeActiveBefore_ = 0;
};

/// 実メモリ実装を設置する。DLL 初期化の最初期に1回だけ呼ぶ。
void InstallRealGameMemory();

} // namespace cccaster::game_interface
