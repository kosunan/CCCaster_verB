#pragma once
#include <cstdint>

namespace cccaster::core::timer {
// D3Dはゲームスレッドだけで使う。Present後の待機区間に限って登録し、
// 生入力時計・通信スレッド・ゲーム更新中からは実行しない。
struct IdlePresentation {
    using Callback = void (*)(int64_t remainingUs);
    inline static thread_local Callback callback = nullptr;
    static void Pump(int64_t remainingUs) {
        if (callback && remainingUs > 1000) callback(remainingUs);
    }
    struct Scope {
        Callback previous;
        explicit Scope(Callback value) : previous(callback) { callback = value; }
        ~Scope() { callback = previous; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
};
}
