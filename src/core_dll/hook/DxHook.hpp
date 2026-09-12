#pragma once
// ============================================================================
// DxHook — DirectX 9 関数フック管理 (MinHook ベース)
//
// 【責務】 純粋インフラ層
//   D3D9 vtable からフック関数を差し替え、
//   EndScene / Present / Reset のタイミングで外部コールバックを呼ぶ。
//   ImGui初期化、描画判定、ビジネスロジック等は一切持たない。
//
// 【vtable Hook 手法】
//   1. 起動gate付きランチャーではゲームのDirect3DCreate9 IATを捕捉
//   2. CreateDeviceの返す実デバイスのvtableからアドレスを取得
//      - EndScene = vtable[42]
//      - Reset    = vtable[16]
//   3. baseline/旧ランチャー/捕捉失敗時のみダミー生成方式へ戻す
//   4. MinHook で Hooked_EndScene / Hooked_Reset にリダイレクト
//   5. Hooked_EndScene 初回呼出時に Present[17] を動的フック
//
// 【フック関数の役割】
//   Hooked_EndScene : onEndScene コールバック呼出 → 元 EndScene 呼出
//   Hooked_Present  : onPresent コールバック呼出 → 元 Present 呼出(※スキップ可能)
//   Hooked_Reset    : onPreReset → 元 Reset → onPostReset
//
// 【依存関係】
//   - MinHook v1.3.3  : API フック基盤
//   - d3d9.h          : DirectX 9 ヘッダ
//   ドメイン層への依存は一切持たない（コールバック経由で分離）
// ============================================================================

#include <windows.h>
#include <d3d9.h>

namespace cccaster::game_interface {

/// EndScene / Present コールバック型（デバイスポインタを受け取る）
using DeviceCallback = void (*)(LPDIRECT3DDEVICE9);

/// Present スキップ判定コールバック型（true = 元Presentをスキップ）
using PresentSkipCallback = bool (*)(LPDIRECT3DDEVICE9);

/// Reset コールバック型（デバイスポインタを受け取る）
using ResetCallback = void (*)(LPDIRECT3DDEVICE9);

class DxHook {
  public:
    /// 起動gate付きなら生成入口を設置する。実デバイスフックは生成時に設定する。
    static bool Initialize();

    /// 全フックを解除する。
    static void Shutdown();

    // ── コールバック登録 ──

    /// EndScene フック時に呼ばれるコールバック（ImGui描画等）
    static void SetEndSceneCallback(DeviceCallback cb);

    /// Present直前の表示用コールバック（待機やゲーム進行は行わない）
    static void SetPresentCallback(DeviceCallback cb);
    // 完成画像を提示した後に、次フレームの入力待機・状態保存を行う。
    static void SetAfterPresentCallback(DeviceCallback cb);

    /// Present の元関数をスキップするか判定するコールバック
    static void SetPresentSkipCallback(PresentSkipCallback cb);

    /// Reset 前に呼ばれるコールバック（リソース解放）
    static void SetPreResetCallback(ResetCallback cb);

    /// Reset 後に呼ばれるコールバック（リソース再生成）
    static void SetPostResetCallback(ResetCallback cb);

    // ── フック関数（D3D9 が呼び出す）──

    static HRESULT APIENTRY Hooked_EndScene(LPDIRECT3DDEVICE9 pDevice);
    static HRESULT APIENTRY Hooked_BeginScene(LPDIRECT3DDEVICE9 pDevice);
    static HRESULT APIENTRY Hooked_Present(LPDIRECT3DDEVICE9 pDevice, const RECT *pSourceRect,
                                           const RECT *pDestRect, HWND hDestWindowOverride,
                                           const RGNDATA *pDirtyRegion);
    static HRESULT APIENTRY Hooked_Reset(LPDIRECT3DDEVICE9 pDevice,
                                         D3DPRESENT_PARAMETERS *pPresentationParameters);

  private:
    static bool InstallDeviceHooks(void **table, bool eager = false);
    static bool InstallFactoryImport();
    static bool InitializeDummy();
    static IDirect3D9 *WINAPI Hooked_Direct3DCreate9(UINT sdkVersion);
    static HRESULT WINAPI Hooked_CreateDevice(IDirect3D9 *self, UINT adapter, D3DDEVTYPE type,
                                              HWND window, DWORD flags,
                                              D3DPRESENT_PARAMETERS *parameters,
                                              IDirect3DDevice9 **device);
    static bool GetD3D9Device(void **pTable, size_t size);
    static void *original_EndScene; ///< 元の EndScene 関数ポインタ
    static void *original_BeginScene;
    static void *original_Reset;    ///< 元の Reset 関数ポインタ
    static void *original_Present;  ///< 元の Present 関数ポインタ
    static bool isInitialized;      ///< Initialize() 完了フラグ
    static bool isPresentHooked;    ///< Present 動的フック完了フラグ

    // ── コールバック ──
    static DeviceCallback onEndScene;
    static DeviceCallback onPresent;
    static DeviceCallback onAfterPresent;
    static PresentSkipCallback onPresentSkip;
    static ResetCallback onPreReset;
    static ResetCallback onPostReset;
};

} // namespace cccaster::game_interface
