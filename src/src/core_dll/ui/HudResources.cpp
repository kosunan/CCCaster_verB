#include "core_dll/ui/HudResources.hpp"
#include "core_dll/ui/StartupFontCache.hpp"
#include "core_dll/ui/TrainingCharacterView.hpp"
#include "core_dll/ui/HudInputSpace.hpp"
#include "core_dll/hook/BorderlessDisplay.hpp"
#include "shared_contracts/EmblemTexture.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include <imgui_impl_dx9.h>
#include <cstdlib>
#include <string>

namespace cccaster::hud {
namespace {
constexpr float Scales[]{1, 1.5f, 2, 3, 4, 6};
constexpr float Sizes[]{15, 13, 24, 10, 22, 12};
ImFont* fonts[6][6]{};
emblem::Texture textures[2];
Layout layout;
InputSpace inputSpace;
unsigned FontScale(float scale) {
    unsigned s = 0;
    while (s < 5 && Scales[s] < scale) ++s;
    return s;
}
bool AddFontScale(unsigned s) {
    if (fonts[s][0]) return false;
    char directory[MAX_PATH]{};
    GetWindowsDirectoryA(directory, MAX_PATH);
    auto* atlas = ImGui::GetIO().Fonts;
    for (unsigned role = 0; role < 6; ++role) {
        const std::string path = std::string(directory) + "\\Fonts\\" +
            (role == 4 ? "arialbi.ttf" : role == 5 ? "tahoma.ttf" : "tahomabd.ttf");
        ImFontConfig config; config.OversampleH = config.OversampleV = 2;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            fonts[s][role] = atlas->AddFontFromFileTTF(path.c_str(), Sizes[role] * Scales[s], &config);
        if (!fonts[s][role]) { config.SizePixels = Sizes[role] * Scales[s]; fonts[s][role] = atlas->AddFontDefault(&config); }
    }
    return true;
}
}
void AddFonts() {
    startup_fonts::Install(ImGui::GetIO().Fonts);
    // 比較用の旧経路。通常は最初の表示サイズに必要な6書体だけをPrepareで登録する。
    if (std::getenv("CCCASTER_STARTUP_FONTS_BASELINE"))
        for (unsigned s = 0; s < 6; ++s) AddFontScale(s);
}
void Prepare(IDirect3DDevice9* device) {
    auto& io = ImGui::GetIO();
    const auto client = io.DisplaySize;
    IDirect3DSurface9* buffer = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &buffer))) {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(buffer->GetDesc(&desc))) {
            io.DisplaySize = ImVec2(float(desc.Width), float(desc.Height));
        }
        buffer->Release();
    }
    if (game_interface::borderless::Active()) {
        const auto rect = game_interface::borderless::FitContent(LONG(client.x), LONG(client.y));
        inputSpace.Begin({float(rect.right - rect.left), float(rect.bottom - rect.top)}, io.DisplaySize,
                         {float(rect.left), float(rect.top)});
    } else inputSpace.Begin(client, io.DisplaySize);
    D3DVIEWPORT9 viewport{};
    Rect bounds{0, 0, io.DisplaySize.x, io.DisplaySize.y};
    if (SUCCEEDED(device->GetViewport(&viewport)) && viewport.Width && viewport.Height &&
        viewport.X + viewport.Width <= io.DisplaySize.x && viewport.Y + viewport.Height <= io.DisplaySize.y)
        bounds = {float(viewport.X), float(viewport.Y), float(viewport.Width), float(viewport.Height)};
    layout = Layout::Fit(bounds);
    if (AddFontScale(FontScale(layout.scale))) {
        // NewFrame前にだけatlasを変更する。リサイズで必要になった倍率も一度だけ追加し、
        // 同じTTF・サイズ・oversamplingを維持する。既存ImFontポインターは保持される。
        ImGui_ImplDX9_InvalidateDeviceObjects();
        if (diagnostics::startup::Enabled())
            domain::session::DebugLog("[StartupFonts] scale=%g fonts=%d",
                Scales[FontScale(layout.scale)], io.Fonts->Fonts.Size);
    }
    const auto mode = domain::session::SceneRunner::AppMode();
    const auto players = mode == 4 ? std::array<std::shared_ptr<const emblem::Image>, 2>{}
                                   : emblem::Store::Players(mode == 2);
    for (unsigned side = 0; side < 2; ++side) textures[side].Update(device, players[side].get());
    domain::ui::training_character_view::Prepare(device);
}
void FinishInput() { inputSpace.End(); }
void Release() {
    for (auto& texture : textures) texture.Release();
    domain::ui::training_character_view::Release();
}
Layout CurrentLayout() { return layout; }
ImFont* Font(unsigned role, float scale) {
    const unsigned s = FontScale(scale);
    return fonts[s][(std::min)(role, 5u)] ? fonts[s][(std::min)(role, 5u)] : ImGui::GetFont();
}
ImTextureID Emblem(unsigned player) { return reinterpret_cast<ImTextureID>(textures[player % 2].Get()); }
}
