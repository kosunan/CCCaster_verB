#pragma once
struct IDirect3DDevice9;
namespace cccaster::testing::state_sweep {
// 専用のTrainingプロセスだけで有効。trueなら通常の60Hz進行をこの更新で代行する。
bool Step(int appMode, IDirect3DDevice9* device);
}
