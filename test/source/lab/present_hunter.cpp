#include <windows.h>
#include <d3d9.h>
#include "MinHook.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "winmm.lib")

void AddLog(const char* msg) {
    FILE* f = fopen("present_hunter_log.txt", "a");
    if (f) {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

// ---------------------------------------------------------
// シンプルなCRC32アルゴリズム（ソフトウェアベースで高速）
// ---------------------------------------------------------
uint32_t CalculateCRC32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
        }
    }
    return ~crc;
}

// ---------------------------------------------------------
// フック用の型定義と状態変数
// ---------------------------------------------------------
typedef HRESULT(APIENTRY* Present_t)(LPDIRECT3DDEVICE9, const RECT*, const RECT*, HWND, const RGNDATA*);
Present_t pOrigPresent = nullptr;

typedef HRESULT(APIENTRY* EndScene_t)(LPDIRECT3DDEVICE9);
EndScene_t pOrigEndScene = nullptr;

bool isPresentHooked = false;

// CRC追跡用の状態変数
uint32_t g_currentFrameCount = 0;
uint32_t g_endSceneCallCountInFrame = 0;
uint32_t g_lastCrc = 0;


// =========================================================
// Hooked D3D9 Present (フレームの終端)
// =========================================================
HRESULT APIENTRY Hooked_Present(LPDIRECT3DDEVICE9 pDevice, const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) {
    g_currentFrameCount++;
    g_endSceneCallCountInFrame = 0; // フレーム区切りでカウンタをリセット
    return pOrigPresent(pDevice, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

// =========================================================
// Hooked D3D9 EndScene (ハッシュ計算の心臓部)
// =========================================================
HRESULT APIENTRY Hooked_EndScene(LPDIRECT3DDEVICE9 pDevice) {
    // 1. Presentフックの遅延初期化
    if (!isPresentHooked) {
        void** gameDeviceVtable = *reinterpret_cast<void***>(pDevice);
        void* gamePresentAddr = gameDeviceVtable[17];
        if (gamePresentAddr) {
            int createStatus = MH_CreateHook(gamePresentAddr, (void*)&Hooked_Present, (void**)&pOrigPresent);
            int enableStatus = MH_EnableHook(gamePresentAddr);

            char buf[256];
            snprintf(buf, sizeof(buf), "[CRC_Lab] Present hook attempt: Addr=0x%p, CreateStatus=%d, EnableStatus=%d",
                     gamePresentAddr, createStatus, enableStatus);
            AddLog(buf);

            if (createStatus == MH_OK && enableStatus == MH_OK) {
                AddLog("[CRC_Lab] Hooked REAL Game Present successfully.");
                isPresentHooked = true;
            } else {
                AddLog("[CRC_Lab] Failed to hook Present.");
            }
        } else {
            AddLog("[CRC_Lab] gamePresentAddr is NULL, cannot hook Present.");
        }
    }

    g_endSceneCallCountInFrame++;

    // 2. バックバッファの取得とハッシュ計算
    LPDIRECT3DSURFACE9 pRenderTarget = nullptr;
    LPDIRECT3DSURFACE9 pBackBuffer = nullptr;
    bool isBackBuffer = false;
    
    if (SUCCEEDED(pDevice->GetRenderTarget(0, &pRenderTarget))) {
        if (SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer))) {
            if (pRenderTarget == pBackBuffer) {
                isBackBuffer = true;
                
                D3DSURFACE_DESC desc;
                pBackBuffer->GetDesc(&desc);
                
                LPDIRECT3DSURFACE9 pOffscreenSurface = nullptr;
                HRESULT hrCreate = pDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &pOffscreenSurface, NULL);
                if (SUCCEEDED(hrCreate)) {
                    HRESULT hrGet = pDevice->GetRenderTargetData(pBackBuffer, pOffscreenSurface);
                    if (SUCCEEDED(hrGet)) {
                        D3DLOCKED_RECT lockedRect;
                        if (SUCCEEDED(pOffscreenSurface->LockRect(&lockedRect, NULL, D3DLOCK_READONLY))) {
                            uint32_t crc_accum = 0;
                            for(UINT y = 0; y < desc.Height; y += desc.Height/10) {
                                uint8_t* ps = (uint8_t*)lockedRect.pBits + y * lockedRect.Pitch;
                                crc_accum ^= CalculateCRC32(ps, desc.Width * 4);
                            }
                            
                            uint32_t currentCrc = crc_accum;
                            char buf[256];
                            snprintf(buf, sizeof(buf), "Frame [%05u] - EndScene Call %d: CRC = 0x%08X %s", 
                                     g_currentFrameCount,
                                     g_endSceneCallCountInFrame, 
                                     currentCrc,
                                     (g_endSceneCallCountInFrame > 1 && currentCrc != g_lastCrc) ? " <--- CHANGED!" : ""
                                     );
                            AddLog(buf);
                            
                            g_lastCrc = currentCrc;
                            pOffscreenSurface->UnlockRect();
                        } else {
                            AddLog("[CRC_Lab] LockRect failed!");
                        }
                    } else {
                        char buf[128];
                        snprintf(buf, sizeof(buf), "[CRC_Lab] GetRenderTargetData failed! HRESULT=0x%08lX", hrGet);
                        AddLog(buf);
                    }
                    pOffscreenSurface->Release();
                } else {
                    char buf[128];
                    snprintf(buf, sizeof(buf), "[CRC_Lab] CreateOffscreenPlainSurface failed! HRESULT=0x%08lX", hrCreate);
                    AddLog(buf);
                }
            } else {
                char buf[128];
                snprintf(buf, sizeof(buf), "Frame [%05u] - EndScene Call %d: NOT BACKBUFFER", g_currentFrameCount, g_endSceneCallCountInFrame);
                AddLog(buf);
            }
            if (pBackBuffer) pBackBuffer->Release();
        }
        if (pRenderTarget) pRenderTarget->Release();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "Frame [%05u] - EndScene Call %d: GetRenderTarget FAILED", g_currentFrameCount, g_endSceneCallCountInFrame);
        AddLog(buf);
    }

    return pOrigEndScene(pDevice);
}

// =========================================================
// ダミーデバイスによる EndScene 初期アドレスの取得
// =========================================================
bool GetDummyD3D9EndScene(void** pEndScene) {
    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) return false;

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, DefWindowProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "DummyClassLogLab", NULL };
    RegisterClassEx(&wc);
    HWND dummyWindow = CreateWindow("DummyClassLogLab", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

    D3DPRESENT_PARAMETERS d3dpp = {};
    d3dpp.Windowed = TRUE;
    d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    d3dpp.hDeviceWindow = dummyWindow;

    IDirect3DDevice9* pDummyDevice = nullptr;
    HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummyWindow,
                                    D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &pDummyDevice);
    if (FAILED(hr)) {
        d3dpp.Windowed = FALSE;
        hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummyWindow,
                                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &pDummyDevice);
        if (FAILED(hr)) {
            pD3D->Release();
            DestroyWindow(dummyWindow);
            UnregisterClass("DummyClassLogLab", wc.hInstance);
            return false;
        }
    }

    void** vtable = *reinterpret_cast<void***>(pDummyDevice);
    *pEndScene = vtable[42];

    pDummyDevice->Release();
    pD3D->Release();
    DestroyWindow(dummyWindow);
    UnregisterClass("DummyClassLogLab", wc.hInstance);
    return true;
}

// =========================================================
// Hook Manager
// =========================================================
DWORD WINAPI MainThread(LPVOID lpParam) {
    // ローダーロックを抜けるのを待つため少し待機
    Sleep(1000);

    FILE* f = fopen("present_hunter_log.txt", "w");
    if (f) {
        fprintf(f, "--- CRC Check Lab Initialized (v24) ---\n");
        fclose(f);
    }
    
    AddLog("[CRC_Lab] DLL Injected natively. Attempting MH_Initialize...");
    if (MH_Initialize() != MH_OK) {
        AddLog("[CRC_Lab] MH_Initialize failed!");
        return 0;
    }

    // Direct3DCreate9を動的に取得 (d3d9.dllがまだロードされていない場合を考慮)
    HMODULE hD3D9 = GetModuleHandle("d3d9.dll");
    if (!hD3D9) {
        hD3D9 = LoadLibrary("d3d9.dll");
    }
    
    if (hD3D9) {
        typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT);
        Direct3DCreate9_t pDirect3DCreate9 = (Direct3DCreate9_t)GetProcAddress(hD3D9, "Direct3DCreate9");
        
        if (pDirect3DCreate9) {
            IDirect3D9* pD3D = pDirect3DCreate9(D3D_SDK_VERSION);
            if (pD3D) {
                WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_CLASSDC, DefWindowProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, "DummyClassLogLab22", NULL };
                RegisterClassEx(&wc);
                HWND dummyWindow = CreateWindow("DummyClassLogLab22", "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
                D3DPRESENT_PARAMETERS d3dpp = {};
                d3dpp.Windowed = TRUE;
                d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                d3dpp.hDeviceWindow = dummyWindow;
                IDirect3DDevice9* pDummyDevice = nullptr;
                HRESULT hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummyWindow, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &pDummyDevice);
                if (FAILED(hr)) {
                    d3dpp.Windowed = FALSE;
                    hr = pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummyWindow, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &d3dpp, &pDummyDevice);
                }

                if (SUCCEEDED(hr) && pDummyDevice) {
                    void** vtable = *reinterpret_cast<void***>(pDummyDevice);
                    void* pEndScene = vtable[42];
                    
                    // JMP追跡
                    uint8_t* pCode = static_cast<uint8_t*>(pEndScene);
                    if (pCode[0] == 0xE9) {
                        int32_t relAddr = *reinterpret_cast<int32_t*>(pCode + 1);
                        pEndScene = reinterpret_cast<void*>(pCode + 5 + relAddr);
                        AddLog("[CRC_Lab] EndScene is already hooked. Traced JMP.");
                    }
                    
                    if (MH_CreateHook(pEndScene, (void*)&Hooked_EndScene, (void**)&pOrigEndScene) == MH_OK) {
                        MH_EnableHook(pEndScene);
                        AddLog("[CRC_Lab] EndScene Hook applied.");
                    } else {
                        AddLog("[CRC_Lab] MH_CreateHook failed on EndScene.");
                    }
                    pDummyDevice->Release();
                } else {
                    AddLog("[CRC_Lab] CreateDevice failed.");
                }
                pD3D->Release();
                DestroyWindow(dummyWindow);
                UnregisterClass("DummyClassLogLab22", wc.hInstance);
            } else {
                AddLog("[CRC_Lab] pDirect3DCreate9 failed to create IDirect3D9.");
            }
        } else {
            AddLog("[CRC_Lab] GetProcAddress(Direct3DCreate9) failed.");
        }
    } else {
        AddLog("[CRC_Lab] GetModuleHandle/LoadLibrary(d3d9.dll) failed.");
    }

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, MainThread, hModule, 0, nullptr);
    }
    return TRUE;
}
