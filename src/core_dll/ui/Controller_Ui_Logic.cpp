#include "core_dll/ui/Controller_Ui_Logic.hpp"
#include "core_dll/common/DataPaths.hpp"
#include "core_dll/common/InputDiagnostic.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include <imgui.h>
#include <fstream>

namespace cccaster::domain::ui {
namespace {
using namespace cccaster::input;
using Hook = cccaster::game_interface::DirectInputHook;
using cccaster::main_app::Config;
using cccaster::main_app::ConfigManager;
struct Draft {
    DeviceIdentity device;
    Bindings binds{};
    std::array<std::string, 4> alt{};
    bool dirty = false, allocationDirty = false;
    std::string lastInput;
};
std::array<Draft, 2> drafts;
bool active = false, statusError = false;
std::string status, releaseInput;
int capturePlayer = -1, captureBinding = -1;
bool sequential = false;
double captureAfter = 0;
constexpr const char *AltKeys[] = {"Up_Alt", "Down_Alt", "Left_Alt", "Right_Alt"};
constexpr const char *AltDefaults[] = {"A1+", "A1-", "A0-", "A0+"};
std::string Path(const DeviceIdentity &device, bool read) {
    const auto path = cccaster::core::paths::Resolve(ProfileFilename(device));
    if (read && !std::ifstream(path).good())
        return cccaster::core::paths::Resolve(ProfileFilename(device, true));
    return path;
}
void Message(std::string text, bool error = false) {
    status = std::move(text); statusError = error;
    if (cccaster::diagnostics::input::Enabled())
        cccaster::domain::session::DebugLog("[InputSetup] error=%u message=%s", unsigned(error), status.c_str());
}
void LoadDraft(Draft &draft) {
    Config config;
    if (!draft.device.Empty()) config.Load(Path(draft.device, true));
    const bool keyboard = draft.device.name == "Keyboard";
    const auto defaults = DefaultBindings(keyboard);
    for (int i = 0; i < BindingCount; ++i)
        draft.binds[i] = config.GetString("Mapping", BindingKeys[i], defaults[i]);
    for (int i = 0; i < 4; ++i)
        draft.alt[i] = config.GetString("Mapping", AltKeys[i], keyboard ? "" : AltDefaults[i]);
    draft.dirty = false;
    draft.lastInput.clear();
}
std::string KeyboardEdge() {
    for (int key = ImGuiKey_NamedKey_BEGIN; key <= ImGuiKey_AppForward; ++key) {
        if (key == ImGuiKey_F4 || key == ImGuiKey_Escape) continue;
        if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false))
            return ImGui::GetKeyName(static_cast<ImGuiKey>(key));
    }
    return {};
}
void Advance() {
    if (sequential && captureBinding < SaveStateBinding - 1) ++captureBinding;
    else {
        capturePlayer = captureBinding = -1;
        Message("Draft updated. Review the bindings, then SAVE & CLOSE / F4.");
    }
}
}

void ControllerUiLogic::BeginUiSession() {
    if (active) return;
    active = true;
    // 覗くだけではINIを書かない。旧名だけの設定は一意な個体に限り解決。
    const auto devices = Hook::GetConnectedDevices();
    for (int p = 0; p < 2; ++p) {
        auto &draft = drafts[p];
        draft = {};
        const std::string prefix = p == 0 ? "P1" : "P2";
        draft.device = {ConfigManager::GetString("Settings", prefix + "Device", ""),
                        ConfigManager::GetString("Settings", prefix + "DeviceGuid", "")};
        const auto id = ResolveDevice(draft.device, devices);
        if (id >= 0) draft.device = IdentifyDevice(id, devices);
        LoadDraft(draft);
    }
    Message("Choose your device, then click a binding to change only that input.");
}
int ControllerUiLogic::DeviceId(int p) { return ResolveDevice(drafts[p].device, Hook::GetConnectedDevices()); }
const DeviceIdentity &ControllerUiLogic::Device(int p) { return drafts[p].device; }
const Bindings &ControllerUiLogic::Binds(int p) { return drafts[p].binds; }
bool ControllerUiLogic::Dirty() {
    for (const auto &draft : drafts) if (draft.dirty || draft.allocationDirty) return true;
    return false;
}
int ControllerUiLogic::CapturePlayer() { return capturePlayer; }
int ControllerUiLogic::CaptureBinding() { return captureBinding; }
const std::string &ControllerUiLogic::LastInput(int p) { return drafts[p].lastInput; }
const std::string &ControllerUiLogic::Status() { return status; }
bool ControllerUiLogic::StatusError() { return statusError; }
ControllerMappingValidation ControllerUiLogic::Validation(int p) { return ValidateControllerMapping(drafts[p].binds); }
bool ControllerUiLogic::AnalogDirections(int p) {
    for (const auto &alt : drafts[p].alt) if (!alt.empty()) return true;
    return false;
}
void ControllerUiLogic::SetAnalogDirections(int p, bool enabled) {
    auto &draft = drafts[p];
    for (int i = 0; i < 4; ++i) draft.alt[i] = enabled ? AltDefaults[i] : "";
    draft.dirty = true;
}
bool ControllerUiLogic::SelectDevice(int p, int joyId) {
    const auto device = IdentifyDevice(joyId, Hook::GetConnectedDevices());
    if (device == drafts[p].device) return true;
    if (capturePlayer >= 0 || drafts[p].dirty) {
        Message("Save or REVERT changes before switching devices.", true); return false;
    }
    if (!device.Empty() && device == drafts[1 - p].device) {
        Message("This device belongs to the other player. Select NONE there first.", true); return false;
    }
    drafts[p].device = device;
    LoadDraft(drafts[p]);
    drafts[p].allocationDirty = true;
    Message("Device selected. Check its bindings before saving.");
    return true;
}
void ControllerUiLogic::StartBinding(int p, int binding, bool all) {
    if (DeviceId(p) == -1 || binding < 0 || binding >= BindingCount) {
        Message("Connect and select a device first.", true); return;
    }
    capturePlayer = p; captureBinding = binding; sequential = all;
    releaseInput.clear();
    captureAfter = ImGui::GetTime() + 0.2;
    Message("Press the requested input. ESC stops capture; changes stay in the draft.");
}
void ControllerUiLogic::StartMappingForPlayer(int p) { StartBinding(p, 0, true); }
void ControllerUiLogic::CancelCapture() {
    capturePlayer = captureBinding = -1; releaseInput.clear();
}
void ControllerUiLogic::SkipCapture() { if (capturePlayer >= 0) Advance(); }
void ControllerUiLogic::PreviousBinding() {
    if (capturePlayer >= 0 && captureBinding > 0) { --captureBinding; captureAfter = ImGui::GetTime() + 0.2; }
}
void ControllerUiLogic::ClearBinding(int p, int binding) {
    if (binding < 10 || binding >= BindingCount) return;
    drafts[p].binds[binding].clear(); drafts[p].dirty = true;
}
void ControllerUiLogic::ResetPlayerToDefaults(int p) {
    if (DeviceId(p) == -1) return;
    CancelCapture();
    drafts[p].binds = DefaultBindings(drafts[p].device.name == "Keyboard");
    SetAnalogDirections(p, drafts[p].device.name != "Keyboard");
    Message("Defaults are in the draft. REVERT can undo this until you save.");
}
void ControllerUiLogic::Revert() {
    CancelCapture(); active = false; BeginUiSession();
    Message("Unsaved changes reverted. Your saved settings are unchanged.");
}
void ControllerUiLogic::Suspend() {
    CancelCapture();
    if (Dirty()) Message("Scene changed. Your unsaved draft is kept here; review and save it.", true);
    else active = false;
}
void ControllerUiLogic::Update() {
    for (int p = 0; p < 2; ++p) {
        const int id = DeviceId(p);
        const auto edge = id == -2 ? KeyboardEdge() : Hook::GetAnyInputEdge(id);
        if (!edge.empty()) drafts[p].lastInput = edge;
        if (capturePlayer != p) continue;
        if (id == -1) {
            CancelCapture(); Message("Device disconnected. Draft kept; reconnect the same device.", true); continue;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { CancelCapture(); continue; }
        if (ImGui::GetTime() < captureAfter) continue;
        if (!releaseInput.empty()) {
            if (Hook::IsBindingPressed(id, releaseInput)) continue;
            releaseInput.clear();
        }
        if (edge.empty()) continue;
        drafts[p].binds[captureBinding] = edge; drafts[p].dirty = true;
        releaseInput = edge;
        Advance();
    }
}
bool ControllerUiLogic::EndUiSession() {
    if (!active) return true;
    // 何も変えていない古い設定は検証エラーで閉じ込めず、そのまま維持する。
    for (int p = 0; p < 2; ++p) {
        const auto &draft = drafts[p];
        if ((!draft.dirty && !draft.allocationDirty) || draft.device.Empty()) continue;
        if (DeviceId(p) == -1) {
            Message("NOT SAVED: edited device is disconnected. Reconnect it or REVERT.", true); return false;
        }
        const auto validation = Validation(p);
        if (!validation.valid) {
            Message(std::string("NOT SAVED / P") + char('1' + p) + ": " + validation.message, true); return false;
        }
    }
    if (Dirty()) {
        Config main;
        const auto mainPath = cccaster::core::paths::Resolve("cccaster_v10.ini");
        main.Load(mainPath);
        for (int p = 0; p < 2; ++p) {
            const auto &draft = drafts[p];
            if (!draft.dirty && !draft.allocationDirty) continue;
            if (!draft.device.Empty()) {
                Config config;
                config.Load(Path(draft.device, true)); // 他の設定項目は保全。
                for (int i = 0; i < BindingCount; ++i)
                    config.SetString("Mapping", BindingKeys[i], draft.binds[i]);
                for (int i = 0; i < 4; ++i) config.SetString("Mapping", AltKeys[i], draft.alt[i]);
                if (!config.SaveChecked(Path(draft.device, false))) {
                    Message("NOT SAVED: cannot write profile. Draft kept; check disk permissions.", true); return false;
                }
            }
            const std::string prefix = p == 0 ? "P1" : "P2";
            main.SetString("Settings", prefix + "Device", draft.device.name);
            main.SetString("Settings", prefix + "DeviceGuid", draft.device.guid);
        }
        if (!main.SaveChecked(mainPath)) {
            Message("NOT APPLIED: profiles written but player settings failed. Retry saving.", true); return false;
        }
        ConfigManager::Load(mainPath);
        Hook::ReloadConfigs();
    }
    CancelCapture(); active = false;
    if (cccaster::diagnostics::input::Enabled())
        cccaster::domain::session::DebugLog("[InputSetup] saved settings accepted; setup close permitted");
    return true;
}
} // namespace cccaster::domain::ui
