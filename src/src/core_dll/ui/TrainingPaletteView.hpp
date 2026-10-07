#pragma once
#include <d3d9.h>
namespace cccaster::domain::ui::training_palette_view {
void Prepare(IDirect3DDevice9* device);
void Release();
bool Draw();
}
