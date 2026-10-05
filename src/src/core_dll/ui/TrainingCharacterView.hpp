#pragma once
struct IDirect3DDevice9;
namespace cccaster::domain::ui::training_character_view {
void Prepare(IDirect3DDevice9* device);
void Release();
bool Draw();
}
