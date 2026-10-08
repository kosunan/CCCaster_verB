#pragma once
#include <cstdint>

namespace cccaster::core::timer {
// D3Dはゲームスレッドだけで使う。Present後の待機区間に限って登録し、
// 生入力時計・通信スレッド・ゲーム更新中からは実行しない。
struct IdlePresentation {
    using Callback = void (*)(int64_t remainingUs);
    inline static thread_local Callback callback = nullptr;
    // 呼出側は粗い待機区間に限る。提示側にもスピン開始までの残り時間だけを渡す。
    static void Pump(int64_t remainingUs, int64_t spinGuardUs = 0) {
        const auto availableUs = remainingUs - (spinGuardUs > 0 ? spinGuardUs : 0);
        if (callback && availableUs > 1000) callback(availableUs);
    }
    struct Scope {
        Callback previous;
        explicit Scope(Callback value) : previous(callback) {
            callback = value;
        }
        ~Scope() { callback = previous; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
};
}
