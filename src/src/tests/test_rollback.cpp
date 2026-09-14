#include "core_dll/rollback/PointerSnapshot.hpp"
#include "core_dll/rollback/PredictionHistory.hpp"
#include "core_dll/rollback/GameSnapshotLayout.hpp"
#include "core_dll/rollback/ReplayRoundLocation.hpp"
#include "core_dll/rollback/IntroSoundClock.hpp"
#include "core_dll/rollback/RollbackStates.hpp"
#include <iostream>
#include <array>
#include <cstdlib>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            std::cerr << "FAILED line " << __LINE__ << "\n";                                                 \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)

struct Model {
    uint32_t x = 0, y = 0, rng = 17;
    bool operator==(const Model &) const = default;
};
static void advance(Model &s, uint32_t a, uint32_t b) {
    s.rng = s.rng * 1664525u + 1013904223u + (a ^ b);
    s.x += a * 3u + b + s.rng;
    s.y ^= b * 7u + a + (s.x >> 3);
}
static int delayedSimulation(uint32_t limit) {
    using cccaster::sync::PredictionHistory;
    constexpr uint32_t first = 65537, count = 700;
    PredictionHistory history;
    history.Reset(first, limit);
    std::array<Model, 32> saves;
    Model state, baseline;
    uint32_t frontier = first, rollbacks = 0, skipped = 0;
    auto local = [](uint32_t f) { return ((f % 9) << 16) | ((f * 17) & 1023); };
    auto remote = [](uint32_t f) { return (((f / 3) % 9) << 16) | ((f * 31) & 1023); };
    for (uint32_t f = first; f < first + count; ++f)
        advance(baseline, local(f), remote(f));
    for (uint32_t tick = 0; tick < count + 50; ++tick) {
        auto read = [&](uint32_t f, uint32_t &v) {
            if (f - first + (f * 13 % 7) > tick)
                return false;
            v = remote(f);
            return true;
        };
        const auto previouslyConfirmed=history.Confirmed();
        auto mismatch = history.Reconcile(read);
        if (mismatch) {
            CHECK(mismatch > previouslyConfirmed);
            CHECK(frontier - mismatch <= limit);
            ++rollbacks;
            state = saves[mismatch % 32];
            for (uint32_t f = mismatch; f < frontier; ++f) {
                uint32_t a, b;
                CHECK(history.ResolveReplay(f, read, a, b));
                if (history.NeedsReplaySnapshot(f)) saves[f % 32] = state;
                else { saves[f % 32] = {0xdeadbeef,0xbadf00d,0}; ++skipped; }
                advance(state, a, b);
            }
        }
        if (frontier == first + count) {
            if (history.Confirmed() == frontier - 1)
                break;
            continue;
        }
        uint32_t value;
        if (!read(frontier, value)) {
            if (!history.CanPredict())
                continue;
            value = history.Prediction();
        }
        saves[frontier % 32] = state;
        CHECK(history.Record(frontier, local(frontier), value));
        advance(state, local(frontier), value);
        ++frontier;
    }
    CHECK(frontier == first + count);
    CHECK(history.Confirmed() == frontier - 1);
    CHECK(state == baseline);
    CHECK(limit == 0 || rollbacks > 50);
    CHECK(limit == 0 || skipped > 50);
    return 0;
}

struct SnapshotMemory : cccaster::game_interface::IGameMemory {
    uint32_t value = 0;
    bool IsAvailable() const override {
        return true;
    }
    uint32_t GameMode() const override {
        return 1;
    }
    uint8_t IntroState() const override {
        return 0;
    }
    uint32_t WorldTimer() const override {
        return value;
    }
    uint32_t RealTimer() const override {
        return value;
    }
    uint32_t MenuStateCounter() const override {
        return 0;
    }
    void WriteInput(cccaster::game_interface::GameInput, cccaster::game_interface::GameInput) override {}
    bool SaveSnapshot(std::span<char> b) override {
        if (b.size() != 4)
            return false;
        std::memcpy(b.data(), &value, 4);
        return true;
    }
    bool LoadSnapshot(std::span<char> b) override {
        if (b.size() != 4)
            return false;
        std::memcpy(&value, b.data(), 4);
        return true;
    }
};
int main() {
    CHECK(delayedSimulation(0) == 0);
    CHECK(delayedSimulation(4) == 0);
    CHECK(delayedSimulation(8) == 0);
    using namespace cccaster::sync;
    {
        PredictionHistory h;
        h.Reset(1, 4);
        for (uint32_t f = 1; f <= 4; ++f) CHECK(h.Record(f, 0, 0));
        CHECK(!h.CanPredict());
        auto latestOnly = [](uint32_t f, uint32_t &v) { v = 9; return f == 5; };
        CHECK(!h.ReadyToResume(latestOnly, true));
        auto oldestOnly = [](uint32_t f, uint32_t &v) { v = 9; return f == 1; };
        CHECK(h.ReadyToResume(oldestOnly, true));
        CHECK(!h.ReadyToResume(oldestOnly, false));
        CHECK(h.Confirmed() == 0); // 待機判定で不一致を消費しない。
        CHECK(h.Reconcile(oldestOnly) == 1);
        CHECK(h.CanPredict());
        CHECK(h.Record(5, 0, 9));
        CHECK(!h.CanPredict()); // 4F上限は緩めない。
        h.Reset(5, 0);
        CHECK(!h.ReadyToResume(oldestOnly, true));
        CHECK(h.ReadyToResume(latestOnly, true));
    }
    SnapshotMemory memory;
    RollbackStates ring;
    ring.Reset(4);
    const int oldRound = std::fegetround();
    std::fesetround(FE_DOWNWARD);
    memory.value = 10;
    CHECK(ring.Save(1, memory));
    std::fesetround(FE_UPWARD);
    memory.value = 20;
    CHECK(ring.Load(1, memory));
    CHECK(memory.value == 10 && std::fegetround() == FE_DOWNWARD);
    memory.value = 30;
    CHECK(ring.Save(13, memory));
    CHECK(!ring.Load(1, memory));
    CHECK(ring.Load(13, memory) && memory.value == 30);
    for (uint32_t f = 14; f <= 25; ++f) {
        memory.value = f;
        CHECK(ring.Save(f, memory, f + 100, f + 200));
    }
    for (uint32_t f = 15; f < 25; ++f) {
        CHECK(ring.Export(f, [&](const auto &bytes, const auto &, uint32_t local, uint32_t remote) {
            uint32_t value = 0; std::memcpy(&value, bytes.data(), 4);
            return value == f && local == f + 100 && remote == f + 200;
        }));
    }
    CHECK(!ring.Export(13, [](const auto &, const auto &, auto, auto) { return true; }));
    ring.Reset(4);
    CHECK(!ring.Export(25, [](const auto &, const auto &, auto, auto) { return true; }));
    CHECK(!ring.Load(13, memory));
    std::fesetround(oldRound);
    PointerSnapshot p;
    CHECK(p.Configure(GameSnapshotLayout));
    CHECK(p.Size() == 1241295); // 進行値20B、演出用乱数228Bを含む。
    // 未作成の次ラウンドを前ラウンド末尾と混同せず、再確保後の現アドレスで解決する。
    CHECK(LocateReplayRound(0, 0, 0).valid && !LocateReplayRound(0, 0, 0).address);
    CHECK(LocateReplayRound(0x1000, 0x1140, 1).valid && !LocateReplayRound(0x1000, 0x1140, 1).address);
    CHECK(LocateReplayRound(0x8000, 0x8280, 1).address == 0x8140);
    CHECK(!LocateReplayRound(0x8000, 0x8280, 3).valid);
    CHECK(!LocateReplayRound(0x8000, 0x7fff, 0).valid);
    CHECK(!LocateReplayRound(0x8000, 0x8281, 0).valid);
    CHECK(IntroSoundClock::Frames(192000, 4, 48000) == 60);
    CHECK(IntroSoundClock::ControlsScript(true, 1, false, false));
    CHECK(IntroSoundClock::ControlsScript(true, 2, false, false));
    CHECK(IntroSoundClock::ControlsScript(true, 0, true, true));
    CHECK(!IntroSoundClock::ControlsScript(true, 0, true, false));
    CHECK(!IntroSoundClock::ControlsScript(true, 0, false, false));
    CHECK(!IntroSoundClock::ControlsScript(false, 0, true, true));
    CHECK(IntroSoundClock::Frames(192004, 4, 48000) == 61);
    IntroSoundClock::duration[400] = 60;
    IntroSoundClock::Start(400, 131073);
    const auto savedSound = IntroSoundClock::until;
    CHECK(IntroSoundClock::Playing(400, 131132));
    CHECK(!IntroSoundClock::Playing(400, 131133));
    IntroSoundClock::Start(400, 131100);
    IntroSoundClock::until = savedSound;
    CHECK(!IntroSoundClock::Playing(400, 131133));
    std::array<uint32_t, 4> a{1, 2, 3, 4};
    SnapshotNode layout[] = {{-1, reinterpret_cast<uintptr_t>(a.data()), 0, 4},
                             {-1, reinterpret_cast<uintptr_t>(&a[3]), 0, 4}};
    CHECK(p.Configure(layout));
    std::vector<char> bytes(p.Size());
    CHECK(p.Save(bytes));
    a = {10, 20, 30, 40};
    CHECK(p.Load(bytes));
    CHECK(a[0] == 1 && a[1] == 20 && a[2] == 30 && a[3] == 4);
    CHECK(!p.Configure({}));
    CHECK(!p.Load(bytes));
    if (sizeof(uintptr_t) == 4) {
        uint32_t child = 77, other = 99;
        uint32_t root = uint32_t(reinterpret_cast<uintptr_t>(&child));
        SnapshotNode tree[] = {{-1, reinterpret_cast<uintptr_t>(&root), 0, 4}, {0, 0, 0, 4}};
        CHECK(p.Configure(tree));
        bytes.resize(p.Size());
        CHECK(p.Save(bytes));
        root = uint32_t(reinterpret_cast<uintptr_t>(&other));
        child = 0;
        CHECK(p.Load(bytes));
        CHECK(child == 77 && other == 99);
        root = 0;
        CHECK(p.Save(bytes));
        root = uint32_t(reinterpret_cast<uintptr_t>(&other));
        CHECK(p.Load(bytes));
        CHECK(root == 0 && other == 99);
    }
    PredictionHistory h;
    h.Reset(65537, 4);
    for (uint32_t f = 65537; f < 65541; ++f) {
        CHECK(h.CanPredict());
        CHECK(h.Record(f, 0x60001, 0x40002));
    }
    CHECK(!h.CanPredict());
    CHECK(!h.Record(65540, 0, 0));
    auto read = [](uint32_t f, uint32_t &value) {
        if (f > 65538)
            return false;
        value = f == 65537 ? 0x40002 : 0x60008;
        return true;
    };
    CHECK(h.Reconcile(read) == 65538);
    CHECK(h.Confirmed() == 65538);
    CHECK(h.CanPredict());
    uint32_t local, remote;
    CHECK(h.ResolveReplay(65539, read, local, remote));
    CHECK(local == 0x60001 && remote == 0x60008);
    CHECK(h.ResolveReplay(65540, read, local, remote));
    CHECK(remote == 0x60008);
    h.Reset(131073, 0);
    CHECK(!h.CanPredict());
    CHECK(!h.Get(65537));
    std::cout << "rollback snapshot and prediction tests passed\n";
}
