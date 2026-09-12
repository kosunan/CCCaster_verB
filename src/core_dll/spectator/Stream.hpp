#pragma once
#include "core_dll/mbaa_mem/RngState.hpp"
#include "core_dll/sync/SelectionState.hpp"
#include "shared_contracts/SpectatorHello.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <memory>

namespace cccaster::spectator {
// TCP専用。プレイヤーのUDP版10/拡張5には混ぜない。全整数は32bit little endian。
enum Kind : uint32_t { Start = 1, Epoch, Input, Result, Retry, Heartbeat, Selection };
struct Score {
    uint32_t p1 = 0, p2 = 0, unresolved = 0, revision = 0;
};
struct StartData {
    game_interface::RngState rng{};
    core::sync::SelectionState p1{}, p2{};
    char names[2][32]{};
    Score score{};
    uint32_t roundsToWin = 2;
    uint32_t damageLevel = 2, timerSpeed = 2;
};
static_assert(sizeof(StartData) == 444);
struct Record {
    uint32_t size, kind, frame;
    std::array<uint32_t, 128> payload;
    template<class T> void Set(Kind k, uint32_t f, const T &value) {
        static_assert(sizeof(T) <= sizeof(payload));
        size = 12 + sizeof(T); kind = k; frame = f;
        std::memcpy(payload.data(), &value, sizeof(T));
    }
    template<class T> T Get() const {
        T value;
        std::memcpy(&value, payload.data(), sizeof(T));
        return value;
    }
    static uint32_t ExpectedSize(uint32_t k) {
        switch (k) {
        case Start: return 12 + sizeof(StartData);
        case Selection: return 12 + sizeof(StartData);
        case Epoch: return 12 + sizeof(game_interface::RngState);
        case Input: return 20;
        case Result: return 12 + sizeof(Score);
        case Retry: return 16;
        case Heartbeat: return 16;
        default: return 0;
        }
    }
    bool Valid() const {
        if (!ExpectedSize(kind) || size != ExpectedSize(kind)) return false;
        if (kind == Start || kind == Selection) {
            const auto s = Get<StartData>();
            return (kind == Selection || (frame && (frame & 65535) == 1)) && s.p1.Valid() && s.p2.Valid() &&
                s.p1.confirmed && s.p2.confirmed && s.p1.stageConfirmed &&
                s.names[0][31] == 0 && s.names[1][31] == 0 && s.roundsToWin >= 1 && s.roundsToWin <= 5 &&
                s.damageLevel <= 4 && s.timerSpeed <= 4;
        }
        if (kind == Epoch) return frame && (frame & 65535) == 1;
        if (kind == Input) return frame && (frame & 65535) && (payload[0] >> 16) <= 9 && (payload[1] >> 16) <= 9;
        if (kind == Retry) return payload[0] <= 1;
        return true;
    }
};
// 単一書き手・単一読み手。満杯時も上書き/待機なし。可変長部分のみコピー。
// 対戦スレッドはPush、専用workerはPop。観戦端末では向きが逆。
template<uint32_t N> class Queue {
    static_assert(N && !(N & (N - 1)));
    std::array<Record, N> slots_;
    alignas(64) std::atomic<uint32_t> write_{0};
    alignas(64) std::atomic<uint32_t> read_{0};
public:
    Queue() { std::memset(slots_.data(), 0, sizeof(slots_)); } // 初期化スレッドで全ページを先行確保。
    bool PushInput(uint32_t frame, uint32_t p1, uint32_t p2) {
        const auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) == N) return false;
        auto &slot = slots_[w & (N - 1)];
        slot.size = 20; slot.kind = Input; slot.frame = frame;
        slot.payload[0] = p1; slot.payload[1] = p2;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool Push(const Record &record) {
        const auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) == N) return false;
        std::memcpy(&slots_[w & (N - 1)], &record, record.size);
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool Pop(Record &record) {
        const auto r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        const auto &slot = slots_[r & (N - 1)];
        std::memcpy(&record, &slot, slot.size);
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
    uint32_t Count() const { return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire); }
};
// workerだけが所有する履歴。遅い観戦者のカーソルで保存量を増やさない。
template<uint32_t N> class Archive {
    static_assert(N && !(N & (N - 1)));
    std::unique_ptr<Record[]> slots_{new Record[N]};
public:
    uint64_t head = 1, start = 0;
    void Append(const Record &r) {
        if (r.kind == Start || r.kind == Selection) start = head;
        std::memcpy(&slots_[head & (N - 1)], &r, r.size);
        ++head;
    }
    const Record *Get(uint64_t cursor) const {
        return cursor && cursor < head && head - cursor <= N ? &slots_[cursor & (N - 1)] : nullptr;
    }
    uint64_t Join() const { return Get(start) ? start : 0; }
};
}
