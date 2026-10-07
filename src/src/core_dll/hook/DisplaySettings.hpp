#pragma once
#include <array>

namespace cccaster::game_interface::borderless {
struct Resolution { int width = 0, height = 0; };
struct DisplaySettings {
    bool available = false, fullscreen = false;
    Resolution windowSize;
};
// 面積順。モニターの作業領域に収まる候補だけを使用する。
inline constexpr std::array<Resolution, 10> Resolutions{{
    {640,480}, {800,600}, {960,720}, {1024,768}, {1280,720},
    {1280,960}, {1600,900}, {1920,1080}, {2560,1440}, {3840,2160}
}};
inline Resolution NextResolution(Resolution current, Resolution limit, int direction) {
    Resolution first{}, last{}, next{};
    const int area = current.width * current.height;
    for (const auto size : Resolutions) {
        if (size.width > limit.width || size.height > limit.height) continue;
        if (!first.width) first = size;
        last = size;
        const int candidateArea = size.width * size.height;
        if (direction > 0 && candidateArea > area && !next.width) next = size;
        if (direction < 0 && candidateArea < area) next = size;
    }
    return next.width ? next : direction > 0 ? first : last;
}
// ゲームスレッド専用。描画解像度の再設定成功後に窓と全画面の比率を合わせる。
DisplaySettings GetDisplaySettings();
Resolution ResolutionLimit();
bool ApplyRenderResolution(Resolution size);
bool SetFullscreen(bool enabled);
void SetScaleFilter(bool enabled);
}
