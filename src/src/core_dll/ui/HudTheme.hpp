#pragma once
#include "core_dll/ui/HudResources.hpp"
#include <string>
#include <algorithm>

namespace cccaster::hud {
// MBAACCの白い縁・濃紺の面・プレイヤー色を、表示と設定の両方で使う。
inline constexpr ImU32 Ink = IM_COL32(8, 13, 30, 255);
inline constexpr ImU32 White = IM_COL32(237, 244, 255, 255);
inline constexpr ImU32 Muted = IM_COL32(149, 169, 198, 255);
inline constexpr ImU32 Blue = IM_COL32(79, 197, 248, 255);
inline constexpr ImU32 Red = IM_COL32(244, 78, 107, 255);
inline constexpr ImU32 Gold = IM_COL32(255, 207, 109, 255);
inline constexpr ImU32 Green = IM_COL32(125, 225, 186, 255);
inline constexpr ImU32 Line = IM_COL32(101, 130, 171, 170);

struct Canvas {
    Layout layout = CurrentLayout();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    ImVec2 At(float x, float y) const { return {layout.X(x), layout.Y(y)}; }
    float S(float value) const { return value * layout.scale; }
    void Fill(Rect r, ImU32 color) const {
        draw->AddRectFilled(At(r.x, r.y), At(r.x+r.width, r.y+r.height), color);
    }
    void Rule(float x, float y, float width, ImU32 color = Line) const {
        draw->AddLine(At(x,y), At(x+width,y), color, (std::max)(1.f, S(1)));
    }
    void Plate(Rect r, ImU32 edge = Line, bool strong = false) const {
        draw->AddRectFilledMultiColor(At(r.x,r.y), At(r.x+r.width,r.y+r.height),
            IM_COL32(28,41,70,255), IM_COL32(28,41,70,255), Ink, Ink);
        draw->AddRect(At(r.x,r.y), At(r.x+r.width,r.y+r.height), strong ? White : edge,
            0, 0, (std::max)(1.f, S(1)));
        if (strong) Rule(r.x+2, r.y+r.height-2, r.width-4, edge);
    }
    std::string Fit(const char* input, float width, float size, unsigned role) const {
        std::string value = input ? input : "";
        auto* font = Font(role, layout.scale);
        const auto measure = [&](const std::string& text) { return font->CalcTextSizeA(S(size), 100000, 0, text.c_str()).x; };
        if (measure(value) <= S(width)) return value;
        while (!value.empty() && measure(value + "...") > S(width)) {
            // UTF-8の途中で切って未定義の文字を描かない。
            auto pos = value.size()-1;
            while (pos && (static_cast<unsigned char>(value[pos]) & 0xc0) == 0x80) --pos;
            value.resize(pos);
        }
        return value + "...";
    }
    void Text(float x, float y, const char* value, float size = 10, ImU32 color = White,
              unsigned role = 3, float width = 10000, bool right = false) const {
        auto* font = Font(role, layout.scale);
        const auto fitted = Fit(value, width, size, role);
        const float length = font->CalcTextSizeA(S(size), 100000, 0, fitted.c_str()).x;
        const auto pos = At(x,y);
        draw->AddText(font, S(size), {right ? pos.x-length : pos.x, pos.y}, color, fitted.c_str());
    }
    void Key(float x, float y, float width, const char* label) const {
        Plate({x,y,width,14});
        auto* font = Font(3,layout.scale);
        const float extent = font->CalcTextSizeA(S(9),10000,0,label).x;
        draw->AddText(font,S(9),{At(x+width/2,y+2).x-extent/2,At(x,y+2).y},White,label);
    }
};
}
