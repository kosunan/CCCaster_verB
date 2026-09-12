#include "core_dll/ui/LearningOverlay.hpp"
#include "core_dll/ui/HudDisplay.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/mbaa_mem/TrainingMetrics.hpp"
#include <imgui.h>
#include <algorithm>
#include <cstdio>

namespace cccaster::domain::ui {

void LearningOverlay::Draw(int appMode, const cccaster::FrameAdvantageResult &result) {
    if ((appMode != 1 && appMode != 2) || StateUiLogic::IsMappingWindowOpen())
        return;
    const auto mode = HudDisplay::Get();
    if (mode == HudDisplayMode::Hidden)
        return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f)
        return;
    const bool detailed = mode == HudDisplayMode::Detailed;
    const float scale = std::min(display.x / 640.0f, display.y / 480.0f);
    const float width = 320.0f * scale;
    const float left = (display.x - width) * 0.5f;
    // 中央の細い領域を使い、左右の標準入力履歴、体力、下端ゲージを避ける。
    // 縦長画面ではゲームの中央480ラインへ追従する。
    const float top = (display.y - 480.0f * scale) * 0.5f + 82.0f * scale;
    const float height = (detailed ? 72.0f : 24.0f) * scale;
    auto *draw = ImGui::GetForegroundDrawList();
    auto *font = ImGui::GetFont();
    draw->AddRectFilled(ImVec2(left, top), ImVec2(left + width, top + height),
                        IM_COL32(10, 18, 29, 206), 4.0f * scale);
    draw->PushClipRect(ImVec2(left, top), ImVec2(left + width, top + height), true);
    int row = 0;
    const auto text = [&](const char *value, ImU32 color) {
        float size = 12.0f * scale;
        const float measured = font->CalcTextSizeA(size, 100000.0f, 0.0f, value).x;
        const float available = width - 16.0f * scale;
        if (measured > available && measured > 0.0f)
            size *= available / measured;
        draw->AddText(font, size, ImVec2(left + 8.0f * scale,
                                       top + (6.0f + row * 16.0f) * scale), color, value);
        ++row;
    };

    constexpr ImU32 normal = IM_COL32(222, 233, 243, 255);
    constexpr ImU32 muted = IM_COL32(174, 193, 209, 255);
    constexpr ImU32 accent = IM_COL32(112, 231, 243, 255);
    constexpr ImU32 waiting = IM_COL32(255, 200, 116, 255);
    switch (result.state) {
    case cccaster::FrameAdvantageState::Confirmed: {
        char line[128];
        // 0F は同時終了として有効。未知や計測途中の値をここへ通さない。
        // int最小値でも符号反転がオーバーフローしないよう拡張する。
        const auto p1 = static_cast<long long>(result.p1Frames);
        std::snprintf(line, sizeof(line), "ADV   P1 %+lldF   P2 %+lldF   [FINAL]", p1, -p1);
        text(line, accent);
        break;
    }
    case cccaster::FrameAdvantageState::Measuring:
        text("ADV   MEASURING...", waiting);
        break;
    case cccaster::FrameAdvantageState::Unmeasured:
    default:
        text("ADV   --   [NOT MEASURED]", muted);
        break;
    }
    if (detailed) {
        text("Final only after both players recover.", normal);
        text("+F: earlier recovery   0F: simultaneous", muted);
        text("Use game input history.   Ctrl+F3: hide", muted);
    }
    draw->PopClipRect();
}

} // namespace cccaster::domain::ui
