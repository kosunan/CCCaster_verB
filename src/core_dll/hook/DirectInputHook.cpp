#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/hook/DirectInputHook.hpp"
#include "core_dll/hook/TimeHooks.hpp"
#include "core_dll/hook/ControllerProfile.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include "core_dll/common/DataPaths.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/InputDiagnostic.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/VirtualControllerTest.hpp"
#include "core_dll/ui/HudDisplay.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include <windows.h>
#include <dinput.h>
#include <cfgmgr32.h>
#include <hidsdi.h>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>
#include <imgui.h>
#include <fstream>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <unordered_map>
#include <new>
#include <memory>
static std::recursive_mutex g_inputMutex;

using namespace cccaster::game_interface;
using namespace cccaster::main_app;

static IDirectInput8 *g_pDI = nullptr;
static bool g_initialized = false;
static bool g_devicesEnumerated = false;
static std::vector<DIDEVICEINSTANCE> g_initialDevices;
static std::atomic<bool> g_initializationStarted{false};
static std::vector<std::wstring> g_initialHidPaths;
static bool g_initialTopologyKnown = false;

// PnPが保持する接続パスだけを取得する。HIDを開いたりDirectInput内部の排他を取ったりしない。
static bool ReadHidPaths(std::vector<std::wstring> &paths) {
    GUID hid;
    HidD_GetHidGuid(&hid);
    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG size = 0;
        if (CM_Get_Device_Interface_List_SizeW(&size, &hid, nullptr,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return false;
        std::vector<wchar_t> buffer(std::max<ULONG>(size, 2), L'\0');
        const auto status = CM_Get_Device_Interface_ListW(&hid, nullptr, buffer.data(), buffer.size(),
                                                          CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (status == CR_BUFFER_SMALL) continue;
        if (status != CR_SUCCESS) return false;
        paths.clear();
        for (size_t offset = 0; offset < buffer.size() && buffer[offset];) {
            const auto begin = buffer.begin() + offset;
            const auto end = std::find(begin, buffer.end(), L'\0');
            if (end == buffer.end()) return false;
            paths.emplace_back(begin, end);
            offset += static_cast<size_t>(end - begin) + 1;
        }
        std::sort(paths.begin(), paths.end());
        return true;
    }
    return false;
}

static BOOL CALLBACK RememberInitialDevice(const DIDEVICEINSTANCE *instance, VOID *) {
    g_initialDevices.push_back(*instance);
    return DIENUM_CONTINUE;
}

// HWNDへの関連付けや設定の公開は行わない。Initialize()と同じ排他で所有権を渡す。
static bool PrepareInitialDevices() {
    if (!g_pDI && FAILED(DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
                                           IID_IDirectInput8, (VOID **)&g_pDI, nullptr)))
        return false;
    if (!g_devicesEnumerated) {
        // 列挙前に記録し、その途中で挿された機器も後続の差分検出から落とさない。
        g_initialTopologyKnown = ReadHidPaths(g_initialHidPaths);
        g_initialDevices.clear();
        g_devicesEnumerated = SUCCEEDED(g_pDI->EnumDevices(
            DI8DEVCLASS_GAMECTRL, RememberInitialDevice, nullptr, DIEDFL_ATTACHEDONLY));
    }
    return g_devicesEnumerated;
}

static DWORD WINAPI PrepareInputThread(void *context) {
    const auto module = static_cast<HMODULE>(context);
    cccaster::diagnostics::startup::Mark("input_async_begin");
    bool prepared = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
        // 先にゲームスレッドが初期化した場合は再列挙しない。
        try { prepared = g_initialized || PrepareInitialDevices(); }
        catch (...) { g_initialDevices.clear(); g_devicesEnumerated = false; }
    }
    cccaster::domain::session::DebugLog("[StartupInput] prepared=%u", prepared ? 1u : 0u);
    cccaster::diagnostics::startup::Mark("input_async_end");
    FreeLibraryAndExitThread(module, prepared ? 0 : 1);
}

void DirectInputHook::BeginInitialize() {
    if (g_initializationStarted.exchange(true, std::memory_order_acq_rel)) return;
    HMODULE module = nullptr;
    // ワーカーの実行中にDLLが解放されないよう参照を所有する。失敗時は通常初期化へ戻る。
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&PrepareInputThread), &module)) return;
    HANDLE thread = CreateThread(nullptr, 0, PrepareInputThread, module, 0, nullptr);
    if (thread) CloseHandle(thread);
    else FreeLibrary(module);
}

struct ControllerData {
    IDirectInputDevice8 *device;
    GUID instanceGuid;
    std::string guidText;
    DIJOYSTATE2 state;
    DIJOYSTATE2 prevState; // UI frame edge detection
    DIJOYSTATE2 uiState;
    int id;
    DWORD product;
    char name[256];
};

static std::vector<ControllerData> g_Controllers;
static HWND g_hwnd = nullptr;
static std::atomic<bool> g_deviceRefreshRequested{false};
static ULONGLONG g_nextDeviceProbeMs = 0;
static constexpr ULONGLONG kDeviceProbeIntervalMs = 1000;

static std::string FormatGuid(const GUID &guid) {
    char text[40]{};
    std::snprintf(text, sizeof(text), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                  guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return text;
}

struct AttachedDeviceInfo {
    GUID instanceGuid;
};

static BOOL CALLBACK RememberProbeDevice(const DIDEVICEINSTANCE *instance, VOID *context) {
    static_cast<std::vector<DIDEVICEINSTANCE> *>(context)->push_back(*instance);
    return DIENUM_CONTINUE;
}

static bool ContainsGuid(const std::vector<AttachedDeviceInfo> &devices, const GUID &guid) {
    return std::any_of(devices.begin(), devices.end(), [&](const auto &device) {
        return InlineIsEqualGUID(device.instanceGuid, guid) != FALSE;
    });
}

static void ReleaseControllers(std::vector<ControllerData> &devices) {
    for (auto &device : devices) {
        if (device.device) { device.device->Unacquire(); device.device->Release(); }
    }
    devices.clear();
}

// テストモード用: パケット値で入力をオーバーライド
static bool g_testModeEnabled = false;
static uint32_t g_testInputP1 = 0; // (direction<<16)|buttons
static uint32_t g_testInputP2 = 0;

BOOL CALLBACK EnumObjectsCallback(const DIDEVICEOBJECTINSTANCE *pdidoi, VOID *pContext) {
    if (pdidoi->dwType & DIDFT_AXIS) {
        DIPROPRANGE diprg;
        diprg.diph.dwSize = sizeof(DIPROPRANGE);
        diprg.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        diprg.diph.dwHow = DIPH_BYID;
        diprg.diph.dwObj = pdidoi->dwType;
        diprg.lMin = -32768;
        diprg.lMax = 32767;
        ((IDirectInputDevice8 *)pContext)->SetProperty(DIPROP_RANGE, &diprg.diph);

        // Disable internal deadzone so we can handle it manually with our THRESHOLD / DEADZONE logic
        DIPROPDWORD dipdw;
        dipdw.diph.dwSize = sizeof(DIPROPDWORD);
        dipdw.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        dipdw.diph.dwHow = DIPH_BYID;
        dipdw.diph.dwObj = pdidoi->dwType;
        dipdw.dwData = 0;
        ((IDirectInputDevice8 *)pContext)->SetProperty(DIPROP_DEADZONE, &dipdw.diph);
    }
    return DIENUM_CONTINUE;
}

struct ControllerBuild {
    IDirectInput8 *input;
    HWND hwnd;
    std::vector<ControllerData> *devices;
};

BOOL CALLBACK EnumJoysticksCallback(const DIDEVICEINSTANCE *pdidInstance, VOID *pContext) {
    ControllerBuild initial{};
    if (!pContext) initial = {g_pDI, g_hwnd, &g_Controllers};
    const auto &build = pContext ? *static_cast<ControllerBuild *>(pContext) : initial;
    IDirectInputDevice8 *joystick;
    if (FAILED(build.input->CreateDevice(pdidInstance->guidInstance, &joystick, nullptr))) {
        return DIENUM_CONTINUE;
    }
    const auto release = [](IDirectInputDevice8 *device) { device->Release(); };
    std::unique_ptr<IDirectInputDevice8, decltype(release)> pending(joystick, release);

    if (FAILED(joystick->SetDataFormat(&c_dfDIJoystick2)) ||
        FAILED(joystick->SetCooperativeLevel(build.hwnd, DISCL_NONEXCLUSIVE | DISCL_BACKGROUND))) {
        return DIENUM_CONTINUE;
    }
    joystick->EnumObjects(EnumObjectsCallback, joystick, DIDFT_AXIS);

    ControllerData data{};
    data.device = joystick;
    data.instanceGuid = pdidInstance->guidInstance;
    data.guidText = FormatGuid(data.instanceGuid);
    data.id = build.devices->size();
    data.product = pdidInstance->guidProduct.Data1;
    std::fill(std::begin(data.state.rgdwPOV), std::end(data.state.rgdwPOV), 0xFFFFFFFFu);
    data.uiState = data.prevState = data.state;
    strncpy(data.name, pdidInstance->tszInstanceName, sizeof(data.name) - 1);
    data.name[sizeof(data.name) - 1] = '\0';

    build.devices->push_back(std::move(data));
    pending.release();

    return DIENUM_CONTINUE;
}

// Running中はワーカーだけ、Idle/Ready中はゲームスレッドだけが内容を所有する。
// 入力mutexとDirectInput本体を共有せず、HID問い合わせの完了をゲーム側で待たない。
struct DeviceProbe {
    enum Phase { Idle, Running, Ready };
    std::atomic<Phase> phase{Idle};
    std::vector<AttachedDeviceInfo> known, attached;
    std::vector<ControllerData> added, retired;
    std::vector<std::wstring> hidPaths;
    bool topologyKnown = false;
    bool requested = false;
    HWND hwnd = nullptr;
    HRESULT status = E_PENDING;
    int64_t elapsedUs = 0;
    ~DeviceProbe() { ReleaseControllers(added); ReleaseControllers(retired); }
};
static std::shared_ptr<DeviceProbe> g_deviceProbe;
struct DeviceProbeWork {
    std::shared_ptr<DeviceProbe> probe;
    HMODULE module;
};

static void CALLBACK RunDeviceProbe(PTP_CALLBACK_INSTANCE instance, void *context) {
    std::unique_ptr<DeviceProbeWork> work(static_cast<DeviceProbeWork *>(context));
    // コールバックが戻ってからOSがDLL参照を解放する。待機中の専用スレッドは残さない。
    FreeLibraryWhenCallbackReturns(instance, work->module);
    auto &probe = *work->probe;
    const auto begin = cccaster::platform::RealMonotonicUs();
    IDirectInput8 *input = nullptr;
    bool enumerated = false;
    try {
        ReleaseControllers(probe.retired);
        probe.attached.clear();
        // 診断専用。数秒かかる列挙でもゲームが進行することを実機で検査する。
        if (const auto value = std::getenv("CCCASTER_TEST_DEVICE_PROBE_DELAY_MS"))
            cccaster::core::hooks::TimeHooks::RealSleep(static_cast<DWORD>(std::clamp(std::atoi(value), 0, 5000)));
        std::vector<std::wstring> paths;
        if (!ReadHidPaths(paths)) {
            probe.status = E_FAIL; // 取得失敗を「全機器が抜けた」と解釈しない。
        } else if (!probe.requested && probe.topologyKnown && paths == probe.hidPaths) {
            probe.attached = probe.known;
            probe.status = S_OK;
        } else {
            enumerated = true;
            probe.status = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION,
                                              IID_IDirectInput8, (VOID **)&input, nullptr);
            std::vector<DIDEVICEINSTANCE> attached;
            if (SUCCEEDED(probe.status))
                probe.status = input->EnumDevices(DI8DEVCLASS_GAMECTRL, RememberProbeDevice,
                                                  &attached, DIEDFL_ATTACHEDONLY);
            if (SUCCEEDED(probe.status)) {
                ControllerBuild build{input, probe.hwnd, &probe.added};
                bool allPrepared = true;
                for (const auto &device : attached) {
                    probe.attached.push_back({device.guidInstance});
                    if (!ContainsGuid(probe.known, device.guidInstance)) {
                        const auto before = probe.added.size();
                        EnumJoysticksCallback(&device, &build);
                        if (probe.added.size() == before) allPrepared = false;
                    }
                }
                probe.hidPaths = std::move(paths);
                // 接続直後でCreateDevice等がまだ失敗する場合は次の周期で再試行する。
                probe.topologyKnown = allPrepared;
            }
        }
    } catch (...) {
        probe.status = E_OUTOFMEMORY;
        ReleaseControllers(probe.added);
    }
    if (enumerated && FAILED(probe.status)) probe.topologyKnown = false;
    if (input) input->Release();
    probe.elapsedUs = cccaster::platform::RealMonotonicUs() - begin;
    static const bool trace = std::getenv("CCCASTER_DEVICE_TRACE") != nullptr;
    if (trace || FAILED(probe.status))
        cccaster::domain::session::DebugLog("[DeviceProbe] worker=%lu elapsedUs=%lld status=%08lx attached=%u added=%u enumerated=%d",
            GetCurrentThreadId(), probe.elapsedUs, static_cast<unsigned long>(probe.status),
            static_cast<unsigned>(probe.attached.size()), static_cast<unsigned>(probe.added.size()), enumerated);
    probe.phase.store(DeviceProbe::Ready, std::memory_order_release);
}

static bool SubmitDeviceProbe(const std::shared_ptr<DeviceProbe> &probe) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&RunDeviceProbe), &module)) return false;
    auto *work = new (std::nothrow) DeviceProbeWork{probe, module};
    if (!work) { FreeLibrary(module); return false; }
    probe->phase.store(DeviceProbe::Running, std::memory_order_release);
    if (TrySubmitThreadpoolCallback(RunDeviceProbe, work, nullptr)) return true;
    probe->phase.store(DeviceProbe::Idle, std::memory_order_release);
    delete work;
    FreeLibrary(module);
    return false;
}

bool DirectInputHook::Initialize(HWND hwnd) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    if (g_initialized)
        return true;

    g_hwnd = hwnd;
    if (!PrepareInitialDevices()) {
        return false;
    }

    for (const auto &instance : g_initialDevices) EnumJoysticksCallback(&instance, nullptr);
    if (g_Controllers.size() != g_initialDevices.size()) g_initialTopologyKnown = false;
    g_initialDevices.clear();
    g_deviceProbe = std::make_shared<DeviceProbe>();
    g_deviceProbe->hidPaths = std::move(g_initialHidPaths);
    g_deviceProbe->topologyKnown = g_initialTopologyKnown;
    g_nextDeviceProbeMs = GetTickCount64() + kDeviceProbeIntervalMs;
    g_deviceRefreshRequested.store(false, std::memory_order_release);
    ReloadConfigs();
    g_initialized = true;
    return true;
}

void DirectInputHook::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    // 実行中の問い合わせは自身のshared_ptrとDLL参照で完了する。ここでjoinしない。
    g_deviceProbe.reset();
    for (auto &ctrl : g_Controllers) {
        if (ctrl.device) {
            ctrl.device->Unacquire();
            ctrl.device->Release();
        }
    }
    g_Controllers.clear();
    g_initialDevices.clear();
    g_initialHidPaths.clear();
    g_initialTopologyKnown = false;
    g_devicesEnumerated = false;
    g_initialized = false;

    if (g_pDI) {
        g_pDI->Release();
        g_pDI = nullptr;
    }
    g_deviceRefreshRequested.store(false, std::memory_order_release);
    g_nextDeviceProbeMs = 0;
}

// DirectInput内部のメッセージ処理から同じPollへ再入しない。
static bool g_isPolling = false;

void DirectInputHook::RefreshDevices() {
    RequestDeviceRefresh();
}

void DirectInputHook::RequestDeviceRefresh() {
    g_deviceRefreshRequested.store(true, std::memory_order_release);
}

void DirectInputHook::UpdateHotplug() {
    // 入力採取がロックを持つ場合は次のフレームへ回す。問い合わせの待機はしない。
    std::unique_lock<std::recursive_mutex> lock(g_inputMutex, std::try_to_lock);
    if (!lock.owns_lock() || !g_deviceProbe || !g_initialized) return;
    auto &probe = *g_deviceProbe;
    auto phase = probe.phase.load(std::memory_order_acquire);
    if (phase == DeviceProbe::Running) return;
    if (phase == DeviceProbe::Ready) {
        bool changed = false;
        if (SUCCEEDED(probe.status)) {
            std::vector<ControllerData> next;
            next.reserve(g_Controllers.size() + probe.added.size());
            for (auto &device : g_Controllers) {
                if (ContainsGuid(probe.attached, device.instanceGuid))
                    next.push_back(std::move(device));
                else {
                    probe.retired.push_back(std::move(device));
                    changed = true;
                }
                device.device = nullptr; // COM所有権も移す。破棄は次のワーカーで行う。
            }
            for (auto &device : probe.added) {
                next.push_back(std::move(device));
                device.device = nullptr;
                changed = true;
            }
            probe.added.clear();
            g_Controllers.swap(next);
            for (size_t i = 0; i < g_Controllers.size(); ++i) g_Controllers[i].id = i;
        }
        probe.phase.store(DeviceProbe::Idle, std::memory_order_release);
        if (changed) {
            ReloadConfigs();
            cccaster::domain::session::DebugLog("[DirectInputHook] controller list published: %u device(s)",
                                               static_cast<unsigned>(g_Controllers.size()));
        }
    }

    const ULONGLONG now = GetTickCount64();
    if (now < g_nextDeviceProbeMs && !g_deviceRefreshRequested.load(std::memory_order_acquire)) return;
    probe.requested = g_deviceRefreshRequested.exchange(false, std::memory_order_acq_rel);
    g_nextDeviceProbeMs = now + kDeviceProbeIntervalMs;
    probe.known.clear();
    for (const auto &device : g_Controllers) probe.known.push_back({device.instanceGuid});
    probe.hwnd = g_hwnd;
    if (!SubmitDeviceProbe(g_deviceProbe)) {
        cccaster::domain::session::DebugLog("[DeviceProbe] submit failed error=%lu", GetLastError());
    }
}

void DirectInputHook::Poll() {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    if (g_isPolling)
        return;
    g_isPolling = true;

    // 範囲for文ではなくインデックスベースで安全に回す（念のため）
    size_t count = g_Controllers.size();
    for (size_t i = 0; i < count; ++i) {
        auto &ctrl = g_Controllers[i];
        if (!ctrl.device)
            continue;

        ctrl.device->Poll();

        DIJOYSTATE2 newState;
        HRESULT hr = ctrl.device->GetDeviceState(sizeof(DIJOYSTATE2), &newState);
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            ctrl.device->Acquire();
            hr = ctrl.device->GetDeviceState(sizeof(DIJOYSTATE2), &newState);
        }

        if (SUCCEEDED(hr)) {
            ctrl.state = newState;
        } else {
            // 抜けた瞬間に最後の押下を保持しない。対戦中の再列挙は行わない。
            ctrl.state = {};
            std::fill(std::begin(ctrl.state.rgdwPOV), std::end(ctrl.state.rgdwPOV), 0xFFFFFFFFu);
        }
    }

    g_isPolling = false;


}

// Constants for MBAA Inputs
#define CC_BUTTON_A 0x0010
#define CC_BUTTON_B 0x0020
#define CC_BUTTON_C 0x0008
#define CC_BUTTON_D 0x0004
#define CC_BUTTON_E 0x0080
#define CC_BUTTON_AB 0x0040
#define CC_BUTTON_START 0x0001
#define CC_BUTTON_FN1 0x0100
#define CC_BUTTON_FN2 0x0200
#define CC_BUTTON_CONFIRM 0x0400
#define CC_BUTTON_CANCEL 0x0800

#define AXIS_CENTERED 0
#define AXIS_POSITIVE 1
#define AXIS_NEGATIVE 2

static inline uint8_t mapAxisValue(LONG value, uint32_t deadzone) {
    LONG absValue = value < 0 ? -value : value;
    if (absValue > (LONG)deadzone)
        return (value > 0 ? AXIS_POSITIVE : AXIS_NEGATIVE);
    return AXIS_CENTERED;
}

static inline uint8_t mapHatValue(uint32_t value) {
    static const uint8_t values[] = {8, 9, 6, 3, 2, 1, 4, 7}; // U, UR, R, DR, D, DL, L, UL
    if (LOWORD(value) == 0xFFFF)
        return 5;
    value %= 36000;
    value /= 4500;
    return values[value];
}

static bool IsJoyButtonPressed(const DIJOYSTATE2 &state, int btn) {
    if (btn < 0 || btn >= 128)
        return false;
    return (state.rgbButtons[btn] & 0x80) != 0;
}

static bool IsJoyHatPressed(const DIJOYSTATE2 &state, int pov, int dir) {
    if (pov < 0 || pov >= 4)
        return false;
    uint8_t mapped = mapHatValue(state.rgdwPOV[pov]);
    if (mapped == 5)
        return false; // Centered
    // Strict cardinal checking including diagonals
    if (dir == 8 && (mapped == 7 || mapped == 8 || mapped == 9))
        return true; // Up
    if (dir == 6 && (mapped == 9 || mapped == 6 || mapped == 3))
        return true; // Right
    if (dir == 2 && (mapped == 3 || mapped == 2 || mapped == 1))
        return true; // Down
    if (dir == 4 && (mapped == 1 || mapped == 4 || mapped == 7))
        return true; // Left
    return false;
}

static bool IsJoyAxisPressed(const DIJOYSTATE2 &state, int axis, int sign) {
    LONG val = 0;
    switch (axis) {
    case 0:
        val = state.lX;
        break;
    case 1:
        val = -state.lY;
        break; // Invert Y
    case 2:
        val = state.lZ;
        break;
    case 3:
        val = state.lRx;
        break;
    case 4:
        val = -state.lRy;
        break; // Invert Y
    case 5:
        val = state.lRz;
        break;
    case 6:
        val = state.rglSlider[0];
        break;
    case 7:
        val = state.rglSlider[1];
        break;
    default:
        return false;
    }
    const LONG THRESHOLD = 16383; // Half of 32767
    uint8_t mapped = mapAxisValue(val, THRESHOLD);
    if (sign < 0 && mapped == AXIS_NEGATIVE)
        return true;
    if (sign > 0 && mapped == AXIS_POSITIVE)
        return true;
    return false;
}

// ImGuiの状態は描画スレッド専用。入力時計はOSのキー状態を直接読む。
static std::unordered_map<std::string, int> g_keyboardKeys;
static void CacheKeyboardKeys() {
    if (!g_keyboardKeys.empty())
        return;
    auto add = [](ImGuiKey key, int vk) { g_keyboardKeys.emplace(ImGui::GetKeyName(key), vk); };
    for (int i = 0; i < 26; ++i)
        add(static_cast<ImGuiKey>(ImGuiKey_A + i), 'A' + i);
    for (int i = 0; i < 10; ++i) {
        add(static_cast<ImGuiKey>(ImGuiKey_0 + i), '0' + i);
        add(static_cast<ImGuiKey>(ImGuiKey_Keypad0 + i), VK_NUMPAD0 + i);
    }
    for (int i = 0; i < 24; ++i)
        add(static_cast<ImGuiKey>(ImGuiKey_F1 + i), VK_F1 + i);
    add(ImGuiKey_Tab, VK_TAB);
    add(ImGuiKey_LeftArrow, VK_LEFT);
    add(ImGuiKey_RightArrow, VK_RIGHT);
    add(ImGuiKey_UpArrow, VK_UP);
    add(ImGuiKey_DownArrow, VK_DOWN);
    add(ImGuiKey_PageUp, VK_PRIOR);
    add(ImGuiKey_PageDown, VK_NEXT);
    add(ImGuiKey_Home, VK_HOME);
    add(ImGuiKey_End, VK_END);
    add(ImGuiKey_Insert, VK_INSERT);
    add(ImGuiKey_Delete, VK_DELETE);
    add(ImGuiKey_Backspace, VK_BACK);
    add(ImGuiKey_Space, VK_SPACE);
    add(ImGuiKey_Enter, VK_RETURN);
    add(ImGuiKey_Escape, VK_ESCAPE);
    add(ImGuiKey_LeftCtrl, VK_LCONTROL);
    add(ImGuiKey_RightCtrl, VK_RCONTROL);
    add(ImGuiKey_LeftShift, VK_LSHIFT);
    add(ImGuiKey_RightShift, VK_RSHIFT);
    add(ImGuiKey_LeftAlt, VK_LMENU);
    add(ImGuiKey_RightAlt, VK_RMENU);
    add(ImGuiKey_LeftSuper, VK_LWIN);
    add(ImGuiKey_RightSuper, VK_RWIN);
    add(ImGuiKey_Menu, VK_APPS);
    add(ImGuiKey_Apostrophe, VK_OEM_7);
    add(ImGuiKey_Comma, VK_OEM_COMMA);
    add(ImGuiKey_Minus, VK_OEM_MINUS);
    add(ImGuiKey_Period, VK_OEM_PERIOD);
    add(ImGuiKey_Slash, VK_OEM_2);
    add(ImGuiKey_Semicolon, VK_OEM_1);
    add(ImGuiKey_Equal, VK_OEM_PLUS);
    add(ImGuiKey_LeftBracket, VK_OEM_4);
    add(ImGuiKey_Backslash, VK_OEM_5);
    add(ImGuiKey_RightBracket, VK_OEM_6);
    add(ImGuiKey_GraveAccent, VK_OEM_3);
    add(ImGuiKey_CapsLock, VK_CAPITAL);
    add(ImGuiKey_ScrollLock, VK_SCROLL);
    add(ImGuiKey_NumLock, VK_NUMLOCK);
    add(ImGuiKey_PrintScreen, VK_SNAPSHOT);
    add(ImGuiKey_Pause, VK_PAUSE);
    add(ImGuiKey_KeypadDecimal, VK_DECIMAL);
    add(ImGuiKey_KeypadDivide, VK_DIVIDE);
    add(ImGuiKey_KeypadMultiply, VK_MULTIPLY);
    add(ImGuiKey_KeypadSubtract, VK_SUBTRACT);
    add(ImGuiKey_KeypadAdd, VK_ADD);
    add(ImGuiKey_KeypadEnter, VK_RETURN);
}

static cccaster::domain::ui::HudShortcutLatch hudShortcut;
static bool CheckInputBind(int joyId, const std::string &bindStr) {
    if (bindStr.empty())
        return false;

    if (joyId == -2) {
        // 設定ショートカットは非同期キーボード採取からもゲームへ漏らさない。
        if (*CC_GAME_MODE_ADDR == CC_GAME_MODE_CHARA_SELECT &&
            ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000))
            return false;
        const auto key = g_keyboardKeys.find(bindStr);
        if (key != g_keyboardKeys.end() && key->second == VK_F1 &&
            cccaster::domain::ui::FrameBarDisplay::Available(cccaster::domain::session::SceneRunner::AppMode()))
            return false;
        if (key != g_keyboardKeys.end() &&
            ((hudShortcut.f3 && key->second == VK_F3) ||
             (hudShortcut.control && (key->second == VK_CONTROL || key->second == VK_LCONTROL ||
                                  key->second == VK_RCONTROL))))
            return false;
        return GetForegroundWindow() == g_hwnd && key != g_keyboardKeys.end() &&
               (GetAsyncKeyState(key->second) & 0x8000) != 0;
    }

    if (joyId < 0 || joyId >= (int)g_Controllers.size())
        return false;

    try {
        // Binding strings example: "B0", "A0+", "H0_6"
        char type = bindStr[0];
        if (type == 'B') {
            int btn = std::stoi(bindStr.substr(1));
            return IsJoyButtonPressed(g_Controllers[joyId].state, btn);
        } else if (type == 'A') {
            int axis = std::stoi(bindStr.substr(1, bindStr.length() - 2));
            int sign = bindStr.back() == '+' ? 1 : -1;
            return IsJoyAxisPressed(g_Controllers[joyId].state, axis, sign);
        } else if (type == 'H') {
            size_t underscore = bindStr.find('_');
            if (underscore != std::string::npos) {
                int hat = std::stoi(bindStr.substr(1, underscore - 1));
                int dir = std::stoi(bindStr.substr(underscore + 1));
                return IsJoyHatPressed(g_Controllers[joyId].state, hat, dir);
            }
        }
    } catch (...) {
        // 不正なバインド文字列 — クラッシュ防止
    }
    return false;
}

static std::string GetDeviceFileName(int joyId) {
    std::string deviceName = (joyId == -2) ? "Keyboard" : "";
    if (joyId >= 0 && joyId < (int)g_Controllers.size()) {
        deviceName = g_Controllers[joyId].name;
    }
    if (deviceName.empty())
        deviceName = "UnknownDevice_" + std::to_string(joyId);

    std::string sanitizedName = deviceName;
    for (char &c : sanitizedName) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' ||
            c == '|')
            c = '_';
    }
    // 同名2台は個体GUID別ファイルを使う。既存利用者の名前だけの設定は、
    // 個体別ファイルがまだ無い間に限って後方互換として読み込む。
    const std::string legacy = cccaster::core::paths::Resolve(sanitizedName + ".ini");
    if (joyId >= 0 && joyId < static_cast<int>(g_Controllers.size())) {
        const std::string unique = cccaster::core::paths::Resolve(
            sanitizedName + "__" + FormatGuid(g_Controllers[joyId].instanceGuid) + ".ini");
        std::ifstream uniqueFile(unique);
        if (uniqueFile.good())
            return unique;
        std::ifstream legacyFile(legacy);
        if (!legacyFile.good())
            return unique;
    }
    // 相対パスは書き手と読み手が別の場所を指しうるため、DLL基準の絶対パス。
    return legacy;
}

static std::string s_p1Device, s_p2Device;
static std::string s_p1DeviceGuid, s_p2DeviceGuid;
static int s_p1Resolved = -1, s_p2Resolved = -1;
static cccaster::main_app::Config s_p1Config;
static cccaster::main_app::Config s_p2Config;

static int GetJoyIdFromDeviceName(const std::string &sanitizedDeviceName, const std::string &instanceGuid = "");

static void LoadDeviceConfig(Config &cfg, int joyId) {
    cfg.Load(GetDeviceFileName(joyId));
    const auto defaults = cccaster::input::DefaultBindings(joyId == -2);
    for (int i = 0; i < cccaster::input::BindingCount; ++i) {
        const auto key = cccaster::input::BindingKeys[i];
        cfg.SetString("Mapping", key, cfg.GetString("Mapping", key, defaults[i]));
    }
    // 未保存の機器でも既定値を使うが、読込みによるINI作成はしない。
}

void DirectInputHook::ReloadConfigs() {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    CacheKeyboardKeys();
    s_p1Device = cccaster::main_app::ConfigManager::GetString("Settings", "P1Device", "");
    s_p1DeviceGuid = cccaster::main_app::ConfigManager::GetString("Settings", "P1DeviceGuid", "");
    int p1Idx = GetJoyIdFromDeviceName(s_p1Device, s_p1DeviceGuid);
    s_p1Resolved = p1Idx;
    s_p1Config.Clear();
    if (p1Idx != -1)
        LoadDeviceConfig(s_p1Config, p1Idx);

    s_p2Device = cccaster::main_app::ConfigManager::GetString("Settings", "P2Device", "");
    s_p2DeviceGuid = cccaster::main_app::ConfigManager::GetString("Settings", "P2DeviceGuid", "");
    int p2Idx = GetJoyIdFromDeviceName(s_p2Device, s_p2DeviceGuid);
    s_p2Resolved = p2Idx;
    s_p2Config.Clear();
    if (p2Idx != -1)
        LoadDeviceConfig(s_p2Config, p2Idx);
    if (cccaster::diagnostics::input::Enabled()) {
        using cccaster::domain::session::DebugLog;
        DebugLog("[InputConfig] P1 device=%s resolved=%d guid=%s", s_p1Device.c_str(), p1Idx, s_p1DeviceGuid.c_str());
        DebugLog("[InputConfig] P2 device=%s resolved=%d guid=%s", s_p2Device.c_str(), p2Idx, s_p2DeviceGuid.c_str());
        for (int p = 0; p < 2; ++p) {
            const auto &config = p == 0 ? s_p1Config : s_p2Config;
            for (const auto *key : cccaster::input::BindingKeys)
                DebugLog("[InputConfig] P%d %s=%s", p + 1, key, config.GetString("Mapping", key, "").c_str());
        }
    }
}

static uint32_t BuildPlayerInput(int joyId, const cccaster::main_app::Config &deviceConfig) {
    if (joyId == -1)
        return 0; // デバイス未接続 → ニュートラル
    // g_inputMutex 内。各バインドではなくキーボード採取ごとに1回確認する。
    if (joyId == -2)
        hudShortcut.Update((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0,
                           (GetAsyncKeyState(VK_F3) & 0x8000) != 0,
                           (GetAsyncKeyState(VK_MENU) & 0x8000) != 0,
                           GetForegroundWindow() == g_hwnd);
    uint16_t buttons = 0;

    // Evaluate buttons
    if (CheckInputBind(joyId, deviceConfig.GetString("Mapping", "A", "B0")))
        buttons |= (CC_BUTTON_A | CC_BUTTON_CONFIRM);
    if (CheckInputBind(joyId, deviceConfig.GetString("Mapping", "B", "B1")))
        buttons |= (CC_BUTTON_B | CC_BUTTON_CANCEL);
    if (CheckInputBind(joyId, deviceConfig.GetString("Mapping", "C", "B2")))
        buttons |= CC_BUTTON_C;
    if (CheckInputBind(joyId, deviceConfig.GetString("Mapping", "D", "B3")))
        buttons |= CC_BUTTON_D;
    if (CheckInputBind(joyId, deviceConfig.GetString("Mapping", "E", "B4")))
        buttons |= CC_BUTTON_E;
    if (CheckInputBind(joyId, deviceConfig.GetString("Mapping", "Start", "B7")))
        buttons |= CC_BUTTON_START;
    if (CheckInputBind(joyId, deviceConfig.GetString("Mapping", "FN1", "B8")))
        buttons |= CC_BUTTON_FN1;
    if (CheckInputBind(joyId, deviceConfig.GetString("Mapping", "FN2", "B9")))
        buttons |= CC_BUTTON_FN2;
    if (CheckInputBind(joyId, deviceConfig.GetString("Mapping", "A+B", "")))
        buttons |= CC_BUTTON_AB;

    // Evaluate directions
    bool up = CheckInputBind(joyId, deviceConfig.GetString("Mapping", "Up", "H0_8")) ||
              CheckInputBind(joyId, deviceConfig.GetString("Mapping", "Up_Alt", "A1+"));
    bool down = CheckInputBind(joyId, deviceConfig.GetString("Mapping", "Down", "H0_2")) ||
                CheckInputBind(joyId, deviceConfig.GetString("Mapping", "Down_Alt", "A1-"));
    bool left = CheckInputBind(joyId, deviceConfig.GetString("Mapping", "Left", "H0_4")) ||
                CheckInputBind(joyId, deviceConfig.GetString("Mapping", "Left_Alt", "A0-"));
    bool right = CheckInputBind(joyId, deviceConfig.GetString("Mapping", "Right", "H0_6")) ||
                 CheckInputBind(joyId, deviceConfig.GetString("Mapping", "Right_Alt", "A0+"));

    // Pre-clean SOCD
    if (up && down) {
        up = false;
        down = false;
    }
    if (left && right) {
        left = false;
        right = false;
    }

    uint16_t direction = 5; // Neutral
    if (up && left)
        direction = 7;
    else if (up && right)
        direction = 9;
    else if (down && left)
        direction = 1;
    else if (down && right)
        direction = 3;
    else if (up)
        direction = 8;
    else if (down)
        direction = 2;
    else if (left)
        direction = 4;
    else if (right)
        direction = 6;
    else
        direction =
            0; // 0 is neutral in MBAA memory format normally when writing to state? Note: DllProcessManager says "if dir == 5 -> dir = 0"

    if (direction == 5)
        direction = 0;

    return ((uint32_t)direction << 16) | buttons;
}

static int GetJoyIdFromDeviceName(const std::string &sanitizedDeviceName, const std::string &instanceGuid) {
    if (sanitizedDeviceName.empty())
        return -1;
    if (sanitizedDeviceName == "Keyboard")
        return -2;

    if (!instanceGuid.empty()) {
        for (size_t i = 0; i < g_Controllers.size(); ++i)
            if (g_Controllers[i].guidText == instanceGuid)
                return static_cast<int>(i);
        return -1; // 同型別個体への乗り換えは設定画面で明示的に行う。
    }

    int matched = -1;
    int matchCount = 0;
    for (size_t i = 0; i < g_Controllers.size(); ++i) {
        std::string rawName = g_Controllers[i].name;
        for (char &c : rawName) {
            if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' ||
                c == '>' || c == '|')
                c = '_';
        }
        if (rawName == sanitizedDeviceName) {
            matched = static_cast<int>(i);
            ++matchCount;
        }
    }
    return matchCount == 1 ? matched : -1;
}

uint32_t DirectInputHook::GetPlayer1Input() {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    if (cccaster::testing::IsVirtualControllerTest())
        return GetLocalPlayerInput(true, false); // オフライン試験も同じ実DirectInput経路を使う。
    if (g_testModeEnabled)
        return g_testInputP1;
    const auto value = BuildPlayerInput(s_p1Resolved, s_p1Config);
    if (cccaster::diagnostics::input::Enabled()) {
        static cccaster::diagnostics::input::Sampler samples;
        const auto foreground = GetForegroundWindow();
        if (samples.Record(value, uint32_t(s_p1Resolved), foreground == g_hwnd))
            cccaster::domain::session::DebugLog(
                "[InputSource] P1 resolved=%d mapped=%08X foreground=%p gameWindow=%p initialized=%u",
                s_p1Resolved, value, static_cast<void *>(foreground), static_cast<void *>(g_hwnd), unsigned(g_initialized));
    }
    return value;
}

uint32_t DirectInputHook::GetPlayer2Input() {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    if (g_testModeEnabled)
        return g_testInputP2;
    return BuildPlayerInput(s_p2Resolved, s_p2Config);
}

bool DirectInputHook::IsBindingPressed(int joyId, const std::string &bind) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    return CheckInputBind(joyId, bind);
}
uint8_t DirectInputHook::GetTrainingControls() {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    uint8_t result = 0;
    for (int p = 0; p < 2; ++p) {
        const auto &cfg = p == 0 ? s_p1Config : s_p2Config;
        const auto id = p == 0 ? s_p1Resolved : s_p2Resolved;
        if (CheckInputBind(id, cfg.GetString("Mapping", "TrainingSave", ""))) result |= 1;
        if (CheckInputBind(id, cfg.GetString("Mapping", "TrainingLoad", ""))) result |= 2;
    }
    return result;
}

// ============================================================================
// GetLocalPlayerInput — この機体で操作している人の入力を取る
// ============================================================================
//
// 【なぜ P1/P2 を直接呼ばないか】
//   `P1Device` / `P2Device` の意味が UI と読み手で食い違っていた。
//     UI（Controller_Ui_*）  … 画面左が P1、右が P2 という**ローカルの座席**
//     読み手（SceneRunner）  … isHost ? P1 : P2 ＝ **ホスト機/クライアント機**
//   クライアント機の人は自分を「1P」と認識して左（P1）に割り当てるが、
//   読み手は P2Device を見るため永久に無反応になる。実際に
//   `_TEST_MBAACC` の ini は両機とも「読むほうのキーが空」になっていた。
//
// 【解決】
//   ネットプレイでは**この機体のローカルプレイヤーは1人しかいない**。
//   ならば「どちらのスロットに入っていても、それがローカルデバイス」で正しい。
//   本来のスロットが空のときだけ、もう一方にフォールバックする。
//   オフライン（Training 等）は2人がローカルなので、この読み替えはしない。
//
//   UI 側の意味づけを変える案もあるが、既存の ini を壊すうえ、
//   「どちらに割り当てても動く」ほうが利用者にとって事故が起きない。
//
uint32_t DirectInputHook::GetLocalPlayerInput(bool isHost, bool soloLocal) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    if (cccaster::testing::IsVirtualControllerTest()) {
        // 設定ファイルを変更せず、指定した製品が1台だけのときに限り読む。
        // 未接続・重複時は他のコントローラへフォールバックしない。
        const auto product = cccaster::testing::VirtualControllerProduct();
        int selected = -1, count = 0;
        for (size_t i = 0; i < g_Controllers.size(); ++i)
            if (g_Controllers[i].product == product) { selected = int(i); ++count; }
        static Config mapping;
        static const bool configured = [] {
            const char *keys[] = {"Up", "Down", "Left", "Right", "Up_Alt", "Down_Alt", "Left_Alt", "Right_Alt", "A", "B", "C", "D", "E", "Start"};
            const char *binds[] = {"H0_8", "H0_2", "H0_4", "H0_6", "A1+", "A1-", "A0-", "A0+", "B0", "B1", "B2", "B3", "B4", "B9"};
            for (size_t i = 0; i < sizeof(keys)/sizeof(*keys); ++i)
                mapping.SetString("Mapping", keys[i], binds[i]);
            return true;
        }();
        (void)configured;
        const auto value = count == 1 ? BuildPlayerInput(selected, mapping) : 0u;
        static uint32_t last = ~0u;
        static int lastCount = -1;
        if (value != last || count != lastCount) {
            cccaster::domain::session::DebugLog("[VirtualPad] product=%08X matches=%d joy=%d value=%u", product, count, selected, value);
            last = value; lastCount = count;
        }
        return value;
    }
    if (g_testModeEnabled)
        return isHost ? g_testInputP1 : g_testInputP2;

    const char *primaryKey = isHost ? "P1Device" : "P2Device";
    const char *otherKey = isHost ? "P2Device" : "P1Device";

    std::string devName = isHost ? s_p1Device : s_p2Device;
    std::string devGuid = isHost ? s_p1DeviceGuid : s_p2DeviceGuid;
    const cccaster::main_app::Config *cfg = isHost ? &s_p1Config : &s_p2Config;

    if (devName.empty() && soloLocal) {
        const auto &alt = isHost ? s_p2Device : s_p1Device;
        if (!alt.empty()) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                cccaster::domain::session::DebugLog("[DirectInputHook] %s が空のため %s のデバイス '%s' を"
                                                    "ローカル入力として使う（ネットプレイのローカルは1人）",
                                                    primaryKey, otherKey, alt.c_str());
            }
            devName = alt;
            devGuid = isHost ? s_p2DeviceGuid : s_p1DeviceGuid;
            cfg = isHost ? &s_p2Config : &s_p1Config;
        }
    }

    const int joyId = GetJoyIdFromDeviceName(devName, devGuid);

    // 解決に失敗した状態は「入力が一切効かない」と等価だが、これまで
    // ログにも UI にも出ていなかった。同じ状態が続く間は1回だけ出す。
    static int s_lastReportedJoyId = -12345;
    if (joyId == -1 && s_lastReportedJoyId != joyId) {
        cccaster::domain::session::DebugLog("[DirectInputHook] ローカルデバイスを解決できない（%s='%s'）。"
                                            "入力は常にニュートラルになる。接続デバイス数=%d",
                                            primaryKey, devName.c_str(),
                                            static_cast<int>(g_Controllers.size()));
    }
    s_lastReportedJoyId = joyId;

    return BuildPlayerInput(joyId, *cfg);
}

std::vector<JoyDeviceInfo> DirectInputHook::GetConnectedDevices() {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    std::vector<JoyDeviceInfo> list;
    for (const auto &ctrl : g_Controllers) {
        JoyDeviceInfo info;
        info.id = ctrl.id;
        strncpy(info.name, ctrl.name, sizeof(info.name));
        strncpy(info.instanceGuid, ctrl.guidText.c_str(), sizeof(info.instanceGuid));
        info.instanceGuid[sizeof(info.instanceGuid) - 1] = '\0';
        list.push_back(info);
    }
    return list;
}

int DirectInputHook::GetActiveDeviceDirection(int &outJoyId) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    // Check all controllers for a left or right hat/axis press
    for (const auto &ctrl : g_Controllers) {
        // Only trigger on newly pressed to avoid rapid firing
        bool edgeLeft = !IsJoyHatPressed(ctrl.prevState, 0, 4) && IsJoyHatPressed(ctrl.uiState, 0, 4);
        bool edgeRight = !IsJoyHatPressed(ctrl.prevState, 0, 6) && IsJoyHatPressed(ctrl.uiState, 0, 6);
        bool edgeAxisLeft = !IsJoyAxisPressed(ctrl.prevState, 0, -1) && IsJoyAxisPressed(ctrl.uiState, 0, -1);
        bool edgeAxisRight = !IsJoyAxisPressed(ctrl.prevState, 0, 1) && IsJoyAxisPressed(ctrl.uiState, 0, 1);

        if (edgeLeft || edgeAxisLeft) {
            outJoyId = ctrl.id;
            return -1;
        }
        if (edgeRight || edgeAxisRight) {
            outJoyId = ctrl.id;
            return 1;
        }
    }
    return 0;
}

std::string DirectInputHook::GetAnyInputEdge(int joyId) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    auto it = std::find_if(g_Controllers.begin(), g_Controllers.end(),
                           [&](const ControllerData &c) { return c.id == joyId; });
    if (it == g_Controllers.end())
        return "";

    const auto &ctrl = *it;

    // Check Buttons (0 to 127)
    for (int i = 0; i < 128; ++i) {
        if ((ctrl.uiState.rgbButtons[i] & 0x80) && !(ctrl.prevState.rgbButtons[i] & 0x80)) {
            return "B" + std::to_string(i);
        }
    }

    // Check POV Hats (0 to 3)
    for (int i = 0; i < 4; ++i) {
        uint8_t curMap = mapHatValue(ctrl.uiState.rgdwPOV[i]);
        uint8_t prevMap = mapHatValue(ctrl.prevState.rgdwPOV[i]);
        if (curMap != 5 && prevMap == 5) { // Edge from neutral
            // For binding, we snap to nearest cardinal
            if (curMap == 7 || curMap == 8 || curMap == 9)
                return "H" + std::to_string(i) + "_8"; // Up
            if (curMap == 3 || curMap == 2 || curMap == 1)
                return "H" + std::to_string(i) + "_2"; // Down
            if (curMap == 1 || curMap == 4 || curMap == 7)
                return "H" + std::to_string(i) + "_4"; // Left
            if (curMap == 9 || curMap == 6 || curMap == 3)
                return "H" + std::to_string(i) + "_6"; // Right
        }
    }

    // Check Axes (0 to 7)
    const long DEADZONE = 16383;
    auto checkAxisEdge = [&](long current, long prev, int axisId, bool invert) -> std::string {
        long c = invert ? -current : current;
        long p = invert ? -prev : prev;
        uint8_t curMap = mapAxisValue(c, DEADZONE);
        uint8_t prevMap = mapAxisValue(p, DEADZONE);
        if (curMap == AXIS_POSITIVE && prevMap != AXIS_POSITIVE)
            return "A" + std::to_string(axisId) + "+";
        if (curMap == AXIS_NEGATIVE && prevMap != AXIS_NEGATIVE)
            return "A" + std::to_string(axisId) + "-";
        return "";
    };

    std::string res;
    if (!(res = checkAxisEdge(ctrl.uiState.lX, ctrl.prevState.lX, 0, false)).empty())
        return res;
    if (!(res = checkAxisEdge(ctrl.uiState.lY, ctrl.prevState.lY, 1, true)).empty())
        return res;
    if (!(res = checkAxisEdge(ctrl.uiState.lZ, ctrl.prevState.lZ, 2, false)).empty())
        return res;
    if (!(res = checkAxisEdge(ctrl.uiState.lRx, ctrl.prevState.lRx, 3, false)).empty())
        return res;
    if (!(res = checkAxisEdge(ctrl.uiState.lRy, ctrl.prevState.lRy, 4, true)).empty())
        return res;
    if (!(res = checkAxisEdge(ctrl.uiState.lRz, ctrl.prevState.lRz, 5, false)).empty())
        return res;
    if (!(res = checkAxisEdge(ctrl.uiState.rglSlider[0], ctrl.prevState.rglSlider[0], 6, false)).empty())
        return res;
    if (!(res = checkAxisEdge(ctrl.uiState.rglSlider[1], ctrl.prevState.rglSlider[1], 7, false)).empty())
        return res;

    return "";
}

void DirectInputHook::StartMappingPlayer1(int inputIndex) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
}

void DirectInputHook::StartMappingPlayer2(int inputIndex) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
}

void DirectInputHook::SetTestModeEnabled(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    g_testModeEnabled = enabled;
}

void DirectInputHook::SetTestInputP1(uint32_t input) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    g_testInputP1 = input;
}

void DirectInputHook::SetTestInputP2(uint32_t input) {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    g_testInputP2 = input;
}

void DirectInputHook::PollUi() {
    std::lock_guard<std::recursive_mutex> lock(g_inputMutex);
    for (auto &ctrl : g_Controllers) {
        ctrl.prevState = ctrl.uiState;
        ctrl.uiState = ctrl.state;
    }
}
