#include "core_dll/sync/NetplaySession.hpp"
#include "tests/test_support.hpp"
#include "core_dll/sync/InputTimeline.hpp"
#include "core_dll/sync/MatchInputBuffer.hpp"
#include "core_dll/hook/DirectInputHook.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
namespace {
uint32_t physical = 0;
bool mapping = false;
unsigned polls = 0;
} // namespace
namespace cccaster::game_interface {
void DirectInputHook::Poll() {
    ++polls;
}
uint32_t DirectInputHook::GetLocalPlayerInput(bool, bool) {
    return physical;
}
} // namespace cccaster::game_interface
namespace cccaster::domain::ui {
bool StateUiLogic::IsMappingWindowOpen() {
    return mapping;
}
} // namespace cccaster::domain::ui
void HookLog(const char *) {}
namespace cccaster::platform {
int64_t RealMonotonicUs() {
    return 0;
}
int64_t RealMonotonicTicks() {
    return 0;
}
} // namespace cccaster::platform
using namespace cccaster::core::sync;
int main() {
    CC_CASE("未来の開始時刻より前に入力を採取しない");
    auto &future = InputTimeline::GetInstance();
    future.Reset();
    future.Begin(65536, 65539, cccaster::game_interface::GamePhase::InGame, true, 60000000);
    future.Pump(999999, 16666, 59999940);
    CC_CHECK(!future.HasCaptured(65539));
    future.Pump(1000000, 16666, 60000000);
    CC_CHECK(future.HasCaptured(65539));
    CC_CHECK_EQ(future.CapturedDeadlineTicks(65539), 60000000);

    CC_CASE("共通開始は提案・受領・確定を経て異なる時計原点へ変換する");
    EpochStartGate host, client;
    host.Begin(65536); client.Begin(65536);
    CC_CHECK(host.Update(true, client.local, 0, (1000000LL*60), (50000LL*60)) == EpochStartGate::Waiting);
    const auto offer = host.local;
    CC_CHECK_EQ(offer.hostTicks, (1500000LL*60));
    CC_CHECK(client.Update(false, offer, offer.hostTicks + (20000LL*60), (1030000LL*60), (50000LL*60)) == EpochStartGate::Waiting);
    CC_CHECK_EQ(client.local.stage, EpochStart::Accepted);
    host.Update(true, client.local, 0, (1040000LL*60), (50000LL*60));
    CC_CHECK_EQ(host.local.stage, EpochStart::Commit);
    CC_CHECK(client.Update(false, host.local, 999, (1070000LL*60), (50000LL*60)) == EpochStartGate::Armed);
    CC_CHECK_EQ(client.dueTicks, host.dueTicks + (20000LL*60)); // 受領後の推定変動で再計算しない。
    CC_CHECK(host.Update(true, client.local, 0, (1080000LL*60), (50000LL*60)) == EpochStartGate::Armed);
    CC_CHECK(!offer.NewerThan(host.local));
    auto corrupt = host.local; corrupt.hostTicks++;
    CC_CHECK(!corrupt.NewerThan(host.local));

    CC_CASE("提案喪失時は再提案、確定後の遅着は開始せず失敗する");
    host.Begin(131072); client.Begin(131072);
    host.Update(true, client.local, 0, (1000000LL*60), (50000LL*60));
    const auto stale = host.local;
    host.Update(true, client.local, 0, (1490000LL*60), (50000LL*60));
    CC_CHECK(host.local.serial > stale.serial);
    client.Update(false, host.local, host.dueTicks, (1500000LL*60), (50000LL*60));
    CC_CHECK(!stale.NewerThan(client.local));
    host.Update(true, client.local, 0, (1510000LL*60), (50000LL*60));
    CC_CHECK(client.Update(false, host.local, host.dueTicks, host.dueTicks, (50000LL*60)) == EpochStartGate::Expired);
    CC_CHECK(host.Update(true, client.local, 0, host.dueTicks, (50000LL*60)) == EpochStartGate::Expired);

    polls = 0;
    auto &timeline = InputTimeline::GetInstance();
    auto &buffer = MatchInputBuffer::GetInstance();
    buffer.Initialize(0, 2, 4);
    timeline.Reset();
    timeline.Begin(65536, 65539, cccaster::game_interface::GamePhase::InGame, true);
    physical = 16;
    timeline.Pump(0, 16666);
    uint32_t input = 0;
    CC_CASE("ゲームが進まなくても独立した締切で入力を蓄える");
    CC_CHECK(timeline.HasCaptured(65539));
    physical = 2;
    timeline.Pump(16665, 16666);
    CC_CHECK_EQ(polls, 1);
    timeline.Pump(16666, 16666);
    CC_CHECK_EQ(polls, 1);
    timeline.Pump(16666, 16666, 1000000);
    CC_CHECK_EQ(polls, 2);
    CC_CHECK(buffer.TryGetLocalInput(65539, input));
    CC_CHECK_EQ(input, 16);
    CC_CHECK(buffer.TryGetLocalInput(65540, input));
    CC_CHECK_EQ(input, 2);
    CC_CASE("採取スレッドの遅延は以前の値を維持し、最新枠だけ実測する");
    physical = 4;
    timeline.Pump(66666, 16666, 4000000);
    CC_CHECK(buffer.TryGetLocalInput(65541, input));
    CC_CHECK_EQ(input, 2);
    CC_CHECK(buffer.TryGetLocalInput(65542, input));
    CC_CHECK_EQ(input, 2);
    CC_CHECK(buffer.TryGetLocalInput(65543, input));
    CC_CHECK_EQ(input, 4);
    CC_CHECK_EQ(polls, 3);
    CC_CASE("採取の遅延をゲーム更新の予定時刻へ持ち越さない");
    CC_CHECK_EQ(timeline.CapturedDeadlineUs(65541), 33334);
    CC_CHECK_EQ(timeline.CapturedDeadlineUs(65542), 50000);
    CC_CHECK_EQ(timeline.CapturedDeadlineUs(65543), 66667);
    CC_CHECK_EQ(timeline.CapturedDeadlineUs(99999), 0);
    CC_CASE("F4設定中と閉じた後の押しっぱなしを公開前に遮断する");
    mapping = true;
    timeline.Pump(83333, 16666, 5000000);
    CC_CHECK(buffer.TryGetLocalInput(65544, input));
    CC_CHECK_EQ(input, 0);
    mapping = false;
    timeline.Pump(100000, 16666);
    CC_CHECK(buffer.TryGetLocalInput(65545, input));
    CC_CHECK_EQ(input, 0);
    physical = 0;
    timeline.Pump(116666, 16666, 7000000);
    physical = 8;
    timeline.Pump(133333, 16666, 8000000);
    CC_CHECK(buffer.TryGetLocalInput(65547, input));
    CC_CHECK_EQ(input, 8);
    CC_CASE("境界停止中は採取せず、未消費枠を周回上書きしない");
    timeline.Pause();
    timeline.Pump(150000, 16666);
    CC_CHECK(!timeline.HasCaptured(65548));
    timeline.Resume();
    timeline.Pump(20000000, 16666);
    CC_CHECK(timeline.HasOverflowed());
    CC_CHECK(buffer.TryGetLocalInput(65539, input));
    CC_CHECK_EQ(input, 16);
    CC_CASE("受信時刻の更新では公開済み締切を動かさない");
    timeline.Reset(); buffer.Initialize(0,2,4);
    timeline.Begin(65536,65539,cccaster::game_interface::GamePhase::InGame,false,60000000);
    auto &state = cccaster::core::netplay::NetplaySession::GetMutableState();
    {
        std::lock_guard lock(state.scheduleMutex);
        state.peerSchedule = {65536,65539,60000000+6000,59999999,0,60000,0,true,1};
    }
    timeline.PumpTicks(59999999,0);
    CC_CHECK_EQ(timeline.NextDeadlineTicks(),60000000);
    timeline.PumpTicks(60000000,0);
    CC_CHECK_EQ(timeline.CapturedDeadlineTicks(65539),60000000);
    CC_CHECK(timeline.NextDeadlineTicks()>61000000);
    CC_CHECK(timeline.NextDeadlineTicks()<=61000001);
    const auto held = timeline.NextDeadlineTicks();
    {
        std::lock_guard lock(state.scheduleMutex);
        state.peerSchedule.dueTicks += 60000;
        state.peerSchedule.stampTicks++;
    }
    timeline.PumpTicks(60000100,0);
    CC_CHECK_EQ(timeline.NextDeadlineTicks(),held);
    CC_CASE("別世代と未学習の時計から通常位相補正を受けない");
    timeline.Reset(); timeline.Begin(65536,65539,cccaster::game_interface::GamePhase::InGame,false,60000000);
    state.peerSchedule.modelReady=false;
    timeline.PumpTicks(60000000,0);
    CC_CHECK_EQ(timeline.NextDeadlineTicks(),61000000);
    state.peerSchedule.base=131072; state.peerSchedule.modelReady=true;
    timeline.PumpTicks(61000000,0);
    CC_CHECK_EQ(timeline.NextDeadlineTicks(),62000000);
    timeline.Pause();
    using namespace cccaster::core::timer;
    CC_CASE("比例補正は正負対称・最大0.25µs/Fで端数も収束させる");
    for (const int sign : {-1,1}) {
        PhaseFollower follower;
        FrameCadence cadence; cadence.ResetTicks(0);
        int64_t error=sign*60000, previous=0;
        for (uint32_t frame=0;frame<15000;++frame) {
            const auto parts=follower.UpdateParts(error,frame,true);
            CC_CHECK(std::abs(parts)<=15*ClockParts);
            CC_CHECK(std::abs(parts-previous)<=ClockParts);
            CC_CHECK_EQ(follower.UpdateParts(error,frame,true),0);
            const auto before=cadence.NextTicks();
            cadence.AdvanceCorrected(parts);
            error-=cadence.NextTicks()-before-ClockFrame;
            CC_CHECK(sign*error>=0);
            previous=parts;
        }
        CC_CHECK(std::abs(error)<=1);
    }
    CC_CASE("1tick未満の補正も捨てず、反転と観測失効でも補正量を滑らかに変える");
    PhaseFollower follower;
    CC_CHECK_EQ(follower.UpdateParts(1,1,true),ClockParts/600);
    CC_CHECK_EQ(follower.UpdateParts(-1,2,true),-ClockParts/600);
    CC_CHECK_EQ(follower.UpdateParts(-60000,3,false),0);
    follower.Reset();
    int64_t last=0;
    for(uint32_t f=0;f<120;++f) {
        const auto parts=follower.UpdateParts(f<30?600000:-600000,f,f<90);
        CC_CHECK(std::abs(parts-last)<=ClockParts);
        last=parts;
    }
    CC_CHECK_EQ(last,0);
    CC_CASE("1F以上の復帰は新しい同方向観測2つを要求し重複適用しない");
    follower.Reset();
    CC_CHECK_EQ(follower.RecoveryTicks(-3000000,10,1),0);
    CC_CHECK_EQ(follower.RecoveryTicks(-3000000,10,1),0);
    CC_CHECK_EQ(follower.RecoveryTicks(-3000000,10,2),-2985000);
    CC_CHECK_EQ(follower.RecoveryTicks(-3000000,10,3),0);
    return cccaster::test::Summarize("input_timeline");
}

namespace cccaster::core::netplay {
NetplaySession &NetplaySession::GetInstance() {
    static NetplaySession s;
    return s;
}
void NetplaySession::Stop() {}
} // namespace cccaster::core::netplay
