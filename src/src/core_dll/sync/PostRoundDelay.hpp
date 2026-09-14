#pragma once
#include <cstdint>
#include <algorithm>

namespace cccaster::core::sync {
// 決着が確定した後だけRを入力余裕へ移す。入口では最後の確定入力を保持し、
// 入力番号を逆行させずにD+Rへ移行する。次の世代では必ず解除する。
struct PostRoundDelay {
    uint32_t first = 0, extra = 0;
    void Reset() { first = extra = 0; }
    void Begin(uint32_t next, uint32_t rollback) {
        if (!first && next && rollback) { first = next; extra = rollback; }
    }
    uint32_t Source(uint32_t frame) const {
        if (!first || frame < first) return frame;
        return frame - first < extra ? first - 1 : frame - extra;
    }
};
}
