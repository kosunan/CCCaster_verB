#pragma once
// ============================================================================
// fake_game_memory.hpp — テスト用の IGameMemory 実装
//
// 読み取り値を直接指定でき、書き込まれた入力を全フレーム記録する。
// 記録した入力列を2セッション間で突き合わせるのが決定性テスト(B-4)の土台になる。
// ============================================================================

#include "core_dll/mbaa_mem/IGameMemory.hpp"

#include <vector>
#include <cstdint>

namespace cccaster::test {

class FakeGameMemory final : public cccaster::game_interface::IGameMemory {
  public:
    using GameInput = cccaster::game_interface::GameInput;

    struct WrittenFrame {
        GameInput p1;
        GameInput p2;
    };

    // ── 読み取り値（テストから直接書き換える）──
    bool available = true;
    uint32_t gameMode = 0;
    uint8_t introState = 0;
    uint32_t worldTimer = 0;
    uint32_t realTimer = 0;
    uint32_t menuStateCounter = 0;

    // ── 書き込み記録 ──
    std::vector<WrittenFrame> written;

    bool IsAvailable() const override {
        return available;
    }
    uint32_t GameMode() const override {
        return gameMode;
    }
    uint8_t IntroState() const override {
        return introState;
    }
    uint32_t WorldTimer() const override {
        return worldTimer;
    }
    uint32_t RealTimer() const override {
        return realTimer;
    }
    uint32_t MenuStateCounter() const override {
        return menuStateCounter;
    }

    void WriteInput(GameInput p1, GameInput p2) override {
        written.push_back(WrittenFrame{p1, p2});
    }

    /// 1フレーム進める（WorldTimer は常時カウントアップする性質を模す）
    void Tick() {
        ++worldTimer;
    }

    void ClearWritten() {
        written.clear();
    }
};

/// スコープを抜けたら設置を元に戻す。テストケース間で漏れないようにする。
class ScopedGameMemory {
  public:
    explicit ScopedGameMemory(cccaster::game_interface::IGameMemory *impl) {
        cccaster::game_interface::InstallGameMemory(impl);
    }
    ~ScopedGameMemory() {
        cccaster::game_interface::InstallGameMemory(nullptr);
    }
    ScopedGameMemory(const ScopedGameMemory &) = delete;
    ScopedGameMemory &operator=(const ScopedGameMemory &) = delete;
};

} // namespace cccaster::test
