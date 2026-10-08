#include "core_dll/engine/SelectionPreferences.hpp"
#include "core_dll/common/DataPaths.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/hook/DisplaySettings.hpp"
#include "core_dll/ui/HudDisplay.hpp"
#include <atomic>

namespace cccaster::domain::scene::selection_preferences {
namespace {
Store store;
bool restoreLocal = false, restoreResolution = false, restoreFullscreen = false;
std::atomic<bool> saveFailed{false};
void Saved(bool ok) {
    saveFailed = !ok;
    session::DebugLog("[SelectionPreferences] SAVE success=%d", int(ok));
}
}
void Initialize() {
    const auto root = core::paths::GetDataRoot();
    // ゲームを持たない同期harnessではローカル設定を読書きしない。
    store.Load(root.empty() ? std::string{} : core::paths::Resolve("display.ini"));
    restoreLocal = restoreResolution = restoreFullscreen = !root.empty();
    saveFailed = false;
    if (store.Get(Key::Hud) >= 0) ui::HudDisplay::Set(ui::HudDisplayMode(store.Get(Key::Hud)));
}
bool Restore() {
    if (!restoreLocal && !restoreResolution && !restoreFullscreen) return false;
    auto& mem = game_interface::GameMem();
    namespace display = game_interface::borderless;
    if (restoreLocal && mem.StageAnimation() >= 0) {
        if (store.Get(Key::Animation) >= 0) mem.SetStageAnimation(store.Get(Key::Animation) == 1);
        for (unsigned i = 0; i < 4; ++i) {
            const auto key = Key(unsigned(Key::CharacterFilter) + i);
            if (store.Get(key) >= 0) mem.SetDisplayOption(game_interface::NativeDisplayOption(i), store.Get(key));
        }
        restoreLocal = false;
    }
    const auto resolution = mem.RenderResolution();
    if (restoreResolution && resolution.available && !resolution.pending) {
        if (store.Get(Key::Width) >= 0) mem.SetRenderResolution(store.Get(Key::Width), store.Get(Key::Height));
        restoreResolution = false;
    }
    if (restoreFullscreen && !restoreResolution) {
        if (mem.RenderResolution().pending) return true;
        const auto current = display::GetDisplaySettings();
        if (current.available) {
            const auto fullscreen = store.Get(Key::Fullscreen);
            if (fullscreen >= 0) display::SetFullscreen(fullscreen == 1);
            restoreFullscreen = false;
            session::DebugLog("[SelectionPreferences] RESTORED animation=%d hud=%s size=%dx%d fullscreen=%d native=%d,%d,%d,%d",
                mem.StageAnimation(), ui::HudDisplay::Name(), mem.RenderResolution().width, mem.RenderResolution().height,
                int(display::GetDisplaySettings().fullscreen), mem.DisplayOption(game_interface::NativeDisplayOption::CharacterFilter),
                mem.DisplayOption(game_interface::NativeDisplayOption::ScreenFilter), mem.DisplayOption(game_interface::NativeDisplayOption::AspectRatio),
                mem.DisplayOption(game_interface::NativeDisplayOption::ViewFps));
        }
    }
    return false;
}
void Save(Key key, int value) { Saved(store.Set(key, value)); }
void SaveResolution(int width, int height) { Saved(store.SetResolution(width, height)); }
bool SaveFailed() { return saveFailed.load(); }
}
