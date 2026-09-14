#pragma once
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include <array>
#include <vector>
#include <cfenv>
namespace cccaster::sync {
class RollbackStates {
  public:
    void Reset(size_t size) {
        for (auto &s : slots_) {
            s.frame = 0;
            s.bytes.resize(size);
        }
    }
    bool Save(uint32_t f, cccaster::game_interface::IGameMemory &mem, uint32_t local = 0, uint32_t remote = 0) {
        auto &s = slots_[f % slots_.size()];
        s.frame = 0;
        if (!mem.SaveSnapshot(s.bytes))
            return false;
        std::fegetenv(&s.fp);
        s.local = local; s.remote = remote;
        s.frame = f;
        return true;
    }
    bool Load(uint32_t f, cccaster::game_interface::IGameMemory &mem) {
        auto &s = slots_[f % slots_.size()];
        if (s.frame != f)
            return false;
        if (!mem.LoadSnapshot(s.bytes))
            return false;
        std::fesetenv(&s.fp);
        return true;
    }

    // ゲームスレッドの締切外で既存リングを直接読む。新しい全状態リングは作らない。
    template<class Write> bool Export(uint32_t f, Write write) const {
        const auto &s = slots_[f % slots_.size()];
        if (!f || s.frame != f) return false;
        return write(s.bytes, s.fp, s.local, s.remote);
    }
  private:
    struct Slot {
        uint32_t frame = 0;
        std::vector<char> bytes;
        std::fenv_t fp{};
        uint32_t local = 0, remote = 0;
    };
    std::array<Slot, 12> slots_;
};
} // namespace cccaster::sync
