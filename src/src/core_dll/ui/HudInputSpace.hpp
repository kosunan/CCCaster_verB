#pragma once
#include <imgui_internal.h>

namespace cccaster::hud {
// Win32入力はクライアント座標、DX9描画はバックバッファ座標。
// NewFrameで持ち越されるイベントも含め、変換を一度だけ適用する。
class InputSpace {
    ImVec2 scale_{1, 1};
    ImVec2 offset_{0, 0};
    static void Transform(ImVec2& pos, float x, float y, float dx = 0, float dy = 0) {
        if (pos.x > -1e20f && pos.y > -1e20f) { pos.x = pos.x * x + dx; pos.y = pos.y * y + dy; }
    }
    static void Queue(float x, float y, float dx = 0, float dy = 0) {
        for (auto& event : ImGui::GetCurrentContext()->InputEventsQueue) {
            if (event.Type != ImGuiInputEventType_MousePos) continue;
            ImVec2 pos(event.MousePos.PosX, event.MousePos.PosY);
            Transform(pos, x, y, dx, dy);
            event.MousePos.PosX = pos.x; event.MousePos.PosY = pos.y;
        }
    }
public:
    void Begin(ImVec2 client, ImVec2 buffer, ImVec2 origin = {0, 0}) {
        const ImVec2 next(client.x > 0 ? buffer.x / client.x : 1,
                          client.y > 0 ? buffer.y / client.y : 1);
        auto& io = ImGui::GetIO();
        const ImVec2 offset(-origin.x * next.x, -origin.y * next.y);
        const float x = next.x / scale_.x, y = next.y / scale_.y;
        const float dx = offset.x - offset_.x * x, dy = offset.y - offset_.y * y;
        Transform(io.MousePos, x, y, dx, dy);
        Transform(io.MousePosPrev, x, y, dx, dy);
        for (auto& pos : io.MouseClickedPos) Transform(pos, x, y, dx, dy);
        scale_ = next;
        offset_ = offset;
        Queue(scale_.x, scale_.y, offset_.x, offset_.y);
    }
    void End() { Queue(1 / scale_.x, 1 / scale_.y, -offset_.x / scale_.x, -offset_.y / scale_.y); }
};
}
