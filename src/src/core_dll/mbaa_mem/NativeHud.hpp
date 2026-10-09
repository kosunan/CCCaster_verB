#pragma once
#include <d3d9.h>
namespace cccaster::game_interface::native_hud {
// ROUNDと同じ画像キューでDELAYを描き、名前と重なる標準ラベルを抑止する。
void Install();
void Prepare(IDirect3DDevice9* device);
void Release();
// 標準HUDの登場が完了し、表示設定でも隠されていない。
bool BattleHudReady();
}
