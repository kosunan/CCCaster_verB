#pragma once
#include "core_dll/spectator/Transport.hpp"
#include "core_dll/mbaa_mem/GamePhase.hpp"
#include "core_dll/timing/Metronome.hpp"

namespace cccaster::spectator {
class Playback {
    Record pending_;
    bool have_ = false, selecting_ = false, retrying_ = false, catching_ = false;
    bool ending_ = false, catchupPending_ = false;
    uint32_t next_ = 0, nav_ = 0, last_ = 0;
    game_interface::GamePhase previous_ = game_interface::GamePhase::Unknown;
    core::netplay::Metronome pace_;
    void Pace(bool fast);
public:
    std::array<uint32_t, 2> inputs{};
    StartData match{};
    Score score{};
    bool Step(game_interface::GamePhase phase);
    uint32_t Last() const { return last_; }
    bool Catching() const { return catching_; }
};
}
