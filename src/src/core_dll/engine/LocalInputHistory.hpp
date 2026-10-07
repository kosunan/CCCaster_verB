#pragma once
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include <array>

namespace cccaster::domain::session {
// An input sampled for normal update f finalizes the native reservation in f-1.
// The current update remains a held-input prediction until the next sample.
class LocalInputHistory {
  public:
    void Clear() { entries_ = {}; }
    void Publish(uint32_t frame, game_interface::GameInput p1, game_interface::GameInput p2) {
        entries_[frame % entries_.size()] = {frame,{p1.Pack(),p2.Pack()}};
    }
    bool Read(uint32_t simulationFrame, unsigned player, uint32_t& value) const {
        const auto source = simulationFrame + 1;
        const auto& entry = entries_[source % entries_.size()];
        if (player > 1 || entry.frame != source) return false;
        value = entry.inputs[player];
        return true;
    }
    static bool Eligible(int mode, bool liveBattle, bool paused, bool configuring, bool holding,
                         bool replacingState, bool playbackOrRecording, uint16_t controls) {
        return (mode == 1 || mode == 5) && liveBattle && !paused && !configuring && !holding &&
            !replacingState && !playbackOrRecording && !(controls & (CC_BUTTON_START | CC_BUTTON_FN1 | CC_BUTTON_FN2));
    }
  private:
    struct Entry { uint32_t frame=0; std::array<uint32_t,2> inputs{}; };
    std::array<Entry,4> entries_{};
};
}
