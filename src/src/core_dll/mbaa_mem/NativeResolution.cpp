#include "core_dll/mbaa_mem/NativeResolution.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/hook/BorderlessDisplay.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/engine/SelectionPreferences.hpp"
#include <MinHook.h>
#include <cstring>
#include <vector>

namespace cccaster::game_interface::native_resolution {
namespace {
using borderless::Resolution;
void* originalWindow = nullptr;
bool installed = false, attempted = false, pending = false;
bool saveOnCompletion = false;
Resolution requested{}, previous{}, actual{};
long resetResult = E_PENDING;
constexpr uintptr_t Width = 0x54D048, Height = 0x54D04C;
constexpr uintptr_t KeepWindowMode = 0x74DFA8, ResetRequest = 0x76E660;
template<class T> T& At(uintptr_t address) { return *reinterpret_cast<T*>(address); }

// 0x4A3EC0: ESI=HWND欄のポインター、stack=Windowed、callerが引数を破棄する。
__attribute__((naked)) unsigned __cdecl ForwardWindow(void*, HWND*, int) {
    __asm__ __volatile__(
        "pushl %esi\n\tmovl 12(%esp),%esi\n\tpushl 16(%esp)\n\t"
        "call *12(%esp)\n\taddl $4,%esp\n\tpopl %esi\n\tret\n\t");
}
extern "C" __attribute__((force_align_arg_pointer)) unsigned __cdecl NativeResolutionWindowImpl(HWND* window, int windowed) {
    if (!pending) return ForwardWindow(originalWindow, window, windowed);
    // 資源解放→Reset→資源再生成は元の0x40E220で完了済み。
    // この要求だけ既存ボーダーレスの窓処理へ戻し、標準側の排他切替・INI保存を行わない。
    const bool resetOk = SUCCEEDED(resetResult) && actual.width == requested.width && actual.height == requested.height;
    const bool applied = resetOk && borderless::ApplyRenderResolution(actual);
    const auto display = borderless::GetDisplaySettings();
    domain::session::DebugLog(
        "[NativeResolution] COMPLETE applied=%d requested=%dx%d backbuffer=%dx%d fullscreen=%d window=%dx%d hr=0x%08lX",
        int(applied), requested.width, requested.height, actual.width, actual.height, int(display.fullscreen),
        display.windowSize.width, display.windowSize.height, static_cast<unsigned long>(resetResult));
    if (!resetOk) {
        At<uint32_t>(Width) = previous.width;
        At<uint32_t>(Height) = previous.height;
    }
    pending = false;
    if (applied && saveOnCompletion) domain::scene::selection_preferences::SaveResolution(actual.width, actual.height);
    saveOnCompletion = false;
    return applied;
}
__attribute__((naked)) void WindowHook() {
    __asm__ __volatile__(
        "pushl 4(%esp)\n\tpushl %esi\n\tcall _NativeResolutionWindowImpl\n\taddl $8,%esp\n\tret\n\t");
}
bool Install() {
    if (attempted) return installed;
    if (!game_build::RuntimeValidated()) return false;
    attempted = true;
    constexpr unsigned char windowSignature[]{0x83,0xec,0x14,0x8b,0x06,0x33,0xc9};
    constexpr unsigned char resetSignature[]{0x83,0xec,0x74,0x53,0x55,0x56,0x33,0xed,0x33,0xf6};
    if (std::memcmp(reinterpret_cast<void*>(0x4A3EC0),windowSignature,sizeof(windowSignature)) ||
        std::memcmp(reinterpret_cast<void*>(0x40E220),resetSignature,sizeof(resetSignature))) return false;
    const auto init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
    auto* site = reinterpret_cast<void*>(0x4A3EC0);
    if (MH_CreateHook(site,reinterpret_cast<void*>(WindowHook),&originalWindow) != MH_OK) return false;
    if (MH_EnableHook(site) != MH_OK) { MH_RemoveHook(site); originalWindow = nullptr; return false; }
    installed = true;
    return true;
}
Resolution Next(Resolution current, int direction) {
    // 標準RESOLUTIONが0x4A3270でコピーする16byte単位の候補表。幅・高さで重複除去済み。
    const auto begin = At<uintptr_t>(0x7B08C4), end = At<uintptr_t>(0x7B08C8);
    std::vector<Resolution> modes;
    const auto limit = borderless::ResolutionLimit();
    if (begin && end > begin && end-begin <= 256*16 && (end-begin)%16 == 0 &&
        !IsBadReadPtr(reinterpret_cast<void*>(begin),end-begin)) {
        for (auto item=begin; item<end; item+=16) {
            const Resolution size{At<int>(item),At<int>(item+4)};
            if (size.width >= 640 && size.height >= 480 && size.width <= limit.width && size.height <= limit.height)
                modes.push_back(size);
        }
    }
    // 起動ダイアログを省略した環境では標準候補表が未生成の場合がある。
    if (modes.empty()) return borderless::NextResolution(current,limit,direction);
    for (size_t i=0; i<modes.size(); ++i)
        if (modes[i].width == current.width && modes[i].height == current.height)
            return modes[(i + (direction>0 ? 1 : modes.size()-1)) % modes.size()];
    return direction>0 ? modes.front() : modes.back();
}
}
ScreenResolution Read() {
    if (!Install() || !At<uintptr_t>(0x76E7D4)) return {};
    const auto width = At<uint32_t>(Width), height = At<uint32_t>(Height);
    if (width < 320 || height < 240 || width > 16384 || height > 16384) return {};
    return {int(width),int(height),true,pending};
}
bool RequestSize(Resolution next, bool save) {
    const auto state = Read();
    if (!state.available || state.pending || *CC_GAME_MODE_ADDR != CC_GAME_MODE_CHARA_SELECT ||
        At<uint32_t>(ResetRequest) || !borderless::GetDisplaySettings().available) return false;
    if (!next.width || (next.width == state.width && next.height == state.height)) return false;
    previous = {state.width,state.height}; requested = next; actual = {};
    saveOnCompletion = save;
    resetResult = E_PENDING; pending = true;
    // 0x4323AB..0x4323CDのCHANGE SCREENと同じ要求。Windowedは維持する。
    At<uint32_t>(Width) = next.width;
    At<uint32_t>(Height) = next.height;
    At<uint32_t>(KeepWindowMode) = 1;
    At<uint32_t>(ResetRequest) = 1;
    domain::session::DebugLog("[NativeResolution] REQUEST size=%dx%d fullscreen=%d",
        next.width,next.height,int(borderless::Active()));
    return true;
}
bool Request(int direction) {
    const auto state = Read();
    return direction && state.available && RequestSize(Next({state.width,state.height},direction), true);
}
bool Restore(int width, int height) {
    const auto limit = borderless::ResolutionLimit();
    if (width < 640 || height < 480 || width > limit.width || height > limit.height) {
        // 別モニターでは収まる最大候補へ。保存済みの希望寸法は書き換えない。
        const auto fallback = borderless::NextResolution({}, limit, -1);
        return fallback.width && RequestSize(fallback, false);
    }
    return RequestSize({width,height}, false);
}
void ResetFinished(long result, unsigned width, unsigned height) {
    if (!pending) return;
    resetResult = result;
    actual = {int(width),int(height)};
}
}
