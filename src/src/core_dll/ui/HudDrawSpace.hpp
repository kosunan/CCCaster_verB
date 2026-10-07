#pragma once
#include <imgui.h>
#include "core_dll/ui/HudLayout.hpp"

namespace cccaster::hud {
// HUDの配置・文字・入力は表示先ピクセルで扱う。旧サイズのバックバッファへ
// 渡す直前に逆変換し、Presentによる縦横別の拡縮を打ち消す。
inline Rect DisplayViewport(Rect viewport, ImVec2 buffer, ImVec2 display) {
    const float x = display.x / buffer.x, y = display.y / buffer.y;
    return {viewport.x*x, viewport.y*y, viewport.width*x, viewport.height*y};
}
inline void MapDrawDataToBuffer(ImDrawData& data, ImVec2 buffer) {
    if (data.DisplaySize.x <= 0 || data.DisplaySize.y <= 0 || buffer.x <= 0 || buffer.y <= 0) return;
    const float x = buffer.x / data.DisplaySize.x, y = buffer.y / data.DisplaySize.y;
    if (x == 1 && y == 1) return;
    for (auto* list : data.CmdLists) {
        for (auto& vertex : list->VtxBuffer) {
            vertex.pos.x *= x;
            vertex.pos.y *= y;
        }
        for (auto& command : list->CmdBuffer) {
            command.ClipRect.x *= x; command.ClipRect.z *= x;
            command.ClipRect.y *= y; command.ClipRect.w *= y;
        }
    }
    data.DisplayPos.x *= x; data.DisplayPos.y *= y;
    data.DisplaySize = buffer;
}
}
