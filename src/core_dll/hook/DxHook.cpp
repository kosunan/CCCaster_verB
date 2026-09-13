#include "core_dll/timing/UpdateCadence.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/timing/FrameTiming.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/hook/GameReleaseGate.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/hook/DirectInputHook.hpp"
#include "core_dll/mbaa_mem/StartupAssets.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <cstdlib>
#include <cstddef>
#include <cstring>
// ============================================================================
// DxHook.cpp — DirectX 9 関数フック（純粋インフラ層）
//
// 【責務】
//   D3D9 の EndScene / Present / Reset をフックし、
//   外部コールバックを呼び出す。
//   ImGui初期化、描画判定、ビジネスロジック等は一切持たない。
//
// 【処理フロー概要】
//   Initialize()
//     1. MinHook ライブラリ初期化
//     2. 起動gateあり: ゲームIATのDirect3DCreate9→CreateDeviceを捕捉
//     3. 生成された実デバイスにEndScene[42] / Reset[16]を設置
//     4. baseline/旧ランチャー/捕捉失敗時はダミーデバイスへ戻す
//
//   Hooked_EndScene() [毎フレーム呼び出し]
//     1. 初回: Present[17] を動的フック
//     2. onEndScene コールバック呼出
//     3. 元の EndScene を呼び出し
//
//   Hooked_Present() [1F1回]
//     1. onPresent コールバック呼出
//     2. onPresentSkip → true なら元の Present をスキップ
//     3. 元の Present を呼び出し
//
//   Hooked_Reset() [解像度変更/復帰時]
//     1. onPreReset コールバック呼出
//     2. 元の Reset 実行
//     3. onPostReset コールバック呼出
// ============================================================================

#include "core_dll/hook/DxHook.hpp"
#include "core_dll/hook/RenderProbe.hpp"
#include "core_dll/hook/DriverLockProbe.hpp"
#include "core_dll/hook/ScenePairMerge.hpp"
#include "core_dll/timing/SpinProbe.hpp"
#include "core_dll/rollback/ReplayEffects.hpp"
#include "core_dll/mbaa_mem/CombatStress.hpp"
#include "core_dll/mbaa_mem/GaugeStress.hpp"
#include <MinHook.h>

/// デバッグログ出力関数（dllmain.cpp で定義）
void HookLog(const char *msg);

using namespace cccaster::game_interface;

// ─── Static メンバ初期化 ───────────────────────────
void *DxHook::original_EndScene = nullptr;
void *DxHook::original_BeginScene = nullptr;
void *DxHook::original_Reset = nullptr;
void *DxHook::original_Present = nullptr;
bool DxHook::isInitialized = false;
bool DxHook::isPresentHooked = false;

namespace {
using Factory = IDirect3D9 *(WINAPI *)(UINT);
using CreateDevice = HRESULT(WINAPI *)(IDirect3D9 *, UINT, D3DDEVTYPE, HWND, DWORD,
                                      D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **);
Factory originalFactory = nullptr;
CreateDevice originalCreateDevice = nullptr;
DWORD *factorySlot = nullptr;
void *createDeviceTarget = nullptr;
void *endSceneTarget = nullptr;
void *resetTarget = nullptr;
bool probingDummy = false;
bool firstEndSceneReady = false;

// ゲーム入口解放前に設置し、同じslotがまだ自分の入口を指すときだけ復元する。
bool WriteImport(DWORD *slot, DWORD expected, DWORD replacement) {
    DWORD protection = 0;
    if (*slot != expected || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protection))
        return false;
    const auto previous = InterlockedCompareExchange(reinterpret_cast<volatile LONG *>(slot),
                                                     static_cast<LONG>(replacement), static_cast<LONG>(expected));
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(*slot), protection, &ignored);
    return static_cast<DWORD>(previous) == expected;
}
}

// ─── コールバック初期化 ────────────────────────────
DeviceCallback DxHook::onEndScene = nullptr;
DeviceCallback DxHook::onPresent = nullptr;
DeviceCallback DxHook::onAfterPresent = nullptr;
PresentSkipCallback DxHook::onPresentSkip = nullptr;
ResetCallback DxHook::onPreReset = nullptr;
ResetCallback DxHook::onPostReset = nullptr;

// ─── コールバック登録 ──────────────────────────────
void DxHook::SetEndSceneCallback(DeviceCallback cb) {
    onEndScene = cb;
}
void DxHook::SetAfterPresentCallback(DeviceCallback cb) {
    onAfterPresent = cb;
}
void DxHook::SetPresentCallback(DeviceCallback cb) {
    onPresent = cb;
}
void DxHook::SetPresentSkipCallback(PresentSkipCallback cb) {
    onPresentSkip = cb;
}
void DxHook::SetPreResetCallback(ResetCallback cb) {
    onPreReset = cb;
}
void DxHook::SetPostResetCallback(ResetCallback cb) {
    onPostReset = cb;
}

// ─── ダミーウィンドウプロシージャ ──────────────────
static LRESULT CALLBACK DummyWindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

// ============================================================================
// GetD3D9Device — ダミーデバイスを生成して vtable を取得する
// ============================================================================
bool DxHook::GetD3D9Device(void **pTable, size_t size) {
    if (!pTable)
        return false;

    WNDCLASSEX wc = {sizeof(WNDCLASSEX),
                     CS_CLASSDC,
                     DummyWindowProc,
                     0L,
                     0L,
                     GetModuleHandle(NULL),
                     NULL,
                     NULL,
                     NULL,
                     NULL,
                     "DummyClass",
                     NULL};
    RegisterClassEx(&wc);
    HWND dummyWindow =
        CreateWindow("DummyClass", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

    LPDIRECT3D9 pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) {
        DestroyWindow(dummyWindow);
        UnregisterClass("DummyClass", wc.hInstance);
        return false;
    }

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = dummyWindow;

    LPDIRECT3DDEVICE9 pDummyDevice = nullptr;
    HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummyWindow,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &pDummyDevice);
    if (FAILED(hr)) {
        pD3D->Release();
        DestroyWindow(dummyWindow);
        UnregisterClass("DummyClass", wc.hInstance);
        return false;
    }

    void *pVTable = *reinterpret_cast<void **>(pDummyDevice);
    memcpy(pTable, pVTable, size);

    pDummyDevice->Release();
    pD3D->Release();
    DestroyWindow(dummyWindow);
    UnregisterClass("DummyClass", wc.hInstance);

    return true;
}

// ============================================================================
// Initialize — MinHook による D3D9 フック設定
// ============================================================================
bool DxHook::InstallDeviceHooks(void **table, bool eager) {
    if (endSceneTarget || resetTarget)
        return endSceneTarget == table[42] && resetTarget == table[16];
    if (eager) {
        // 実デバイス生成直後は4つともアドレスが確定している。
        // Queueでまとめ、MinHookの全スレッド停止を4回から1回にする。
        void *targets[] = {table[42], table[16], table[41], table[17]};
        void *detours[] = {reinterpret_cast<void *>(&Hooked_EndScene),
                           reinterpret_cast<void *>(&Hooked_Reset),
                           reinterpret_cast<void *>(&Hooked_BeginScene),
                           reinterpret_cast<void *>(&Hooked_Present)};
        void **originals[] = {&original_EndScene, &original_Reset,
                              &original_BeginScene, &original_Present};
        size_t created = 0;
        bool ready = true;
        for (size_t i = 0; i < 4; ++i) {
            if (MH_CreateHook(targets[i], detours[i], originals[i]) != MH_OK) {
                ready = false;
                break;
            }
            ++created;
            if (MH_QueueEnableHook(targets[i]) != MH_OK) {
                ready = false;
                break;
            }
        }
        if (ready && MH_ApplyQueued() == MH_OK) {
            endSceneTarget = targets[0];
            resetTarget = targets[1];
            isPresentHooked = true;
            scene_pair_merge::Initialize();
            HookLog("[DxHook] Real device EndScene/Reset/BeginScene/Present enabled in one batch");
            return true;
        }
        // 他の所有者のALREADY_CREATEDフックには触らない。
        // ApplyQueuedの途中失敗で有効になった分も含め、自分で作れた分だけ戻す。
        for (size_t i = 0; i < created; ++i)
            MH_QueueDisableHook(targets[i]);
        if (created)
            MH_ApplyQueued();
        bool cleanupFailed = false;
        for (size_t i = 0; i < created; ++i) {
            MH_DisableHook(targets[i]);
            const auto removed = MH_RemoveHook(targets[i]);
            if (removed == MH_OK || removed == MH_ERROR_NOT_CREATED)
                *originals[i] = nullptr;
            else
                cleanupFailed = true;
        }
        if (cleanupFailed) {
            // MinHookは保護変更失敗時に有効フックを残す。trampolineを消して
            // dummy再試行すると残ったdetourからNULLを呼ぶため、起動失敗とする。
            HookLog("[DxHook] ERROR: eager hook rollback failed; aborting startup");
            ExitProcess(ERROR_DLL_INIT_FAILED);
        }
        isPresentHooked = false;
        return false;
    }
    void *end = table[42], *reset = table[16];
    // ALREADY_CREATEDを成功扱いすると他の登録のtrampolineを失うため、受け入れない。
    if (MH_CreateHook(end, reinterpret_cast<void *>(&Hooked_EndScene), &original_EndScene) != MH_OK)
        return false;
    if (MH_CreateHook(reset, reinterpret_cast<void *>(&Hooked_Reset), &original_Reset) != MH_OK) {
        MH_RemoveHook(end);
        original_EndScene = nullptr;
        return false;
    }
    if (MH_EnableHook(end) != MH_OK || MH_EnableHook(reset) != MH_OK) {
        MH_DisableHook(end);
        MH_DisableHook(reset);
        MH_RemoveHook(end);
        MH_RemoveHook(reset);
        original_EndScene = original_Reset = nullptr;
        return false;
    }
    endSceneTarget = end;
    resetTarget = reset;
    return true;
}

bool DxHook::InitializeDummy() {
    // CreateDevice捕捉後のfallbackでも再帰して設置しない。
    const bool previous = probingDummy;
    probingDummy = true;
    void *table[119]{};
    const bool obtained = GetD3D9Device(table, sizeof(table));
    probingDummy = previous;
    return obtained && InstallDeviceHooks(table);
}

bool DxHook::InstallFactoryImport() {
    auto base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    if (reinterpret_cast<uintptr_t>(base) != 0x400000)
        return false;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096)
        return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC || nt->OptionalHeader.ImageBase != 0x400000 ||
        nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        return false;
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    auto fits = [imageSize](size_t rva, size_t count) { return rva < imageSize && count <= imageSize - rva; };
    auto named = [&](DWORD rva, const char *name) {
        return fits(rva, std::strlen(name) + 1) && !std::strcmp(reinterpret_cast<char *>(base + rva), name);
    };
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !fits(directory.VirtualAddress, directory.Size))
        return false;
    auto d3d = GetModuleHandleW(L"d3d9.dll");
    auto factory = d3d ? GetProcAddress(d3d, "Direct3DCreate9") : nullptr;
    if (!factory)
        return false;
    for (size_t offset = 0; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= directory.Size;
         offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress + offset);
        if (!descriptor->Name)
            break;
        if (!fits(descriptor->Name, 9) ||
            _strnicmp(reinterpret_cast<char *>(base + descriptor->Name), "d3d9.dll", 9) ||
            !descriptor->OriginalFirstThunk || !descriptor->FirstThunk)
            continue;
        for (size_t i = 0;; ++i) {
            const size_t nameRva = descriptor->OriginalFirstThunk + i * sizeof(IMAGE_THUNK_DATA32);
            const size_t slotRva = descriptor->FirstThunk + i * sizeof(IMAGE_THUNK_DATA32);
            if (!fits(nameRva, 4) || !fits(slotRva, 4))
                return false;
            const DWORD name = *reinterpret_cast<DWORD *>(base + nameRva);
            if (!name)
                break;
            if (IMAGE_SNAP_BY_ORDINAL32(name) || name > imageSize ||
                !named(name + offsetof(IMAGE_IMPORT_BY_NAME, Name), "Direct3DCreate9"))
                continue;
            auto slot = reinterpret_cast<DWORD *>(base + slotRva);
            originalFactory = reinterpret_cast<Factory>(factory);
            if (!WriteImport(slot, reinterpret_cast<DWORD>(factory),
                             reinterpret_cast<DWORD>(&Hooked_Direct3DCreate9))) {
                originalFactory = nullptr;
                return false;
            }
            factorySlot = slot;
            HookLog("[DxHook] Game Direct3DCreate9 IAT installed (no dummy device)");
            return true;
        }
    }
    return false;
}

IDirect3D9 *WINAPI DxHook::Hooked_Direct3DCreate9(UINT sdkVersion) {
    HookLog("[DxHook] Direct3DCreate9 begin");
    cccaster::diagnostics::startup::Mark("d3d_factory_begin");
    auto d3d = originalFactory(sdkVersion);
    cccaster::domain::session::DebugLog("[DxHook] Direct3DCreate9 returned object=%p", static_cast<void *>(d3d));
    if (d3d && !originalCreateDevice) {
        HookLog("[DxHook] Installing CreateDevice hook...");
        auto target = (*reinterpret_cast<void ***>(d3d))[16];
        void *trampoline = nullptr;
        if (MH_CreateHook(target, reinterpret_cast<void *>(&Hooked_CreateDevice), &trampoline) == MH_OK) {
            originalCreateDevice = reinterpret_cast<CreateDevice>(trampoline);
            if (MH_EnableHook(target) == MH_OK) {
                createDeviceTarget = target;
            } else {
                MH_RemoveHook(target);
                originalCreateDevice = nullptr;
            }
        }
        if (!originalCreateDevice) {
            HookLog("[DxHook] CreateDevice interception failed; using dummy fallback");
            if (!InitializeDummy())
                HookLog("[DxHook] ERROR: dummy fallback failed");
        }
    }
    HookLog("[DxHook] Direct3DCreate9 interception complete");
    cccaster::diagnostics::startup::Mark("d3d_factory_end");
    return d3d;
}

HRESULT WINAPI DxHook::Hooked_CreateDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type,
                                          HWND window, DWORD flags, D3DPRESENT_PARAMETERS *parameters,
                                          IDirect3DDevice9 **device) {
    if (probingDummy)
        return originalCreateDevice(self, adapter, type, window, flags, parameters, device);
    cccaster::diagnostics::startup::Mark("game_device_begin");
    if(parameters) {
        cccaster::domain::session::DebugLog("[D3DPresentConfig] interval=%u windowed=%d swap=%u buffers=%u",unsigned(parameters->PresentationInterval),int(parameters->Windowed),unsigned(parameters->SwapEffect),unsigned(parameters->BackBufferCount));
    }
    cccaster::domain::session::DebugLog("[D3DDeviceFlags] flags=%u", unsigned(flags));
    const auto result = originalCreateDevice(self, adapter, type, window, flags, parameters, device);
    cccaster::domain::session::DebugLog("[DxHook] CreateDevice returned HRESULT=0x%08lX", static_cast<unsigned long>(result));
    cccaster::diagnostics::startup::Mark("game_device_created");
    if (SUCCEEDED(result) && device && *device) {
        cccaster::diagnostics::driver_lock::Install();
        if (!InstallDeviceHooks(*reinterpret_cast<void ***>(*device), true)) {
            HookLog("[DxHook] Real device hooks failed; using dummy fallback");
            if (!InitializeDummy())
                HookLog("[DxHook] ERROR: dummy fallback failed");
        } else {
            HookLog("[DxHook] Real device EndScene/Reset hooks installed");
        }
        if (cccaster::game_memory::startup_assets::Active() &&
            !std::getenv("CCCASTER_STARTUP_FIRST_BASELINE"))
            DirectInputHook::BeginInitialize();
    }
    cccaster::diagnostics::startup::Mark("game_device_end");
    return result;
}

bool DxHook::Initialize() {
    if (isInitialized)
        return true;
    const auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (!cccaster::diagnostics::startup::Baseline() && cccaster::diagnostics::startup::HasGate()) {
        if (InstallFactoryImport()) {
            isInitialized = true;
            return true;
        }
        HookLog("[DxHook] Direct3DCreate9 interception unavailable; using dummy fallback");
    } else {
        HookLog("[DxHook] Baseline or legacy launcher: using dummy device");
    }
    isInitialized = InitializeDummy();
    return isInitialized;
}

// ============================================================================
// Shutdown — 全フック解除
// ============================================================================
void DxHook::Shutdown() {
    if (!isInitialized)
        return;
    game_release_gate::Remove();

    // コールバックをクリア（ダングリングポインタ防止）
    onEndScene = nullptr;
    onPresent = nullptr;
    onAfterPresent = nullptr;
    onPresentSkip = nullptr;
    onPreReset = nullptr;
    onPostReset = nullptr;

    if (factorySlot) {
        if (!WriteImport(factorySlot, reinterpret_cast<DWORD>(&Hooked_Direct3DCreate9),
                         reinterpret_cast<DWORD>(originalFactory)))
            HookLog("[DxHook] Direct3DCreate9 IAT restore skipped/failed");
        factorySlot = nullptr;
    }

    // MinHook 全フック解除 + ライブラリ終了
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    isInitialized = false;
    isPresentHooked = false;
    originalFactory = nullptr;
    originalCreateDevice = nullptr;
    createDeviceTarget = endSceneTarget = resetTarget = nullptr;
    original_EndScene = original_BeginScene = original_Reset = original_Present = nullptr;
    firstEndSceneReady = false;
}

// ============================================================================
// Hooked_EndScene — EndScene フック
// ============================================================================
//   1. 初回: Present を動的フック（ゲーム実デバイスの vtable から取得）
//   2. onEndScene コールバック呼出
//   3. 元の EndScene を呼び出し
HRESULT APIENTRY DxHook::Hooked_EndScene(LPDIRECT3DDEVICE9 pDevice) {
    if (!original_BeginScene) {
        auto table = *reinterpret_cast<void ***>(pDevice);
        const auto status = MH_CreateHook(table[41], reinterpret_cast<void *>(&Hooked_BeginScene), &original_BeginScene);
        if (status == MH_OK && MH_EnableHook(table[41]) == MH_OK) {
            scene_pair_merge::Initialize();
            HookLog("[DxHook] BeginScene hook enabled");
        } else {
            HookLog("[DxHook] BeginScene hook unavailable; scene merging disabled");
        }
    }
    render_probe::Install(pDevice);
    // ── Present 動的フック（初回のみ）──
    if (!isPresentHooked) {
        void **gameDeviceVtable = *reinterpret_cast<void ***>(pDevice);
        void *gamePresentAddr = gameDeviceVtable[17];

        if (gamePresentAddr) {
            if (MH_CreateHook(gamePresentAddr, (void *)&Hooked_Present, (void **)&original_Present) ==
                MH_OK) {
                if (MH_EnableHook(gamePresentAddr) == MH_OK) {
                    HookLog("[DxHook] Dynamically hooked Present successfully.");
                    isPresentHooked = true;
                } else {
                    HookLog("[DxHook] Failed to enable Present hook.");
                }
            } else {
                HookLog("[DxHook] Failed to CreateHook on Present.");
            }
        }
    }

    // ── コールバック呼出 ──
    if (onEndScene) {
        const auto previous = render_probe::ignored;
        render_probe::ignored = true;
        onEndScene(pDevice);
        render_probe::ignored = previous;
    }
    firstEndSceneReady = true;

    // ── 元の EndScene を呼び出し ──
    typedef HRESULT(APIENTRY * EndScene_t)(LPDIRECT3DDEVICE9);
    EndScene_t pOrigEndScene = (EndScene_t)original_EndScene;
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (!render_probe::ignored && scene_pair_merge::DeferEnd(pDevice, caller))
        return D3D_OK;
    scene_pair_merge::Reset();
    if (!render_probe::Enabled() || render_probe::ignored)
        return pOrigEndScene(pDevice);
    const auto started = cccaster::platform::RealMonotonicTicks();
    const auto result = pOrigEndScene(pDevice);
    render_probe::Record(42, started, result);
    render_probe::EndPass(pDevice, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    return result;
}

HRESULT APIENTRY DxHook::Hooked_BeginScene(LPDIRECT3DDEVICE9 pDevice) {
    using Begin = HRESULT(APIENTRY *)(LPDIRECT3DDEVICE9);
    const auto caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (!render_probe::ignored && scene_pair_merge::ConsumeBegin(pDevice, caller))
        return D3D_OK;
    // 万一照合済みの連続呼出しから外れたら、保留したEndを実行して通常経路に戻す。
    if (scene_pair_merge::pending) {
        reinterpret_cast<Begin>(original_EndScene)(scene_pair_merge::pending);
        scene_pair_merge::Reset();
    }
    const bool trace = render_probe::Enabled() && !render_probe::ignored;
    const auto started = trace ? cccaster::platform::RealMonotonicTicks() : 0;
    const auto result = reinterpret_cast<Begin>(original_BeginScene)(pDevice);
    scene_pair_merge::active = result == D3D_OK ? pDevice : nullptr;
    if (trace) {
        render_probe::beginCaller = caller;
        render_probe::Record(41, started, result);
    }
    return result;
}

// ============================================================================
// Hooked_Present — Present フック（1F1回）
// ============================================================================
//   1. onPresentでUIを合成
//   2. onPresentSkip → true なら元 Present をスキップ
//   3. 元のPresentで表示後、onAfterPresentで次フレームの入力待機・状態保存
HRESULT APIENTRY DxHook::Hooked_Present(LPDIRECT3DDEVICE9 pDevice, const RECT *pSourceRect,
                                        const RECT *pDestRect, HWND hDestWindowOverride,
                                        const RGNDATA *pDirtyRegion) {
    // ゲームスレッドで一度だけ、現在より後に実行されるCS解放命令を設置する。
    static const bool gateInitialized = [] { game_release_gate::Install(); return true; }();
    (void)gateInitialized;
    // eagerで入口だけ早く設置しても、旧方式と同じ最初のEndSceneまでは
    // ゲーム進行・描画省略・入力準備を始めない。
    if (!firstEndSceneReady) {
        using Present = HRESULT(APIENTRY *)(LPDIRECT3DDEVICE9, const RECT *, const RECT *, HWND,
                                            const RGNDATA *);
        return reinterpret_cast<Present>(original_Present)(pDevice, pSourceRect, pDestRect,
                                                            hDestWindowOverride, pDirtyRegion);
    }
    cccaster::testing::gauge_stress::Flush();
    cccaster::diagnostics::driver_lock::Flush(cccaster::core::netplay::NetplaySession::GetState().appliedFrame.load());
    if (render_probe::Enabled()) {
        cccaster::domain::session::DebugLog("[SceneMergeFrame] seq=%u pairs=%u", render_probe::serial, scene_pair_merge::merged);
        scene_pair_merge::merged = 0;
    }
    render_probe::FinishFrame();
    cccaster::sync::FlushSoundProbe();
    cccaster::testing::combat_stress::Flush();
    // 比較試験時だけ旧順序を再現。通常は完成画像を先に提示する。
    static const bool legacy = [] {
        const char *v = std::getenv("CCCASTER_TEST_PRESENT_ORDER");
        return v && v[0] == '1';
    }();
    static const bool trace = std::getenv("CCCASTER_INPUT_LATENCY_TRACE") != nullptr;
    const auto entered = trace ? cccaster::platform::RealMonotonicUs() : 0;
    if (legacy && onAfterPresent)
        onAfterPresent(pDevice);
    if (onPresent) {
        render_probe::ignored = true;
        onPresent(pDevice);
        render_probe::ignored = false;
    }
    const bool skipped = onPresentSkip && onPresentSkip(pDevice);
    HRESULT result = D3D_OK;
    if (!skipped) {
        if (trace) {
            const auto withheld = cccaster::platform::RealMonotonicUs() - entered;
            cccaster::domain::session::DebugLog("[PresentLatency] legacy=%d beforePresentUs=%lld", legacy,
                                                withheld);
        }
        using Present_t =
            HRESULT(APIENTRY *)(LPDIRECT3DDEVICE9, const RECT *, const RECT *, HWND, const RGNDATA *);
        result = reinterpret_cast<Present_t>(original_Present)(pDevice, pSourceRect, pDestRect,
                                                               hDestWindowOverride, pDirtyRegion);
        if (!cccaster::diagnostics::startup::presentRecorded && SUCCEEDED(result) &&
            cccaster::game_interface::GameMem().GameMode() == CC_GAME_MODE_CHARA_SELECT) {
            cccaster::diagnostics::startup::presentRecorded = true;
            cccaster::diagnostics::startup::Mark("chara_present");
        }
    }
    // スキップ時も次フレームの準備を行う。省くとロールアップが進まない。
    if (!legacy && onAfterPresent)
        onAfterPresent(pDevice);
    if (cccaster::diagnostics::SpinProbe::pending)
        cccaster::diagnostics::SpinProbe::sample.gameReturn = cccaster::diagnostics::SpinProbe::Now();
    if (cccaster::core::timer::FrameTiming::releaseDueTicks) {
        if (!game_release_gate::installed ||
            reinterpret_cast<uintptr_t>(__builtin_return_address(0)) != game_release_gate::PresentReturn)
            game_release_gate::Release();
        return result;
    }
    auto &cadence = cccaster::diagnostics::UpdateCadence::Get();
    if (cadence.Armed()) cadence.Capture(cccaster::platform::RealMonotonicTicks());
    return result;
}

// ============================================================================
// Hooked_Reset — Reset フック（解像度変更/Alt+Tab復帰時）
// ============================================================================
HRESULT APIENTRY DxHook::Hooked_Reset(LPDIRECT3DDEVICE9 pDevice,
                                      D3DPRESENT_PARAMETERS *pPresentationParameters) {
    scene_pair_merge::Reset();
    // Reset 前コールバック（リソース解放）
    if (onPreReset) {
        onPreReset(pDevice);
    }

    // 元の Reset 実行
    typedef HRESULT(APIENTRY * Reset_t)(LPDIRECT3DDEVICE9, D3DPRESENT_PARAMETERS *);
    Reset_t pOrigReset = (Reset_t)original_Reset;
    HRESULT hr = pOrigReset(pDevice, pPresentationParameters);

    // Reset 後コールバック（リソース再生成）
    if (onPostReset) {
        onPostReset(pDevice);
    }

    return hr;
}
