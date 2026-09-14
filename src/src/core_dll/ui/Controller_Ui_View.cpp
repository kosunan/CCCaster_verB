#include "core_dll/ui/Controller_Ui_View.hpp"
#include "core_dll/ui/Controller_Ui_Logic.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/ui/OverlayRenderer.hpp"
#include <imgui.h>
#include <algorithm>

namespace cccaster::domain::ui {
namespace {
using L = ControllerUiLogic;
using namespace cccaster::input;
const ImVec4 accent(0.32f, 0.86f, 0.94f, 1), warning(1, 0.77f, 0.32f, 1), error(1, 0.4f, 0.35f, 1);
void BindingRow(int player, int index) {
    ImGui::PushID(index);
    const bool capturing = L::CapturePlayer() == player && L::CaptureBinding() == index;
    const auto &bind = L::Binds(player)[index];
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(BindingLabels[index]);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushStyleColor(ImGuiCol_Text, capturing ? warning : accent);
    if (ImGui::Button(capturing ? "PRESS INPUT..." : bind.empty() ? "Not assigned" : bind.c_str(), ImVec2(-1, 0)))
        L::StartBinding(player, index);
    ImGui::PopStyleColor();
    ImGui::TableSetColumnIndex(2);
    if (index >= 10 && !bind.empty()) {
        if (ImGui::SmallButton("X")) L::ClearBinding(player, index);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this optional binding");
    }
    ImGui::PopID();
}
void BindingTable(int player, const char *id, int from, int to) {
    if (!ImGui::BeginTable(id, 3, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, from == 10 ? 132 : 96);
    ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Clear", ImGuiTableColumnFlags_WidthFixed, 18);
    for (int i = from; i < to; ++i) BindingRow(player, i);
    ImGui::EndTable();
}
}

void ControllerUiView::Draw() {
    L::BeginUiSession();
    L::Update();
    static int player = 0;
    if (L::CapturePlayer() >= 0) player = L::CapturePlayer();
    const auto display = ImGui::GetIO().DisplaySize;
    const float width = std::min(620.0f, display.x - 12.0f);
    const float height = std::min(462.0f, display.y - 12.0f);
    ImGui::SetNextWindowPos(ImVec2((display.x - width) / 2, (display.y - height) / 2), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    cccaster::overlay::OverlayRenderer::PushModernStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(7, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3, 1));
    ImGui::SetNextWindowBgAlpha(0.97f);
    bool close = false;
    if (ImGui::Begin("Controller setup", nullptr, ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(accent, "CONTROLLER SETUP");
        ImGui::SameLine();
        ImGui::TextDisabled("/ %s", L::Dirty() ? "UNSAVED CHANGES" : "SAVED SETTINGS");
        ImGui::TextUnformatted("Choose a device. Click any binding to edit. F4 saves and closes.");
        ImGui::Separator();
        ImGui::BeginDisabled(L::CapturePlayer() >= 0);
        if (ImGui::RadioButton("PLAYER 1", player == 0)) player = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("PLAYER 2", player == 1)) player = 1;
        ImGui::SameLine();
        ImGui::TextDisabled("Last input: %s", L::LastInput(player).empty() ? "--" : L::LastInput(player).c_str());
        const int deviceId = L::DeviceId(player);
        const auto &device = L::Device(player);
        const auto label = device.Empty() ? "SELECT DEVICE" : device.name +
            (deviceId == -1 ? " (disconnected)" : device.guid.empty() ? "" : " [" + device.guid.substr(0, 8) + "]");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##device", label.c_str())) {
            if (ImGui::Selectable("NONE", device.Empty())) L::SelectDevice(player, -1);
            if (ImGui::Selectable("Keyboard", deviceId == -2)) L::SelectDevice(player, -2);
            for (const auto &dev : cccaster::game_interface::DirectInputHook::GetConnectedDevices()) {
                const auto text = std::string(dev.name) + " [" + std::string(dev.instanceGuid).substr(0, 8) + "]";
                if (ImGui::Selectable(text.c_str(), deviceId == dev.id)) L::SelectDevice(player, dev.id);
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();

        // 固定フッターを残す。640x480でも割当・警告・保存操作が重ならない。
        ImGui::BeginChild("bindings", ImVec2(0, -89), false);
        ImGui::BeginDisabled(deviceId == -1);
        if (ImGui::BeginTable("groups", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            ImGui::TextColored(accent, "BASIC / REQUIRED");
            BindingTable(player, "basic", 0, 10);
            ImGui::TableNextColumn();
            ImGui::TextColored(accent, "OPTIONAL / X TO CLEAR");
            BindingTable(player, "optional", 10, 13);
            ImGui::Spacing();
            ImGui::TextColored(warning, "TRAINING ONLY");
            ImGui::TextWrapped("FN1: Save now. Hold to freeze both players; release to resume.");
            ImGui::TextWrapped("FN2: Reset, then load the saved state. Without a save: normal reset.");
            ImGui::TextWrapped("Returning to character select clears the save.");
            ImGui::TextDisabled("One slot / memory only");
            if (deviceId >= 0) {
                bool analog = L::AnalogDirections(player);
                if (ImGui::Checkbox("Extra stick directions", &analog)) L::SetAnalogDirections(player, analog);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Also use the left stick for movement. Disable to prevent unwanted drift.\nThe four bindings on the left remain active.");
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::Separator();
        if (L::CapturePlayer() >= 0) {
            if (ImGui::Button("BACK")) L::PreviousBinding();
            ImGui::SameLine();
            if (ImGui::Button("KEEP / NEXT")) L::SkipCapture();
            ImGui::SameLine();
            if (ImGui::Button("STOP [ESC]")) L::CancelCapture();
            ImGui::SameLine();
            ImGui::TextColored(warning, "P%d: %s", L::CapturePlayer() + 1,
                L::CaptureBinding() >= 0 ? BindingLabels[L::CaptureBinding()] : "Done");
        } else {
            ImGui::BeginDisabled(deviceId == -1);
            if (ImGui::Button("MAP ALL BASIC")) L::StartMappingForPlayer(player);
            ImGui::SameLine();
            if (ImGui::Button("DEFAULTS")) ImGui::OpenPopup("Reset to defaults?");
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("REVERT")) L::Revert();
            ImGui::SameLine();
            if (ImGui::Button("SAVE & CLOSE [F4]")) close = true;
        }
        if (ImGui::BeginPopupModal("Reset to defaults?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("Replace this player's draft with defaults?");
            ImGui::TextUnformatted("Nothing is written until you save. REVERT undoes it.");
            if (ImGui::Button("USE DEFAULTS")) { L::ResetPlayerToDefaults(player); ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("CANCEL")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        const auto validation = L::Validation(player);
        ImGui::PushStyleColor(ImGuiCol_Text, L::StatusError() ? error : warning);
        ImGui::TextWrapped("%s", L::Status().c_str());
        ImGui::PopStyleColor();
        if (deviceId != -1 && !validation.valid)
            ImGui::TextColored(error, "%s", validation.message.c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    cccaster::overlay::OverlayRenderer::PopModernStyle();
    if (close && OnClose()) StateUiLogic::CloseMappingWindow();
}
bool ControllerUiView::OnClose() {
    if (!L::EndUiSession()) return false;
    StateUiLogic::NotifyControllerSettingsClosed();
    return true;
}
} // namespace cccaster::domain::ui
