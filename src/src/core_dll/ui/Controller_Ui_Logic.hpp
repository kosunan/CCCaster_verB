#pragma once
#include "core_dll/hook/ControllerProfile.hpp"

namespace cccaster::domain::ui {
// 旧版と同じく登録・削除の都度保存する。左右の選択位置と入力待ちは独立。
class ControllerUiLogic {
  public:
    static constexpr int OverviewRow=-1, DoneRow=13;
    static void BeginUiSession();
    static void Update();
    static bool EndUiSession();
    static void Suspend();
    static bool SelectDevice(int player,int joyId);
    static void Navigate(int player,int delta);
    static void StartBinding(int player,int binding);
    static void CancelCapture(int player=-1);
    static bool ClearBinding(int player,int binding);
    static void Done(int player);
    static bool TakeCloseRequest();
    static int DeviceId(int player);
    static const cccaster::input::DeviceIdentity& Device(int player);
    static const cccaster::input::Bindings& Binds(int player);
    static int SelectedRow(int player);
    static int CaptureBinding(int player);
    static const std::string& Status();
    static bool StatusError();
};
} // namespace cccaster::domain::ui
