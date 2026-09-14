#pragma once
#include <array>
#include <cstdint>
namespace cccaster::sync {
// 実音声は巻き戻さない。イントロ・決着スクリプトの再生完了判定を同期Fで再現する。
struct IntroSoundClock {
    static constexpr bool ControlsScript(bool inGame, uint32_t intro, bool p1Over, bool p2Over) {
        return inGame && (intro == 1 || intro == 2 || (intro == 0 && p1Over && p2Over));
    }
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
