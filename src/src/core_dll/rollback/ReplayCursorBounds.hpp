#pragma once
#include <cstddef>

namespace cccaster::sync {
// 記録末尾を巻き戻す処理と、循環する読み取り位置の復元を区別する。
inline bool CanRestoreReplayCursor(int savedIndex, size_t savedBytes, bool hasLast,
                                   int currentIndex, size_t currentBytes, bool playback) {
    constexpr size_t stateSize = 8, maxBytes = 16000000;
    if (savedBytes > maxBytes || currentBytes > maxBytes || savedBytes > currentBytes)
        return false;
    if (hasLast && (savedIndex < 0 || size_t(savedIndex) >= savedBytes / stateSize))
        return false;
    if (playback) return currentBytes == savedBytes;
    return !hasLast || currentIndex >= savedIndex;
}
}
