#pragma once
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "shared_contracts/NetplaySettings.hpp"
#include <array>
#include <cstdint>

namespace cccaster::sync {
// Read-only reconstruction of 41F2E0 -> 46B720/46B660 -> 46D6D0 -> 46D908.
// These values are comparison targets, never written into the actor.
struct ActorInputValue {
    uint8_t relative = 0, direction = 0;
    uint32_t buttons = 0, released = 0;
    bool operator==(const ActorInputValue&) const = default;
};
struct ActorInputContext {
    uintptr_t actor = 0;
    uint32_t previousButtons = 0, macroMode = 0;
    std::array<uint32_t, 6> masks{};
    uint8_t source = 0, facing = 0, stateKind = 0;
    uint16_t reverse = 0;
};
inline ActorInputValue ExpectedActorInput(const ActorInputContext& c, uint32_t packed) {
    constexpr uint8_t horizontal[]{0,3,2,1,6,5,4,9,8,7};
    constexpr uint8_t reversed[]{0,9,8,7,6,5,4,3,2,1};
    const auto raw = game_interface::GameInput::Unpack(packed);
    ActorInputValue result;
    auto direction = uint8_t(raw.direction & 15);
    if (direction >= 10) return {255,255,~0u,~0u};
    if (c.reverse) direction = reversed[direction];
    if (c.facing == 1) direction = horizontal[direction];
    result.relative = direction;
    result.direction = c.facing == 1 ? horizontal[direction] : c.reverse ? reversed[direction] : direction;
    uint32_t held = (raw.buttons & c.masks[5]) ? 3 : 0;
    for (unsigned i=0; i<5; ++i) if (raw.buttons & c.masks[i]) held |= 1u << i;
    const auto previous = (c.previousButtons >> 12) & 31;
    result.buttons = (held << 12) | (held & ~previous);
    result.released = previous & ~held;
    if (result.buttons & 16) {
        if (direction == 4 || direction == 6) result.buttons |= 0x9009;
        else if (direction == 2) result.buttons |= 0x3003;
        else if (!direction) result.buttons |= 0x7007;
    } else if ((result.buttons & 0x10000) && c.macroMode == 1 &&
               (c.stateKind == 0 || c.stateKind == 2) && !direction) result.buttons |= 0x7000;
    return result;
}

class InputWriteHistory {
  public:
    struct Write {
        ActorInputContext context{};
        ActorInputValue actual{};
        uint32_t applied = 0;
        bool prepared = false, observed = false;
    };
    struct Frame { uint32_t number = 0; std::array<Write,4> writes{}; };
    struct Difference { uint32_t first = 0, players = 0, count = 0; };
    void Reset() { frames_ = {}; requests_ = {}; active_ = 0; fault = false; }
    void Begin(uint32_t frame) {
        active_ = frame;
        frames_[frame % frames_.size()] = {frame,{}};
    }
    Write* Current(uintptr_t actor) {
        if (!active_ || actor < 0x555134 || (actor-0x555134) % 0xAFC) return nullptr;
        const auto index = (actor-0x555134) / 0xAFC;
        return index < 4 ? &frames_[active_ % frames_.size()].writes[index] : nullptr;
    }
    uint32_t Active() const { return active_; }
    const Frame* Get(uint32_t frame) const {
        const auto& entry = frames_[frame % frames_.size()];
        return entry.number == frame ? &entry : nullptr;
    }
    void Request(uint32_t frame, uintptr_t actor, uint32_t raw) {
        auto& request = requests_[frame % requests_.size()];
        if (request.frame != frame) request = {frame,0,{}};
        const auto index = (actor-0x555134) / 0xAFC;
        if (index >= 4) return;
        request.mask |= 1u << index;
        request.raw[index] = raw;
    }
    bool Corrected(uintptr_t actor, uint32_t raw, bool equal) {
        auto& request = requests_[active_ % requests_.size()];
        const auto index = (actor-0x555134) / 0xAFC;
        if (index >= 4 || request.frame != active_ || !(request.mask & (1u << index)) ||
            request.raw[index] != raw || !equal) return false;
        request.mask &= ~(1u << index);
        return true;
    }
    template<class Read, class Report>
    Difference Compare(uint32_t first, uint32_t end, Read read, Report report) const {
        Difference result;
        for (auto f=first; f<end; ++f) {
            const auto* frame = Get(f);
            if (!frame) continue;
            for (const auto& w : frame->writes) {
                uint32_t raw = 0;
                if (!w.prepared || !w.observed || w.context.source > 1 || !read(f,w.context.source,raw)) continue;
                const auto expected = ExpectedActorInput(w.context,raw);
                if (w.actual == expected) continue;
                if (!result.first) result.first = f;
                result.players |= 1u << w.context.source;
                ++result.count;
                report(f,w,raw,expected);
            }
        }
        return result;
    }
    bool fault = false;
  private:
    std::array<Frame,public_api::NetplaySettings::RollbackHistoryFrames> frames_{};
    struct RequestEntry { uint32_t frame=0,mask=0; std::array<uint32_t,4> raw{}; };
    std::array<RequestEntry,public_api::NetplaySettings::RollbackHistoryFrames> requests_{};
    uint32_t active_ = 0;
};
}
