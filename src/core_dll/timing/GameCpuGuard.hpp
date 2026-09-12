#pragma once
#include <cstdint>
namespace cccaster::core::timer {
// CPU0と同じ物理コアのSMT相手も外す。プロセスの許可範囲は広げない。
inline uint32_t GameCpuMask(uint32_t allowed, uint32_t cpuZeroCore,
                            unsigned physicalCores, unsigned logicalCpus, unsigned groups) {
    if (groups != 1 || logicalCpus > 32 || physicalCores < 4 || !(cpuZeroCore & 1u)) return 0;
    return allowed & ~cpuZeroCore;
}
// 診断用の論理CPU番号。符号・空白・末尾文字を許さず、32bitの範囲だけ受け付ける。
inline int ParseGameCpuPin(const char *text) {
    if (!text || !*text) return -1;
    unsigned value = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return -1;
        value = value * 10 + static_cast<unsigned>(*text - '0');
        if (value >= 32) return -1; // 大きな入力でも乗算がオーバーフローする前に拒否。
    }
    return static_cast<int>(value);
}
// 元のguardと外部の許可範囲を狭めるだけ。無効・範囲外は従来配置を維持。
inline uint32_t GameCpuPinMask(uint32_t guarded, int cpu) {
    if (cpu < 0 || cpu >= 32) return guarded;
    const uint32_t bit = uint32_t{1} << cpu;
    return (guarded & bit) ? bit : guarded;
}
}
