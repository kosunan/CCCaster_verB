#pragma once
#include <algorithm>
#include <cstdint>
namespace cccaster::core::sync {
struct EpochStart {
    uint32_t epoch = 0, serial = 0;
    int64_t hostTicks = 0;
    uint64_t stage = 0;
    enum : uint64_t { Ready = 1, Offer, Accepted, Commit, Committed };
    bool Valid() const {
        if (!epoch) return !serial && !hostTicks && !stage;
        return epoch % 65536 == 0 && stage >= Ready && stage <= Committed &&
               (stage == Ready ? !serial && !hostTicks : serial && hostTicks > 0);
    }
    bool NewerThan(const EpochStart &old) const {
        if (!Valid()) return false;
        if (epoch != old.epoch) return epoch > old.epoch;
        if (serial != old.serial) return serial > old.serial;
        return hostTicks == old.hostTicks && stage >= old.stage;
    }
    bool Matches(const EpochStart &other) const {
        return epoch == other.epoch && serial == other.serial && hostTicks == other.hostTicks;
    }
};
static_assert(sizeof(EpochStart) == 24);
class EpochStartGate {
  public:
    enum Result { Waiting, Armed, Expired };
    void Begin(uint32_t epoch) { local = {epoch, 0, 0, EpochStart::Ready}; dueTicks = 0; }
    Result Update(bool host, const EpochStart &peer, int64_t peerLocalTicks, int64_t now, int64_t rtt) {
        constexpr int64_t guard = 100000*60;
        if (peer.epoch != local.epoch || !peer.Valid()) return Waiting;
        if (host) {
            if (local.stage == EpochStart::Ready ||
                (local.stage == EpochStart::Offer && dueTicks - now < guard)) {
                const auto lead = std::clamp<int64_t>(rtt * 6 + 200000*60, 500000*60, 2500000*60);
                local = {local.epoch, local.serial + 1, now + lead, EpochStart::Offer};
                dueTicks = local.hostTicks;
            }
            if (local.stage == EpochStart::Offer && peer.stage == EpochStart::Accepted && local.Matches(peer))
                local.stage = EpochStart::Commit;
            if (local.stage == EpochStart::Commit) {
                if (now >= dueTicks) return Expired;
                if (peer.stage == EpochStart::Committed && local.Matches(peer)) return Armed;
            }
        } else {
            if (peer.stage == EpochStart::Offer && peer.serial > local.serial && peerLocalTicks - now >= guard) {
                local = peer;
                local.stage = EpochStart::Accepted;
                dueTicks = peerLocalTicks; // 受領時に固定。後続パケットの推定差で動かさない。
            }
            if (peer.stage == EpochStart::Commit && local.stage == EpochStart::Accepted && local.Matches(peer)) {
                if (now >= dueTicks) return Expired;
                local.stage = EpochStart::Committed;
                return Armed;
            }
        }
        return Waiting;
    }
    EpochStart local{};
    int64_t dueTicks = 0;
};
}
