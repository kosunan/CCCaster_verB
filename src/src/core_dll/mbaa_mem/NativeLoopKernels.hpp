#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#if defined(__i386__) || defined(__x86_64__)
#include <emmintrin.h>
#endif

namespace cccaster::game_memory::native_loops {
inline constexpr size_t ObjectStride = 0x33c, ObjectCount = 1000, SoundCount = 1500;

// コールバックを挟まずに次の有効枠まで進む。呼出しごとにメモリを読み直すため、
// 元処理が後続枠を生成・削除した場合も、復元後も、過去の一覧を使用しない。
inline uintptr_t NextLive(uintptr_t cursor, uintptr_t end, size_t existsOffset) {
    while (cursor < end && !*reinterpret_cast<const uint8_t*>(cursor - existsOffset))
        cursor += ObjectStride;
    return cursor;
}

inline uint32_t NextCollision(const uint8_t* pool, uint32_t first, const uint8_t* actor) {
    const auto owner = actor[0x2f0];
    for (auto i = first; i < ObjectCount; ++i) {
        const auto* slot = pool + i * ObjectStride;
        if (slot[0] && slot[9] != 0x1f && slot + 4 != actor && slot[0x2f4] != owner)
            return i;
    }
    return ObjectCount;
}

inline uint32_t NextSoundScalar(std::span<const uint8_t> flags, uint32_t first) {
    while (first < flags.size() && flags[first] != 1) ++first;
    return first;
}
#if defined(__i386__) || defined(__x86_64__)
__attribute__((target("sse2"))) inline uint32_t NextSoundVector(std::span<const uint8_t> flags, uint32_t first) {
    const auto ones = _mm_set1_epi8(1);
    // 末尾を越えて読まない。1以外のフラグを再生要求として扱わない。
    for (; first + 16 <= flags.size(); first += 16) {
        const auto values = _mm_loadu_si128(reinterpret_cast<const __m128i*>(flags.data() + first));
        const auto bits = static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(values, ones)));
        if (bits) return first + __builtin_ctz(bits);
    }
    return NextSoundScalar(flags, first);
}
#endif

// 0x468505の重複する範囲検査を除く。命令実行は状態・命令列を変更し得るので、
// 実行後は毎回state/count/tableを取得し直す。動的な入力・RNG判定は元関数のまま。
template<class State, class Count, class Command, class Phase, class Execute>
inline void RunCommands(unsigned phase, State state, Count count, Command command, Phase kind, Execute execute) {
    for (unsigned index = 0;; ++index) {
        const auto current = state();
        if (index >= count(current)) return;
        const auto entry = command(current, index);
        if (entry) {
            const auto selector = kind(entry);
            if (selector == 0xff || selector == phase) execute(entry);
        }
    }
}
}
