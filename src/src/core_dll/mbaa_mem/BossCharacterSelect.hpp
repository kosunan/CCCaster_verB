#pragma once
namespace cccaster::boss::selection {
bool Configure(unsigned mode,bool enabled);
bool Enabled();
void DrawLabels(); // 元ゲームの描画キュー内だけで呼ぶ。HUDへ重ねない。
}
