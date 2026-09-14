#include "core_dll/ui/Rematch_Ui_View.hpp"
#include "core_dll/engine/RematchChoice.hpp"
#include <imgui.h>
namespace cccaster::domain::ui {
void RematchUiView::Draw() {
    auto &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * .5f, io.DisplaySize.y * .5f), ImGuiCond_Always,
                            ImVec2(.5f, .5f));
    ImGui::SetNextWindowBgAlpha(1.0f);
    if (ImGui::Begin("REMATCH", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoInputs)) {
        ImGui::TextUnformatted("ONCE AGAIN / CHARACTER SELECT");
        ImGui::Separator();
        for (int i = 0; i < 2; ++i) {
            const auto &p = scene::rematchChoice.players[i];
            ImGui::Text("P%d   %s   %s", i + 1, p.cursor == 0 ? "ONCE AGAIN" : "CHARACTER SELECT",
                        p.choice >= 0 ? "READY" : "SELECTING");
        }
        ImGui::Separator();
        ImGui::TextUnformatted("UP/DOWN: Select   A / Confirm: Decide");
        ImGui::TextUnformatted("Once Again requires both players.");
        ImGui::TextUnformatted("Either player can choose Character Select.");
        ImGui::TextDisabled("Replay saving disabled");
    }
    ImGui::End();
}
} // namespace cccaster::domain::ui
