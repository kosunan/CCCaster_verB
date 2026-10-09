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
    auto *font = cccaster::hud::Font(5, scale);
    const float top = layout.Y(112);
    constexpr ImU32 white = IM_COL32(232, 239, 247, 255);
    constexpr ImU32 colors[] = {
        IM_COL32(37, 49, 63, 104), IM_COL32(105, 220, 125, 164), // READY / BUSY
        IM_COL32(170, 180, 194, 184), IM_COL32(241, 224, 132, 164), // STUN / JUMP
        IM_COL32(145, 194, 255, 164), IM_COL32(225, 184, 0, 184), // SHIELD / CLASH
        IM_COL32(255, 255, 255, 220), IM_COL32(36, 125, 67, 200) // INVULN / START
    };
    constexpr ImU32 attackColor = IM_COL32(247, 102, 119, 184);
    constexpr ImU32 stopColor = IM_COL32(77, 94, 172, 210);
    constexpr ImU32 airColor = IM_COL32(112, 219, 235, 255);
    constexpr ImU32 timerColor = IM_COL32(112, 219, 235, 255);
    constexpr ImU32 strikeColor = IM_COL32(255, 255, 255, 220);
    constexpr ImU32 darkNumber = IM_COL32(19, 29, 40, 255);
    constexpr ImU32 throwColor = IM_COL32(255, 203, 126, 255);

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
        if (color != darkNumber)
            draw->AddText(font, fontSize, ImVec2(at.x + scale, at.y + scale), IM_COL32(0, 0, 0, 190), text);
        draw->AddText(font, fontSize, at, color, text);
    };
    draw->AddRectFilled(ImVec2(ox, top), ImVec2(layout.X(640), top + 45 * scale), IM_COL32(8, 13, 30, 165));
    draw->AddLine(ImVec2(ox, top), ImVec2(layout.X(640), top), cccaster::hud::Line, scale);
    draw->AddLine(ImVec2(ox, top + 45 * scale), ImVec2(layout.X(640), top + 45 * scale), cccaster::hud::Line, scale);
    cccaster::hud::Canvas c;
    // ADVは先に描かれるため、左の専用欄へ凡例の背景を重ねない。
    c.Fill({112,157,528,21},IM_COL32(8,13,30,185));
    c.Text(5,113,"F1",8,white);
    c.Text(22,113,"45F",7,cccaster::hud::Muted);
    const FrameBarState legend[]{FrameBarState::Busy, FrameBarState::Stun, FrameBarState::Jump,
        FrameBarState::Shield, FrameBarState::Clash, FrameBarState::Invulnerable, FrameBarState::StartWait};
    for (unsigned i=0;i<9;++i) {
        const float x=118+i*58.f;
        const ImU32 color = i < 7 ? colors[unsigned(legend[i])] : i == 7 ? attackColor : stopColor;
        c.Fill({x,160,4,4},color | IM_COL32_A_MASK);
        c.Text(x+7,157,i < 7 ? FrameBarStateName(legend[i]) : i == 7 ? "ATK" : "STOP",8,white);
    }
    c.Text(118,168,"RED TOP: HITBOX   CYAN BASE: AIR   BASE: STATE   HOVER: BOTH COUNTERS",8,cccaster::hud::Muted);
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
    int hoveredSide = -1;
    size_t hoveredFrame = 0;
    const auto mouse = ImGui::GetIO().MousePos;
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
            const auto stateColor = colors[static_cast<unsigned>(cell.state)];
            const auto signalColor = cell.signal == FrameBarSignal::Hitstop ? stopColor : attackColor;
            const bool whiteCell = cell.strikeInvulnerable || cell.state == FrameBarState::Invulnerable;
            const auto fill = recorded ? (whiteCell ? strikeColor :
                cell.signal != FrameBarSignal::None ? signalColor : stateColor) : IM_COL32(20, 30, 42, 72);
            draw->AddRectFilled(ImVec2(x, y), ImVec2(endX, y + 14 * scale), fill, scale);
            // 参照の2つの色を1セルへ重ねる。白い面でも攻撃/停止と主状態を残す。
            if (recorded && (whiteCell || cell.signal != FrameBarSignal::None))
                draw->AddRectFilled(ImVec2(x, y + 11 * scale), ImVec2(endX, y + 14 * scale),
                    stateColor | IM_COL32_A_MASK);
            if (recorded && whiteCell && cell.signal != FrameBarSignal::None)
                draw->AddRectFilled(ImVec2(x, y), ImVec2(endX, y + 2 * scale), signalColor | IM_COL32_A_MASK);
            // ATKは参照と同じ攻撃属性。実矩形があるFだけ鮮紅色の上線を付ける。
            if (recorded && cell.activeBoxes)
                draw->AddRectFilled(ImVec2(x, y + 2 * scale), ImVec2(endX, y + 3 * scale), IM_COL32(255, 38, 58, 255));
            if (recorded && cell.detail.airborne)
                draw->AddRectFilled(ImVec2(x, y + 13 * scale), ImVec2(endX, y + 14 * scale), airColor);
            if (recorded && cell.stopped)
                draw->AddRectFilled(ImVec2(x, y), ImVec2(endX, y + scale), white);
            if (recorded && cell.timerSuppressed)
                draw->AddRectFilled(ImVec2(x, y + scale), ImVec2(endX, y + 2 * scale), timerColor);
            if (recorded && cell.detail.throwProtected)
                draw->AddLine(ImVec2(endX - scale, y + 2 * scale), ImVec2(endX - scale, y + 11 * scale), throwColor, scale);
            if (recorded && cell.slow)
                draw->AddCircleFilled(ImVec2(x + cellWidth * 0.5f, y + 2 * scale), scale, white);
            if (recorded)
                number(x, y, cellWidth - scale, 12 * scale,
                       !whiteCell && cell.signal != FrameBarSignal::None ? cell.signalFrame : cell.runFrame,
                       whiteCell ? darkNumber :
                       cell.state == FrameBarState::Ready && cell.signal == FrameBarSignal::None
                           ? IM_COL32(168, 188, 207, 255) : white, 11);
            if (recorded && i && cell.boundary)
                draw->AddLine(ImVec2(x - scale, y), ImVec2(x - scale, y + 14 * scale), IM_COL32(6, 11, 18, 160), 2 * scale);
            if (recorded && mouse.x >= x && mouse.x < x + cellWidth && mouse.y >= y && mouse.y < y + 14 * scale) {
                hoveredSide = static_cast<int>(side);
                hoveredFrame = i;
                draw->AddRect(ImVec2(x, y), ImVec2(endX, y + 14 * scale), white, scale);
            }
        }
        if (history.Size()) {
            const float edge = startX + history.Size() * cellWidth - scale;
            draw->AddLine(ImVec2(edge, y), ImVec2(edge, y + 14 * scale), IM_COL32(223, 236, 247, 210), scale);
        }
    }
    if (hoveredSide >= 0) {
        const auto &column = history.At(hoveredFrame);
        const auto &cell = column.players[static_cast<unsigned>(hoveredSide)];
        const auto &detail = cell.detail;
        char lines[8][180]{};
        std::snprintf(lines[0], sizeof(lines[0]), "P%d  %s %uF  |  %s %uF  |  sample %u",
            hoveredSide + 1, FrameBarStateName(cell.state), cell.runFrame,
            FrameBarSignalName(cell.signal), cell.signalFrame, column.trueFrame);
        std::snprintf(lines[1], sizeof(lines[1]), "Action %u  |  move clock %u  |  busy %d  |  stun %d (raw)",
            cell.pattern, detail.patternFrame, cell.busyCounter, detail.stunRemaining);
        char boxes[12] = "UNKNOWN";
        if (detail.attackBoxesKnown) std::snprintf(boxes, sizeof(boxes), "%u", unsigned(detail.attackBoxCount));
        std::snprintf(lines[2], sizeof(lines[2]), "Attack boxes %s  |  contact budget %u  |  stop %u / received %u",
            boxes, unsigned(detail.remainingHits), unsigned(detail.hitstop), unsigned(detail.receivedHitstop));
        std::snprintf(lines[3], sizeof(lines[3]), "Air marker %s / stance %s  |  recovery %d / %d (raw)  |  captured %s",
            detail.airborne ? "ON" : "OFF", !detail.stanceKnown ? "?" : detail.stance == 1 ? "AIR" : "GROUND/OTHER",
            int(detail.untechElapsed), int(detail.untechTotal), detail.thrown ? "YES" : "NO");
        std::snprintf(lines[4], sizeof(lines[4]), "Protection flags: strike %s / throw %s  |  timer hold %s / slow tick %s",
            detail.strikeProtected ? "ON" : "OFF", detail.throwProtected ? "ON" : "OFF",
            cell.timerSuppressed ? "ON" : "OFF", cell.slow ? "YES" : "NO");
        char hurt[12] = "UNKNOWN";
        if (detail.hurtBoxesKnown) std::snprintf(hurt, sizeof(hurt), "%u", unsigned(detail.hurtBoxCount));
        std::snprintf(lines[5], sizeof(lines[5]), "HURT boxes %s  |  white: %s  |  guard eligible %s  |  queued action %d",
            hurt, cell.strikeInvulnerable ? "YES" : "NO", detail.guardEligible ? "YES" : "NO", int(detail.reservedPattern));
        std::snprintf(lines[6], sizeof(lines[6]), "%s", cell.state == FrameBarState::StartWait
            ? (detail.reservedPattern >= 0
                ? "START: recovery edge; next motion queued. Guard eligibility is separate."
                : "START: recovery edge; input ready, no motion queued yet. Not an extra input lock.")
            : "ATK = attack data; bright red top = box present. STOP wins over ATK, state remains at base.");
        std::snprintf(lines[7], sizeof(lines[7]), "Defense slots %s%u  |  guard stun %s  |  one cell includes stop; counts are not frames remaining",
            detail.animationKnown ? "" : "UNKNOWN / ", unsigned(detail.defenseSlotCount),
            cell.pattern >= 17 && cell.pattern <= 19 ? "YES (pattern)" : "NO (pattern)");
        const float textSize = 10 * scale;
        float width = 0;
        for (const auto &line : lines)
            width = std::max(width, font->CalcTextSizeA(textSize, 100000.0f, 0, line).x);
        width = std::min(width + 16 * scale, 640 * scale);
        const float x = std::clamp(mouse.x - width * 0.5f, ox, layout.X(640) - width);
        const float y = layout.Y(181);
        draw->AddRectFilled(ImVec2(x, y), ImVec2(x + width, y + 104 * scale), IM_COL32(8, 13, 30, 242), 3 * scale);
        draw->PushClipRect(ImVec2(x, y), ImVec2(x + width, y + 104 * scale), true);
        for (unsigned row = 0; row < 8; ++row)
            draw->AddText(font, textSize, ImVec2(x + 8 * scale, y + (5 + 12 * row) * scale),
                row == 0 ? white : IM_COL32(184, 203, 218, 255), lines[row]);
        draw->PopClipRect();
    }
}

void LearningOverlay::Draw(int appMode, const cccaster::FrameAdvantageResult &result) {
    if ((appMode != 1 && appMode != 2) || StateUiLogic::IsMappingWindowOpen())
        return;
    if (!FrameBarDisplay::Visible(appMode))
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
