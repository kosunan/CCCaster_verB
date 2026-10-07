#pragma once
#include <d3d9.h>
#include <imgui.h>
#include "core_dll/ui/HudLayout.hpp"
namespace cccaster::hud {
void AddFonts();
void Prepare(IDirect3DDevice9* device);
void FinishInput();
void RenderDrawData();
void Release();
Layout CurrentLayout();
ImFont* Font(unsigned role, float scale);
ImTextureID Emblem(unsigned player);
}
