#pragma once
#include <array>
#include <cstdint>
namespace cccaster::sync {
// 実音声は巻き戻さない。イントロスクリプトの再生完了判定だけを同期Fで再現する。
struct IntroSoundClock {
    inline static std::array<uint32_t, 1500> duration{};
    inline static std::array<uint32_t, 1500> until{}; // ゲームスレッドのsnapshot対象。
    static constexpr uint32_t Frames(uint32_t bytes, uint32_t align, uint32_t frequency) {
        const uint64_t rate = uint64_t(align) * frequency;
        return rate ? uint32_t((uint64_t(bytes) * 60 + rate - 1) / rate) : 0;
    }
    static void Start(uint32_t sound, uint32_t frame) {
        if (sound < until.size()) until[sound] = frame + duration[sound];
    }
    static bool Playing(uint32_t sound, uint32_t frame) {
        return sound < until.size() && until[sound] && int32_t(until[sound] - frame) > 0;
    }
};
}
