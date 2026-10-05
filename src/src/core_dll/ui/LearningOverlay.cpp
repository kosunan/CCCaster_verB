#include "core_dll/ui/LearningOverlay.hpp"
#include "core_dll/ui/HudDisplay.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/ui/HudResources.hpp"
#include "core_dll/ui/HudTheme.hpp"
#include "core_dll/mbaa_mem/TrainingMetrics.hpp"
#include "core_dll/mbaa_mem/FrameBar.hpp"
#include <imgui.h>
#include <algorithm>
#include <cstdio>

namespace cccaster::domain::ui {

void LearningOverlay::DrawFrameBar(int appMode, const cccaster::FrameBarHistory &history) {
    if (!FrameBarDisplay::Visible(appMode) || StateUiLogic::IsMappingWindowOpen()) return;
    const auto display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0 || display.y <= 0) return;
    const auto layout = cccaster::hud::CurrentLayout();
    const float scale = layout.scale;
    const float ox = layout.X(0);
    auto *draw = ImGui::GetForegroundDrawList();
    auto *font = ImGui::GetFont();
    const float top = layout.Y(112);
    constexpr ImU32 white = IM_COL32(232, 239, 247, 255);
    constexpr ImU32 colors[] = {
        IM_COL32(37, 49, 63, 104), IM_COL32(105, 220, 125, 164),
        IM_COL32(247, 102, 119, 164), IM_COL32(94, 184, 245, 164), IM_COL32(192, 144, 242, 164)
    };

    // 標準の体力・ガード・キャラ円を避け、その下へ45Fをまとめる。
    // 毎Fの目盛りは残し、定規の数値は1と5F刻みに絞る。
    // 小型ドット字形を廃止し、セル内は中央揃えの通常フォントで読む。
    const float left = layout.X(3), right = layout.X(637);
    const float startX = layout.X(43);
    const float cellWidth = (right - startX) / FrameBarHistory::Capacity;
    if (cellWidth <= 0) return;
    const auto number = [&](float x, float y, float width, float height, uint32_t value, ImU32 color, float size) {
        char text[16];
        std::snprintf(text, sizeof(text), "%u", value);
        float fontSize = size * scale;
        auto extent = font->CalcTextSizeA(fontSize, 100000.0f, 0.0f, text);
        if (extent.x > width - 2 * scale) {
            fontSize *= (width - 2 * scale) / extent.x;
            extent = font->CalcTextSizeA(fontSize, 100000.0f, 0.0f, text);
        }
        const ImVec2 at(x + (width - extent.x) * 0.5f, y + (height - extent.y) * 0.5f);
        draw->AddText(font, fontSize, ImVec2(at.x + scale, at.y + scale), IM_COL32(0, 0, 0, 190), text);
        draw->AddText(font, fontSize, at, color, text);
    };
    draw->AddRectFilled(ImVec2(ox, top), ImVec2(layout.X(640), top + 45 * scale), IM_COL32(8, 13, 30, 165));
    draw->AddLine(ImVec2(ox, top), ImVec2(layout.X(640), top), cccaster::hud::Line, scale);
    draw->AddLine(ImVec2(ox, top + 45 * scale), ImVec2(layout.X(640), top + 45 * scale), cccaster::hud::Line, scale);
    cccaster::hud::Canvas c;
    // ADVは先に描かれるため、左の専用欄へ凡例の背景を重ねない。
    c.Fill({112,157,528,21},IM_COL32(8,13,30,185));
    c.Text(8,113,"45F",8,cccaster::hud::Muted);
    const char* legend[]{"READY", "BUSY", "ACTIVE", "GUARD", "HIT"};
    for (unsigned i=0;i<5;++i) {
        const float x=140+i*93.f;
        c.Fill({x,164,5,5},colors[i] | IM_COL32(0,0,0,255));
        c.Text(x+10,161,legend[i],9,cccaster::hud::Muted);
    }
    for (size_t i = 0; i < FrameBarHistory::Capacity; ++i) {
        const float x = startX + i * cellWidth;
        const bool major = (i + 1) % 5 == 0;
        if (i == 0 || major)
            number(x, top, cellWidth, 10 * scale, static_cast<uint32_t>(i + 1),
                   IM_COL32(195, 215, 232, 255), 9);
        draw->AddLine(ImVec2(x + cellWidth - scale, top + (major ? 8 : 10) * scale),
                      ImVec2(x + cellWidth - scale, top + 11 * scale),
                      major ? IM_COL32(121, 149, 179, 255) : IM_COL32(55, 72, 91, 255), scale);
    }
    for (unsigned side = 0; side < 2; ++side) {
        const float y = top + (12 + side * 17.0f) * scale;
        const ImU32 sideColor = side ? cccaster::hud::Blue : cccaster::hud::Red;
        draw->AddRectFilled(ImVec2(left, y), ImVec2(layout.X(39), y + 14 * scale), IM_COL32(25, 38, 53, 210));
        draw->AddRectFilled(ImVec2(left, y + 2 * scale), ImVec2(left + 2 * scale, y + 12 * scale), sideColor);
        draw->AddText(font, 10 * scale, ImVec2(left + 5 * scale, y + 2 * scale), sideColor, side ? "P2" : "P1");
        for (size_t i = 0; i < FrameBarHistory::Capacity; ++i) {
            const float x = startX + i * cellWidth;
            const bool recorded = i < history.Size();
            const auto cell = recorded ? history.At(i).players[side] : FrameBarCell{};
            const float endX = x + cellWidth - scale;
            const auto fill = recorded ? colors[static_cast<unsigned>(cell.state)] : IM_COL32(20, 30, 42, 72);
            draw->AddRectFilled(ImVec2(x, y), ImVec2(endX, y + 14 * scale), fill, scale);
            // 上面の控えめな光沢。データ色の面積と数字のコントラストを優先する。
            if (recorded && cell.state != FrameBarState::Ready)
                draw->AddLine(ImVec2(x + scale, y + scale), ImVec2(endX - scale, y + scale), IM_COL32(255, 255, 255, 48), scale);
            if (recorded && cell.busy)
                draw->AddRectFilled(ImVec2(x, y + 12 * scale), ImVec2(endX, y + 14 * scale), IM_COL32(105, 220, 125, 230));
            if (recorded && cell.stopped)
                draw->AddRectFilled(ImVec2(x, y), ImVec2(endX, y + scale), white);
            if (recorded)
                number(x, y, cellWidth - scale, 12 * scale, cell.runFrame,
                       cell.state == FrameBarState::Ready ? IM_COL32(168, 188, 207, 255) : white, 11);
            if (recorded && i && cell.state != history.At(i - 1).players[side].state)
                draw->AddLine(ImVec2(x - scale, y), ImVec2(x - scale, y + 14 * scale), IM_COL32(6, 11, 18, 160), 2 * scale);
        }
        if (history.Size()) {
            const float edge = startX + history.Size() * cellWidth - scale;
            draw->AddLine(ImVec2(edge, y), ImVec2(edge, y + 14 * scale), IM_COL32(223, 236, 247, 210), scale);
        }
    }
}

void LearningOverlay::Draw(int appMode, const cccaster::FrameAdvantageResult &result) {
    if ((appMode != 1 && appMode != 2) || StateUiLogic::IsMappingWindowOpen())
        return;
    if (!HudDisplay::Detailed())
        return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f)
        return;
    const auto layout = cccaster::hud::CurrentLayout();
    const float scale = layout.scale;
    const float width = 100.0f * scale;
    const float left = layout.X(6);
    // フレームバーのP1行から読める位置へまとめ、中央の戦闘領域を空ける。
    const float top = layout.Y(160);
    const float height = 18.0f * scale;
    auto *draw = ImGui::GetForegroundDrawList();
    auto *font = cccaster::hud::Font(3, scale);
    cccaster::hud::Canvas{}.Plate({6,160,100,18},cccaster::hud::Line);
    draw->PushClipRect(ImVec2(left, top), ImVec2(left + width, top + height), true);
    const auto text = [&](const char *value, ImU32 color) {
        float size = 10.0f * scale;
        const float measured = font->CalcTextSizeA(size, 100000.0f, 0.0f, value).x;
        const float available = width - 16.0f * scale;
        if (measured > available && measured > 0.0f)
            size *= available / measured;
        const auto extent = font->CalcTextSizeA(size, 100000.0f, 0.0f, value);
        draw->AddText(font, size, ImVec2(std::round(left + (width - extent.x) / 2), top + 3 * scale), color, value);
    };

    constexpr ImU32 muted = IM_COL32(174, 193, 209, 255);
    constexpr ImU32 accent = IM_COL32(112, 231, 243, 255);
    constexpr ImU32 waiting = IM_COL32(255, 200, 116, 255);
    switch (result.state) {
    case cccaster::FrameAdvantageState::Confirmed: {
        char line[128];
        // 0F は同時終了として有効。未知や計測途中の値をここへ通さない。
        // int最小値でも符号反転がオーバーフローしないよう拡張する。
        const auto p1 = static_cast<long long>(result.p1Frames);
        std::snprintf(line, sizeof(line), "ADV P1 %+lldF", p1);
        text(line, accent);
        break;
    }
    case cccaster::FrameAdvantageState::Measuring:
        text("ADV P1 ...", waiting);
        break;
    case cccaster::FrameAdvantageState::Unmeasured:
    default:
        text("ADV P1 --", muted);
        break;
    }
    draw->PopClipRect();
}

} // namespace cccaster::domain::ui
