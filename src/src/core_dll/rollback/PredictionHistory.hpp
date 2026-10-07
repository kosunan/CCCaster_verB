#pragma once
#include <array>
#include <cstdint>
#include "shared_contracts/NetplaySettings.hpp"
namespace cccaster::sync {
// ゲームスレッドだけが変更する。通信リングから確定値を読み取って照合する。
class PredictionHistory {
  public:
    struct Entry {
        uint32_t frame = 0, local = 0, remote = 0;
        bool valid = false;
    };
    void Reset(uint32_t first, uint32_t limit) {
        entries_ = {};
        first_ = next_ = first;
        confirmed_ = first - 1;
        limit_ = limit;
    }
    bool Record(uint32_t frame, uint32_t local, uint32_t remote) {
        if (frame != next_)
            return false;
        entries_[frame % entries_.size()] = {frame, local, remote, true};
        ++next_;
        return true;
    }
    const Entry *Get(uint32_t frame) const {
        const auto &e = entries_[frame % entries_.size()];
        return e.valid && e.frame == frame ? &e : nullptr;
    }
    template <class Read> uint32_t Reconcile(Read read) {
        uint32_t mismatch = 0;
        for (uint32_t f = confirmed_ + 1; f < next_; ++f) {
            uint32_t value;
            if (!read(f, value))
                break;
            auto &e = entries_[f % entries_.size()];
            if (!e.valid || e.frame != f)
                return f;
            if (e.remote != value && !mismatch)
                mismatch = f;
            e.remote = value;
            confirmed_ = f;
        }
        return mismatch;
    }
    // 両者の未確定入力を照合する。古い相手入力が未着でも新しい自入力の訂正は拾う。
    template <class Local, class Remote> uint32_t Reconcile(Local readLocal, Remote readRemote) {
        uint32_t mismatch = 0;
        for (uint32_t f = confirmed_ + 1; f < next_; ++f) {
            auto &e = entries_[f % entries_.size()];
            if (!e.valid || e.frame != f) return f;
            uint32_t local = 0, remote = 0;
            const bool haveLocal = readLocal(f, local), haveRemote = readRemote(f, remote);
            if (((haveLocal && e.local != local) || (haveRemote && e.remote != remote)) && !mismatch)
                mismatch = f;
            if (haveLocal) e.local = local;
            if (haveRemote) e.remote = remote;
            if (haveLocal && haveRemote && f == confirmed_ + 1) confirmed_ = f;
        }
        return mismatch;
    }
    bool CanPredict() const {
        return next_ - confirmed_ <= limit_;
    }
    template <class Read> bool ReadyToResume(Read read, bool allowPrediction) const {
        uint32_t value;
        // 上限で止まったときは最古の1件の確定で1枠空く。
        // 最新入力まで全件待つと、Rの吸収枠を毎回使い切ってしまう。
        if (allowPrediction && limit_ && !CanPredict())
            return read(confirmed_ + 1, value);
        for (uint32_t f = confirmed_ + 1; f < next_; ++f)
            if (!read(f, value)) return false;
        return read(next_, value);
    }
    uint32_t Prediction() const {
        const auto *e = Get(next_ - 1);
        return e ? e->remote : 0;
    }
    uint32_t LocalPrediction() const {
        const auto *e = Get(next_ - 1);
        return e ? e->local : 0;
    }
    template <class Local, class Remote> bool ReadyToResume(Local readLocal, Remote readRemote, bool allowPrediction) const {
        uint32_t a, b;
        if (allowPrediction && limit_ && !CanPredict())
            return readLocal(confirmed_ + 1, a) && readRemote(confirmed_ + 1, b);
        for (uint32_t f = confirmed_ + 1; f <= next_; ++f)
            if (!readLocal(f, a) || !readRemote(f, b)) return false;
        return true;
    }
    uint32_t Confirmed() const {
        return confirmed_;
    }
    bool NeedsReplaySnapshot(uint32_t frame) const {
        // 次のReconcileはconfirmed_+1からのみ照合する。確定済みの起点には戻らない。
        return frame > confirmed_;
    }
    uint32_t Next() const {
        return next_;
    }
    template <class Read> bool ResolveReplay(uint32_t frame, Read read, uint32_t &local, uint32_t &remote) {
        auto &e = entries_[frame % entries_.size()];
        if (!e.valid || e.frame != frame)
            return false;
        local = e.local;
        if (!read(frame, remote)) {
            const auto *prev = Get(frame - 1);
            remote = prev ? prev->remote : 0;
        }
        e.remote = remote;
        return true;
    }
    template <class Local, class Remote> bool ResolveReplay(uint32_t frame, Local readLocal, Remote readRemote,
                                                           uint32_t &local, uint32_t &remote) {
        auto &e = entries_[frame % entries_.size()];
        if (!e.valid || e.frame != frame) return false;
        const auto *prev = Get(frame - 1);
        if (!readLocal(frame, local)) local = prev ? prev->local : 0;
        if (!readRemote(frame, remote)) remote = prev ? prev->remote : 0;
        e.local = local; e.remote = remote;
        return true;
    }

  private:
    std::array<Entry, public_api::NetplaySettings::RollbackHistoryFrames> entries_{};
    uint32_t first_ = 1, next_ = 1, confirmed_ = 0, limit_ = 0;
};
} // namespace cccaster::sync
