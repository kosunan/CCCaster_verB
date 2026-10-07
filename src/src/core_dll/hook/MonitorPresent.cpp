#include "core_dll/hook/MonitorPresent.hpp"
#include "core_dll/hook/BorderlessDisplay.hpp"
#include "core_dll/hook/RenderProbe.hpp"
#include "core_dll/timing/DisplayCadence.hpp"
#include "core_dll/timing/FrameTiming.hpp"
#include "core_dll/timing/IdlePresentation.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include <algorithm>
#include <atomic>
#include <vector>

namespace cccaster::game_interface::monitor_present {
namespace {
IDirect3DDevice9* device = nullptr;
IDirect3DSwapChain9* swapChain = nullptr;
HWND window = nullptr;
HMONITOR monitor = nullptr;
bool supported = false, active = false, ready = false;
core::timer::DisplayCadence cadence;
uint32_t rateNumerator = 0, rateDenominator = 0, sourceFrame = 0;
uint32_t sourceMode = 0, sourceIntro = 0, sourceWorld = 0;
uint64_t serial = 0, image = 0, lastImage = 0, repeats = 0;
uint64_t busy = 0, missedBase = 0;
int64_t guardUs = 1000, reportAt = 0, retryAt = 0;

bool Rephase(uint32_t numerator, uint32_t denominator) {
    core::timer::DisplayCadence next;
    if (!next.Configure(numerator, denominator, platform::RealMonotonicTicks())) return false;
    missedBase += cadence.Missed(); cadence = next;
    return true;
}

bool RefreshRate(HMONITOR target, uint32_t& numerator, uint32_t& denominator) {
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(target, &info)) return false;
    // Windowsの現在の出力経路と窓のモニターを対応付ける。整数Hzへ丸めない。
    for (unsigned attempt = 0; attempt != 3; ++attempt) {
        UINT32 pathsCount = 0, modesCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathsCount, &modesCount) != ERROR_SUCCESS)
            break;
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathsCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modesCount);
        const auto status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathsCount, paths.data(),
                                               &modesCount, modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) continue;
        if (status != ERROR_SUCCESS) break;
        for (UINT32 i = 0; i < pathsCount; ++i) {
            const auto& path = paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
            name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name),
                           path.sourceInfo.adapterId, path.sourceInfo.id};
            if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS ||
                wcscmp(name.viewGdiDeviceName, info.szDevice)) continue;
            const auto rate = path.targetInfo.refreshRate;
            if (rate.Numerator && rate.Denominator) {
                numerator = rate.Numerator; denominator = rate.Denominator;
                return true;
            }
        }
        break;
    }
    DEVMODEW mode{}; mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency < 20)
        return false;
    numerator = mode.dmDisplayFrequency; denominator = 1;
    domain::session::DebugLog("[MonitorRefresh] integer fallback device=%ls", info.szDevice);
    return true;
}

HRESULT Display() {
    if (!ready || !active) return D3D_OK;
    const auto started = platform::RealMonotonicTicks();
    if (!cadence.Due(started) || started < retryAt) return D3D_OK;
    if (IsIconic(window)) { cadence.Presented(started); return D3D_OK; }
    HRESULT result = D3D_OK;
    const bool ignored = render_probe::ignored;
    render_probe::ignored = true;
    // Device::Presentにはフラグがない。元のswap chainのPresentを使い、
    // GPUが詰まったらゲームスレッドを待たせず、最新画像で後から再試行する。
    if (!borderless::Present(device, result, D3DPRESENT_DONOTWAIT))
        result = swapChain->Present(nullptr, nullptr, nullptr, nullptr, D3DPRESENT_DONOTWAIT);
    render_probe::ignored = ignored;
    const auto ended = platform::RealMonotonicTicks();
    // ドライバが遅い場合は次のゲーム締切直前に追加表示を入れない。
    guardUs = std::max<int64_t>(1000, (ended - started) / 60 + 500);
    if (result == D3DERR_WASSTILLDRAWING) {
        ++busy; retryAt = ended + 15000; // 250µs。忙しいGPUへ空回りで連打しない。
        return D3D_OK;
    }
    retryAt = 0;
    cadence.Presented(ended);
    if (FAILED(result)) {
        ready = active = false;
        domain::session::DebugLog("[MonitorPresent] failed hr=%08X; native presentation until Reset", unsigned(result));
        supported = false;
        return result;
    }
    ++serial;
    if (!diagnostics::startup::presentRecorded &&
        (sourceMode == CC_GAME_MODE_CHARA_SELECT || sourceMode == CC_GAME_MODE_REPLAY)) {
        diagnostics::startup::presentRecorded = true;
        diagnostics::startup::Mark(sourceMode == CC_GAME_MODE_REPLAY ? "replay_present" : "chara_present");
    }
    const bool repeat = image == lastImage;
    repeats += repeat; lastImage = image;
    auto& timing = core::timer::FrameTiming::Get();
    timing.Observe(started / 60, sourceFrame, false);
    static const bool trace = std::getenv("CCCASTER_MONITOR_PRESENT_TRACE") != nullptr;
    if (trace)
        domain::session::DebugLog("[MonitorPresent] seq=%llu image=%llu frame=%u qpc=%lld cost=%lld repeat=%u missed=%llu mode=%u intro=%u world=%u",
            serial, image, sourceFrame, started, ended - started, unsigned(repeat), missedBase + cadence.Missed(),
            sourceMode, sourceIntro, sourceWorld);
    if (ended >= reportAt) {
        reportAt = ended + 60000000;
        domain::session::DebugLog("[MonitorTiming] hz=%u/%u presents=%llu repeats=%llu missed=%llu fps=%.3f gameFps=%.3f guardUs=%lld busy=%llu",
            rateNumerator, rateDenominator, serial, repeats, missedBase + cadence.Missed(), timing.displayFps,
            core::timer::FrameTiming::Simulation().gameFps, guardUs, busy);
    }
    return result;
}
}

void Prepare(IDirect3DDevice9* value) {
    if (value != device) {
        Reset(); device = value;
        D3DPRESENT_PARAMETERS parameters{};
        if (SUCCEEDED(device->GetSwapChain(0, &swapChain))) {
            const auto result = swapChain->GetPresentParameters(&parameters);
            // 通常版のCOPYバックバッファはPresent後も保持される。DISCARDでは
            // 未定義画像を再表示しない。別スレッドやGPU readbackも追加しない。
            supported = SUCCEEDED(result) && parameters.SwapEffect == D3DSWAPEFFECT_COPY &&
                parameters.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE;
            window = parameters.hDeviceWindow;
        }
        if (!window) {
            D3DDEVICE_CREATION_PARAMETERS creation{};
            if (SUCCEEDED(device->GetCreationParameters(&creation))) window = creation.hFocusWindow;
        }
        static const bool disabled = std::getenv("CCCASTER_DISABLE_MONITOR_PRESENT") != nullptr;
        supported = supported && window && !disabled;
        domain::session::DebugLog("[MonitorRefresh] supported=%u swap=%u interval=%u", unsigned(supported),
            unsigned(parameters.SwapEffect), unsigned(parameters.PresentationInterval));
    }
    if (!supported) return;
    const auto target = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (target == monitor && !displayChanged.exchange(false)) return;
    displayChanged = false; monitor = target;
    uint32_t numerator = 0, denominator = 0;
    const bool found = RefreshRate(target, numerator, denominator);
    // 検証専用。モニター設定は変更せず、表示要求だけを指定Hzで測定する。
    if (const auto* value = std::getenv("CCCASTER_TEST_MONITOR_HZ")) {
        char* end = nullptr;
        const auto requested = std::strtoul(value, &end, 10);
        if (end != value && !*end && requested >= 20 && requested <= 1000) {
            domain::session::DebugLog("[MonitorRefresh] testHz=%lu actual=%u/%u", requested, numerator, denominator);
            numerator = uint32_t(requested); denominator = 1;
        }
    }
    active = found && Rephase(numerator, denominator);
    rateNumerator = numerator; rateDenominator = denominator;
    core::timer::FrameTiming::Get().Reset();
    domain::session::DebugLog("[MonitorRefresh] active=%u hz=%u/%u monitor=%p", unsigned(active),
        numerator, denominator, target);
}
bool Active() { return active; }
bool Submit(bool skipped, const RECT* source, const RECT* destination, HWND overrideWindow,
            const RGNDATA* dirty, HRESULT& result) {
    ready = false;
    if (!active) return false;
    if (source || destination || overrideWindow || dirty) {
        // 現行ゲームは全引数NULL。部分更新・別窓の引数を待機中へ持ち越さない。
        supported = active = false;
        domain::session::DebugLog("[MonitorPresent] non-default rectangles/window; native presentation");
        return false;
    }
    if (skipped) return true;
    ready = true; ++image;
    sourceFrame = core::netplay::NetplaySession::GetState().appliedFrame.load();
    auto& memory = GameMem();
    // 起動・意図した描画省略・シーン境界の時間を表示欠落へ数えない。
    // 通常戦闘の連続更新では絶対締切を延長しない。
    if (image == 1 || sourceMode != memory.GameMode() || (sourceIntro && !memory.IntroState()))
        Rephase(rateNumerator, rateDenominator);
    sourceMode = memory.GameMode(); sourceIntro = memory.IntroState(); sourceWorld = memory.WorldTimer();
    result = Display();
    return true;
}
void Pump(int64_t remainingUs) {
    // An image not yet presented may still need its first attempt or a retry
    // after WASSTILLDRAWING. Suppress only duplicate-image presentation.
    if (ready && active && remainingUs > guardUs &&
        core::timer::IdlePresentation::MayPresent(image == lastImage)) Display();
}
void Reset() {
    if (swapChain) swapChain->Release();
    swapChain = nullptr; device = nullptr; window = nullptr; monitor = nullptr;
    supported = active = ready = false; displayChanged = true;
    guardUs = 1000; reportAt = retryAt = 0;
    missedBase += cadence.Missed(); cadence = {};
}
}
