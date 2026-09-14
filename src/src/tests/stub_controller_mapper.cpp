// ============================================================================
// stub_controller_mapper.cpp — ControllerMapper のスタブ実装
// ============================================================================
// テスト時に ControllerMapper の依存 (DirectInputHook, ConfigManager) を
// 排除するためのスタブ。リンクエラーを解消するために最小限の定義のみ提供。
// ============================================================================
#include "core_dll/ui/ControllerMapper.hpp"

namespace cccaster::domain::ui {

void ControllerMapper::SaveDeviceAllocations() {
    // スタブ: テスト時は何もしない
}

void ControllerMapper::Draw() {
    // スタブ: テスト時は何もしない
}

void ControllerMapper::ResetBindingState() {
    // スタブ: テスト時は何もしない
}

} // namespace cccaster::domain::ui
