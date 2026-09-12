#pragma once
#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#include <cwchar>
namespace cccaster::platform {
// 同一ログオンセッションのCCCaster同士だけで物理コア選択を調整する。
// OSや他アプリをそのCPUから排除する予約ではない。呼出スレッドが解放する。
inline HANDLE AcquireGameCore(uint32_t mask) {
    wchar_t name[80];
    std::swprintf(name, 80, L"Local\\CCCaster.GameCore.Group0.%08X", unsigned(mask));
    HANDLE handle = CreateMutexW(nullptr, FALSE, name);
    if (!handle) return nullptr;
    const auto result = WaitForSingleObject(handle, 0);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return handle;
    CloseHandle(handle);
    return nullptr;
}
inline void ReleaseGameCore(HANDLE handle) {
    if (handle) { ReleaseMutex(handle); CloseHandle(handle); }
}
}
#endif
