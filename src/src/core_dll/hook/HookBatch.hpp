#pragma once
#if defined(_WIN32) && defined(cccaster_hook_EXPORTS)
#include <windows.h>
#include <MinHook.h>
namespace cccaster::hook_batch {
// ゲームスレッドの初回メニュー準備だけを囲む。入れ子でもスレッド停止は最後の一度。
inline thread_local unsigned depth = 0;
inline thread_local bool pending = false;
inline MH_STATUS Enable(void* address) {
    if (!depth) return MH_EnableHook(address);
    const auto result = MH_QueueEnableHook(address);
    if (result == MH_OK) pending = true;
    return result;
}
class Scope {
    bool enabled;
public:
    explicit Scope(bool use) : enabled(use) { if (enabled) ++depth; }
    ~Scope() {
        if (enabled && --depth == 0 && pending) {
            pending = false;
            // 部分適用のまま実ゲームへ戻らない。
            if (MH_ApplyQueued() != MH_OK) ExitProcess(ERROR_WRITE_FAULT);
        }
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
}
#else
namespace cccaster::hook_batch { struct Scope { explicit Scope(bool) {} }; }
#endif
