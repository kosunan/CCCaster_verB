#pragma once
#include <cstdint>
#include <cstdlib>

namespace cccaster::diagnostics::input {
inline bool Enabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("CCCASTER_INPUT_DIAGNOSTIC");
        return value && value[0] == '1';
    }();
    return enabled;
}
// 各呼出箇所のゲームスレッドで使用。変化時と60回ごとの生存記録だけを残す。
struct Sampler {
    uint32_t count = 0, previous1 = 0, previous2 = 0;
    bool previousGate = false;
    bool Record(uint32_t p1, uint32_t p2, bool gate) {
        const bool changed = p1 != previous1 || p2 != previous2 || gate != previousGate;
        previous1 = p1; previous2 = p2; previousGate = gate;
        return ++count % 60 == 1 || changed;
    }
};
}
