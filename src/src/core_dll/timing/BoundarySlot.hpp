#pragma once
#include <atomic>
#include <cstdint>

namespace cccaster::core::timer {
// 単一書手・単一読手。停止した書手や前の締切の結果を待たない。
// 32bitでも全値をatomicにし、読取り途中の次世代公開を検出する。
struct alignas(64) BoundarySlot {
    struct Sample { int64_t due = 0, stamp = 0, entered = 0; };
    std::atomic<uint32_t> sequence{0};
    std::atomic<int64_t> due{0}, stamp{0}, entered{0};
    void Publish(Sample value) {
        sequence.fetch_add(1);
        due.store(value.due); stamp.store(value.stamp); entered.store(value.entered);
        sequence.fetch_add(1);
    }
    bool Read(int64_t target, Sample &value) const {
        const auto first = sequence.load();
        if (first & 1) return false;
        const Sample next{due.load(), stamp.load(), entered.load()};
        if (first != sequence.load() || next.due != target || next.stamp < target) return false;
        value = next;
        return true;
    }
};
}
