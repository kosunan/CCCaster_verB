#pragma once
#include "shared_contracts/NetplaySettings.hpp"
#include <cstdint>
#include <limits>

namespace cccaster::core::sync {
// ゲームスレッドだけが所有する。送信先行量と消費位置を混同しない。
class FrameSequence {
  public:
    static constexpr uint32_t STRIDE = 65536;
    // 最新から10件を再送する現行wire形式で、最古の未消費入力を保持する。
    static constexpr uint32_t MAX_LOOKAHEAD = cccaster::public_api::NetplaySettings::MaxBufferedFrames;
    bool Begin(uint32_t lookahead) {
        if (lookahead > MAX_LOOKAHEAD || _epoch >= 65534)
            return false;
        ++_epoch;
        _base = _epoch * STRIDE;
        _next = _base + 1;
        _lookahead = lookahead;
        return true;
    }
    uint32_t Base() const {
        return _base;
    }
    uint32_t Next() const {
        return _next;
    }
    uint32_t Capture() const {
        return _next + _lookahead;
    }
    uint32_t Lookahead() const {
        return _lookahead;
    }
    bool CanAdvance() const {
        return Capture() < _base + STRIDE - 1;
    }
    // 相手が直後の同じ画面へ到達済みなら、旧世代をもう消費しない。
    // ACK値の捏造・フレーム消費はせず、世代境界での履歴保持終了だけを判定する。
    bool PeerEnteredNextPhase(uint64_t peerToken, uint8_t phase) const {
        return _epoch > 0 && _epoch < 65534 &&
               peerToken == (uint64_t(_base + STRIDE) << 32 | phase);
    }
    // 同じ番号の再消費や、欠番を作る進行要求は拒否する。
    bool Commit(uint32_t frame) {
        if (!_epoch || frame != _next || !CanAdvance())
            return false;
        ++_next;
        return true;
    }

  private:
    uint32_t _epoch = 0, _base = 0, _next = 0, _lookahead = 0;
};
} // namespace cccaster::core::sync
