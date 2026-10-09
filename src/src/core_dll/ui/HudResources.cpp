#include "core_dll/ui/HudResources.hpp"
#include "core_dll/ui/StartupFontCache.hpp"
#include "core_dll/ui/TrainingCharacterView.hpp"
#include "core_dll/ui/TrainingPaletteView.hpp"
#include "core_dll/ui/HudInputSpace.hpp"
#include "core_dll/ui/HudDrawSpace.hpp"
#include "core_dll/hook/BorderlessDisplay.hpp"
#include "shared_contracts/EmblemTexture.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/NativeHud.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include <imgui_impl_dx9.h>
#include <cstdlib>
#include <string>

namespace cccaster::hud {
namespace {
constexpr float Scales[]{1, 1.5f, 2, 3, 4, 6};
constexpr float Sizes[]{15, 13, 24, 10, 22, 12, 22};
constexpr unsigned FontRoles = sizeof(Sizes)/sizeof(Sizes[0]);
ImFont* fonts[6][FontRoles]{};
emblem::Texture textures[2];
Layout layout;
Rect nativeViewport;
InputSpace inputSpace;
ImVec2 bufferSize;
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
    for (unsigned role = 0; role < FontRoles; ++role) {
        const std::string path = std::string(directory) + "\\Fonts\\" +
            (role == 4 ? "arialbd.ttf" : role == 5 ? "tahoma.ttf" : role == PlayerNameFont ? "arial.ttf" : "tahomabd.ttf");
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
    // 比較用の旧経路。通常は最初の表示サイズに必要な書体だけをPrepareで登録する。
    if (std::getenv("CCCASTER_STARTUP_FONTS_BASELINE"))
        for (unsigned s = 0; s < 6; ++s) AddFontScale(s);
}
void Prepare(IDirect3DDevice9* device) {
    game_interface::native_hud::Prepare(device);
    auto& io = ImGui::GetIO();
    const auto client = io.DisplaySize;
    bufferSize = client;
    IDirect3DSurface9* buffer = nullptr;
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &buffer))) {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(buffer->GetDesc(&desc))) {
            bufferSize = ImVec2(float(desc.Width), float(desc.Height));
        }
        buffer->Release();
    }
    ImVec2 origin{};
    if (game_interface::borderless::Active()) {
        const auto rect = game_interface::borderless::FitContent(LONG(client.x), LONG(client.y));
        io.DisplaySize = {float(rect.right - rect.left), float(rect.bottom - rect.top)};
        origin = {float(rect.left), float(rect.top)};
    }
    // HUDは窓／全画面の実際の表示領域で等倍率に配置する。全画面の黒帯は除く。
    if (io.DisplaySize.x <= 0 || io.DisplaySize.y <= 0) io.DisplaySize = bufferSize;
    inputSpace.Begin(io.DisplaySize, io.DisplaySize, origin);
    D3DVIEWPORT9 viewport{};
    Rect bounds{0, 0, io.DisplaySize.x, io.DisplaySize.y};
    if (SUCCEEDED(device->GetViewport(&viewport)) && viewport.Width && viewport.Height &&
        viewport.X + viewport.Width <= bufferSize.x && viewport.Y + viewport.Height <= bufferSize.y)
        bounds = DisplayViewport({float(viewport.X), float(viewport.Y), float(viewport.Width), float(viewport.Height)},
                                 bufferSize, io.DisplaySize);
    nativeViewport = bounds;
    layout = Layout::Fit(bounds);
    const int aspect = game_interface::GameMem().DisplayOption(game_interface::NativeDisplayOption::AspectRatio);
    if (aspect >= 0) {
        const auto area = game_interface::CompositeImageRect(int(bufferSize.x),int(bufferSize.y),unsigned(aspect),
                                                            int(io.DisplaySize.x),int(io.DisplaySize.y));
        if (area.x1 > area.x0 && area.y1 > area.y0)
            nativeViewport = DisplayViewport({float(area.x0),float(area.y0),float(area.x1-area.x0),float(area.y1-area.y0)},
                                             bufferSize,io.DisplaySize);
    }
    static ImVec2 lastBuffer{}, lastDisplay{};
    if (lastBuffer.x != bufferSize.x || lastBuffer.y != bufferSize.y ||
        lastDisplay.x != io.DisplaySize.x || lastDisplay.y != io.DisplaySize.y) {
        domain::session::DebugLog("[HudGeometry] buffer=%gx%g display=%gx%g scale=%g",
            bufferSize.x, bufferSize.y, io.DisplaySize.x, io.DisplaySize.y, layout.scale);
        lastBuffer = bufferSize; lastDisplay = io.DisplaySize;
    }
    const bool addedLayoutFonts = AddFontScale(FontScale(layout.scale));
    const bool addedIdentityFonts = AddFontScale(FontScale(Layout::Fit(nativeViewport).scale));
    if (addedLayoutFonts || addedIdentityFonts) {
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
    domain::ui::training_palette_view::Prepare(device);
}
void FinishInput() { inputSpace.End(); }
void RenderDrawData() {
    auto* data = ImGui::GetDrawData();
    // このフレームの描画データへ一度だけ適用。提示の繰返しでは再描画しない。
    MapDrawDataToBuffer(*data, bufferSize);
    ImGui_ImplDX9_RenderDrawData(data);
}
void Release() {
    game_interface::native_hud::Release();
    for (auto& texture : textures) texture.Release();
    domain::ui::training_character_view::Release();
    domain::ui::training_palette_view::Release();
}
Layout CurrentLayout() { return layout; }
Rect CurrentViewport() { return nativeViewport; }
Rect BackbufferToDisplay(Rect bounds) { return DisplayViewport(bounds,bufferSize,ImGui::GetIO().DisplaySize); }
ImFont* Font(unsigned role, float scale) {
    const unsigned s = FontScale(scale);
    return fonts[s][(std::min)(role, FontRoles-1)] ? fonts[s][(std::min)(role, FontRoles-1)] : ImGui::GetFont();
}
ImTextureID Emblem(unsigned player) { return reinterpret_cast<ImTextureID>(textures[player % 2].Get()); }
}
