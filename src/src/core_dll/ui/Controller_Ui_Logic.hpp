#pragma once
#include "core_dll/hook/ControllerProfile.hpp"
#include "core_dll/ui/ControllerMappingValidation.hpp"

namespace cccaster::domain::ui {
// F4内の変更はドラフト。検証・保存成功までは実入力の設定を変えない。
class ControllerUiLogic {
  public:
    static void BeginUiSession();
    static void Update();
    static bool EndUiSession();
    static void Suspend(); // 画面遷移で閉じても未保存内容を保持する。
    static void Revert();
    static bool SelectDevice(int player, int joyId);
    static void StartMappingForPlayer(int player);
    static void StartBinding(int player, int binding, bool sequential = false);
    static void CancelCapture();
    static void SkipCapture();
    static void PreviousBinding();
    static void ClearBinding(int player, int binding);
    static void ResetPlayerToDefaults(int player);
    static void SetAnalogDirections(int player, bool enabled);
    static int DeviceId(int player);
    static const cccaster::input::DeviceIdentity &Device(int player);
    static const cccaster::input::Bindings &Binds(int player);
    static bool AnalogDirections(int player);
    static bool Dirty();
    static int CapturePlayer();
    static int CaptureBinding();
    static const std::string &LastInput(int player);
    static const std::string &Status();
    static bool StatusError();
    static ControllerMappingValidation Validation(int player);
};
} // namespace cccaster::domain::ui
