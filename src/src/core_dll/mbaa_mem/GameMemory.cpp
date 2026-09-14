// ============================================================================
// GameMemory.cpp — 設置口の実装
//
// 未設置時は NullGameMemory を返す。テストバイナリが RealGameMemory を
// リンクせずに済むようにするためで、誤って実アドレスを触ってクラッシュする
// 事故も同時に防ぐ。
// ============================================================================

#include "core_dll/mbaa_mem/IGameMemory.hpp"

namespace cccaster::game_interface {

namespace {

class NullGameMemory final : public IGameMemory {
  public:
    bool IsAvailable() const override {
        return false;
    }
    uint32_t GameMode() const override {
        return 0;
    }
    uint8_t IntroState() const override {
        return 0;
    }
    uint32_t WorldTimer() const override {
        return 0;
    }
    uint32_t RealTimer() const override {
        return 0;
    }
    uint32_t MenuStateCounter() const override {
        return 0;
    }
    void WriteInput(GameInput, GameInput) override {}
};

NullGameMemory g_null;
IGameMemory *g_impl = &g_null;

} // namespace

void InstallGameMemory(IGameMemory *impl) {
    g_impl = impl ? impl : &g_null;
}

IGameMemory &GameMem() {
    return *g_impl;
}

} // namespace cccaster::game_interface
