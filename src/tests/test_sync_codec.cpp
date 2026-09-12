#include "tests/test_support.hpp"
#include "shared_contracts/SessionClosePacket.hpp"
#include "core_dll/network/SyncCodec.hpp"
#include "core_dll/sync/MatchInputBuffer.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"

// 通信・ゲーム・UIを起動せず、本物の符号化/復号経路を検査する。
namespace cccaster::core::netplay {
NetplaySession &NetplaySession::GetInstance() {
    static NetplaySession s;
    return s;
}
void NetplaySession::Stop() {}
} // namespace cccaster::core::netplay
void HookLog(const char *) {}
namespace cccaster::domain::ui {
void StateUiLogic::SetDelay(int) {}
void StateUiLogic::SetRollback(int) {}
} // namespace cccaster::domain::ui

namespace cccaster::core::timer { extern int64_t testClockTicks; extern uint32_t testClockSource; }
using cccaster::core::netplay::SyncCodec;
using cccaster::core::sync::MatchInputBuffer;
static void Receive(SyncCodec &codec, const std::vector<uint8_t> &packet) {
    codec.ProcessReceivedPacket(packet, "127.0.0.1", 7600, 1000000);
}
static void Expire(SyncCodec &codec) {
    for (int i = 0; i < SyncCodec::DISCONNECT_TIMEOUT_FRAMES; ++i)
        codec.IncrementFrameCount();
}

int main() {
    auto &buf = MatchInputBuffer::GetInstance();
    SyncCodec codec;
    codec.Initialize(true, 2, 4, nullptr);

    CC_CASE("初回送信は未生成の履歴を確定しない");
    buf.Initialize(200, 2, 4);
    buf.WriteLocal(201, 17, 0, false);
    Receive(codec, codec.BuildPacket(201, 17));
    uint32_t input = 0;
    CC_CHECK(buf.TryGetRemoteInput(201, input));
    CC_CHECK_EQ(input, 17);
    CC_CHECK(!buf.HasRemote(200));
    CC_CHECK(!buf.HasRemote(192));

    CC_CASE("キープアライブは未生成の先頭入力を捏造しない");
    buf.Initialize(200, 2, 4);
    Receive(codec, codec.BuildPacket(200, 0));
    CC_CHECK(!buf.HasConfirmedRemote());

    CC_CASE("欠落履歴より古い入力は連続列として送らない");
    buf.Reset();
    buf.WriteLocal(299, 99, 0, false);
    buf.WriteLocal(301, 31, 0, false);
    Receive(codec, codec.BuildPacket(301, 31));
    CC_CHECK(buf.HasRemote(301));
    CC_CHECK(!buf.HasRemote(300));
    CC_CHECK(!buf.HasRemote(299));

    CC_CASE("先頭パケットを失っても後続の冗長履歴で復元する");
    buf.Reset();
    for (uint32_t f = 401; f <= 410; ++f)
        buf.WriteLocal(f, f == 405 ? 0 : f * 3, 0, false);
    const auto redundant = codec.BuildPacket(410, 999);
    Receive(codec, redundant);
    Receive(codec, redundant);
    for (uint32_t f = 401; f <= 410; ++f) {
        CC_CHECK(buf.TryGetRemoteInput(f, input));
        CC_CHECK_EQ(input, f == 405 ? 0 : f * 3);
    }
    CC_CHECK_EQ(buf.ConfirmConflicts(), 0);
    CC_CHECK(!buf.HasRemote(400));

    CC_CASE("不正パケットでは疎通タイムアウトを解除しない");
    const auto valid = codec.BuildPacket(0, 0, true);
    std::vector<std::vector<uint8_t>> invalid;
    invalid.push_back({});
    auto bad = valid;
    bad[0] ^= 0xff;
    invalid.push_back(bad);
    bad = valid;
    bad[5] = 0xff;
    invalid.push_back(bad);
    bad = valid;
    bad.pop_back();
    invalid.push_back(bad);
    // wire仕様: ヘッダ20B + NTP24B + baseFrame4B の次がinputCount。
    bad = valid;
    bad[48] = 11;
    invalid.push_back(bad);
    bad = valid;
    bad[48] = 1;
    invalid.push_back(bad); // baseFrame=0 と矛盾
    for (const auto &packet : invalid) {
        codec.Reset();
        Expire(codec);
        CC_CHECK(!codec.IsPeerAlive());
        Receive(codec, packet);
        CC_CHECK(!codec.IsPeerAlive());
        CC_CHECK(!codec.IsPeerReady());
    }
    Receive(codec, valid);
    CC_CHECK(codec.IsPeerAlive());
    CC_CHECK(codec.IsPeerReady());
    CC_CASE("プレイヤー名を固定長ペイロードで交換する");
    auto &shared = cccaster::core::netplay::NetplaySession::GetMutableState();
    std::snprintf(shared.localPlayerName, sizeof(shared.localPlayerName), "%s", "HOST NAME");
    Receive(codec, codec.BuildPacket(0, 0, true));
    CC_CHECK(std::strcmp(shared.peerPlayerName, "HOST NAME") == 0);
    CC_CASE("乱数は該当世代のホスト状態を転送し、古い再送で戻さない");
    shared.localPhaseToken.store((uint64_t(65536) << 32) | 2);
    shared.localSeedEpoch = 65536;
    for (size_t i = 0; i < shared.localSeed.size(); ++i)
        shared.localSeed[i] = uint32_t(i * 17 + 9);
    codec.Initialize(true, 2, 4, nullptr);
    auto seedPacket = codec.BuildPacket(0, 0, true);
    SyncCodec client;
    client.Initialize(false, 2, 4, nullptr);
    Receive(client, seedPacket);
    CC_CHECK_EQ(shared.peerSeedEpoch, 65536);
    CC_CHECK(shared.peerSeed == shared.localSeed);
    shared.localPhaseToken.store((uint64_t(131072) << 32) | 4);
    shared.localSeedEpoch = 131072;
    shared.localSeed[0] = 42;
    Receive(client, codec.BuildPacket(0, 0, true));
    Receive(client, seedPacket);
    CC_CHECK_EQ(shared.peerSeedEpoch, 131072);
    CC_CHECK_EQ(shared.peerSeed[0], 42);
    CC_CHECK_EQ(shared.peerPhaseToken.load() >> 32, 131072);
    CC_CASE("消費ACKは古い再送で後退しない");
    shared.consumedFrame.store(131100);
    auto ack = codec.BuildPacket(0, 0, true);
    Receive(client, ack);
    CC_CHECK_EQ(shared.peerConsumedFrame.load(), 131100);
    shared.consumedFrame.store(131099);
    Receive(client, codec.BuildPacket(0, 0, true));
    CC_CHECK_EQ(shared.peerConsumedFrame.load(), 131100);
    CC_CASE("異なるwire版を検出し、セッション中の遅延変更を拒否する");
    shared.protocolError.store(false);
    ack[6] = 0;
    Receive(client, ack);
    CC_CHECK(shared.protocolError.load());
    codec.SetDelayFrames(8);
    codec.SetMaxRollback(0);
    CC_CHECK_EQ(codec.GetDelayFrames(), 2);
    CC_CHECK_EQ(codec.GetMaxRollback(), 4);
    CC_CASE("時計が先行しても未消費入力を再送窓から落とさない");
    shared.localPhaseToken.store((uint64_t(196608) << 32) | 3);
    shared.peerConsumedFrame.store(196608);
    buf.Reset();
    for (uint32_t f = 196609; f <= 196708; ++f)
        buf.WriteLocal(f, f, 0, false);
    Receive(codec, codec.BuildPacket(196708, 0));
    CC_CHECK(buf.HasRemote(196609));
    CC_CHECK(buf.HasRemote(196618));
    CC_CHECK(!buf.HasRemote(196708));
    CC_CASE("消費ACKが遅れても最新入力と未消費先頭を同時に配送できる");
    CC_CHECK_EQ(codec.RepairWindowEnd(196708), 196618);
    Receive(codec, codec.BuildPacket(196708, 0, false, 0, true));
    CC_CHECK(buf.HasRemote(196708));
    CC_CHECK(buf.HasRemote(196699));
    CC_CHECK(buf.HasRemote(196609));
    CC_CHECK(!buf.HasRemote(196650)); // 中間欠落を架空の入力で埋めない。
    shared.peerConsumedFrame.store(196618);
    Receive(codec, codec.BuildPacket(196708, 0));
    CC_CHECK(buf.HasRemote(196619));
    CC_CHECK(buf.HasRemote(196628));
    CC_CASE("終了通知は完全な形式だけ受理し遅延入力で復活しない");
    auto close = cccaster::public_api::BuildSessionClosePacket();
    auto shortClose = close;
    shortClose.pop_back();
    Receive(codec, shortClose);
    CC_CHECK(!codec.IsPeerClosed());
    auto extraClose = close;
    extraClose.push_back(0);
    Receive(codec, extraClose);
    CC_CHECK(!codec.IsPeerClosed());
    Receive(codec, close);
    CC_CHECK(codec.IsPeerClosed());
    CC_CHECK(!codec.IsPeerAlive());
    Receive(codec, valid);
    CC_CHECK(!codec.IsPeerAlive());
    codec.Reset();
    CC_CHECK(!codec.IsPeerClosed());
    CC_CASE("キャラセレは入力履歴を送らず確定状態を損失後も再送する");
    using cccaster::core::sync::SelectionState;
    shared.localPhaseToken = (uint64_t(262144) << 32) | 2;
    shared.localSelection = {};
    shared.localSelection.epoch = 262144;
    shared.localSelection.revision = 2;
    shared.localSelection.confirmed = 1;
    shared.localSelection.character = 11;
    shared.localSelection.selector = 14;
    shared.localSelection.color = 15;
    auto selection = codec.BuildPacket(262145, 0);
    Receive(codec, selection);
    CC_CHECK_EQ(shared.peerSelection.character, 11u);
    CC_CHECK(!buf.HasRemote(262145));
    auto previous = selection;
    shared.localSelection.stageConfirmed = 1;
    shared.localSelection.stage = 7;
    shared.localSelection.revision = 3;
    selection = codec.BuildPacket(262145, 0);
    // 1回目を失った後の同じ状態の再送だけで回復する。
    Receive(codec, selection);
    Receive(codec, previous);
    CC_CHECK_EQ(shared.peerSelection.stage, 7u);
    CC_CHECK_EQ(shared.peerSelection.revision, 3u);
    CC_CASE("ランダム指定0は未確定時だけ許可し、確定通知には実ステージを要求する");
    auto unresolved = shared.localSelection;
    unresolved.stage = 0;
    CC_CHECK(!unresolved.Valid());
    CC_CHECK(!shared.peerSelection.Accept(unresolved));
    CC_CHECK_EQ(shared.peerSelection.stage, 7u);
    unresolved.stageConfirmed = 0;
    CC_CHECK(unresolved.Valid());
    unresolved.stageConfirmed = 1;
    unresolved.stage = 99;
    CC_CHECK(unresolved.Valid());
    unresolved.stage = 100;
    CC_CHECK(!unresolved.Valid());
    CC_CASE("最終確定は同じ世代の相手確認を必要とする");
    SelectionState local = shared.localSelection, peer = shared.peerSelection;
    CC_CHECK(!local.PeerHasFinal(peer));
    peer.ack = local.revision;
    CC_CHECK(local.PeerHasFinal(peer));
    peer.epoch -= 65536;
    CC_CHECK(!local.PeerHasFinal(peer));
    peer = shared.peerSelection;
    peer.moon = 3;
    CC_CHECK(!local.Accept(peer));
    peer = shared.peerSelection;
    peer.character = 100;
    CC_CHECK(!local.Accept(peer));
    CC_CHECK_EQ(SelectionState::CharacterCell(51), 4);
    CC_CASE("同じ確定の再送で受信確認は後退しない");
    local = shared.localSelection;
    local.ack = 3;
    peer = local;
    peer.ack = 1;
    CC_CHECK(local.Accept(peer));
    CC_CHECK_EQ(local.ack, 3u);
    CC_CASE("再戦は確定項目を再送し、相手が再戦へ入るまで旧戦闘末尾を保持する");
    buf.Reset();
    shared.protocolError = false;
    shared.localPhaseToken = (uint64_t(327680) << 32) | 5;
    shared.peerPhaseBaseFrame = 262144;
    shared.retryPreviousFrame = 262200;
    shared.localRetry = {327680, 1, 0};
    shared.peerRetry = {};
    buf.WriteLocal(262200, 77, 0, false);
    buf.WriteLocal(327681, 99, 0, false);
    auto retryPending = codec.BuildPacket(327681, 0);
    Receive(codec, retryPending);
    CC_CHECK_EQ(shared.peerRetry.choice, 1u);
    CC_CHECK(buf.TryGetRemoteInput(262200, input));
    CC_CHECK_EQ(input, 77u);
    CC_CHECK(!buf.HasRemote(327681));
    shared.peerPhaseBaseFrame = 327680;
    shared.localRetry.ack = 2;
    auto retryAck = codec.BuildPacket(327681, 0);
    Receive(codec, retryAck);
    Receive(codec, retryPending);
    CC_CHECK_EQ(shared.peerRetry.ack, 2u);
    CC_CHECK(!buf.HasRemote(327681));
    // 次画面に移っても直前の選択とACKを再送し、相手を取り残さない。
    shared.localPhaseToken = (uint64_t(393216) << 32) | 2;
    Receive(codec, codec.BuildPacket(0, 0));
    CC_CHECK_EQ(shared.peerRetry.epoch, 327680u);
    CC_CHECK_EQ(shared.peerRetry.ack, 2u);
    retryAck[7] = 2;
    Receive(codec, retryAck);
    CC_CHECK(shared.protocolError);
    CC_CASE("旧キャラセレ入力同期DLLとの混在を拒否する");
    selection[7] = 0;
    shared.protocolError = false;
    Receive(codec, selection);
    CC_CHECK(shared.protocolError);
    CC_CASE("イントロ・決着後の旧同期方式との混在を拒否する");
    selection[7] = 4;
    shared.protocolError = false;
    Receive(codec, selection);
    CC_CHECK(shared.protocolError);
    CC_CASE("送信バッファ再利用で旧フラグ・予約領域・入力を残さず従来wireと一致する");
    std::vector<uint8_t> reusable(512, 0xa5);
    const auto *storage = reusable.data();
    for (unsigned i = 0; i < 20; ++i) {
        const bool latest = (i & 1) != 0, ready = (i & 2) != 0;
        const auto frame = (i & 4) ? 0u : 262200u;
        const auto expected = codec.BuildPacket(frame, 0, ready, i * 1000, latest);
        std::fill(reusable.begin(), reusable.end(), uint8_t{0xa5});
        codec.BuildPacketInto(reusable, frame, 0, ready, i * 1000, latest);
        CC_CHECK(reusable == expected);
        CC_CHECK(reusable.data() == storage);
    }
    CC_CASE("wire 10拡張6は1/60µsの端数を送信し旧µs単位を拒否する");
    cccaster::core::timer::testClockTicks=60000007;
    auto tickPacket=codec.BuildPacket(0,0,true);
    int64_t headerTime=0,ntpTime=0;
    std::memcpy(&headerTime,tickPacket.data()+8,8);
    std::memcpy(&ntpTime,tickPacket.data()+20,8);
    CC_CHECK_EQ(headerTime,60000007);
    CC_CHECK_EQ(ntpTime,60000007);
    CC_CHECK_EQ(tickPacket[7],6);
    tickPacket[7]=5;
    shared.protocolError=false;
    Receive(codec,tickPacket);
    CC_CHECK(shared.protocolError);
    cccaster::core::timer::testClockTicks=0;
    CC_CASE("音声時計切替で推定を破棄し旧世代と切替前エコーで復活させない");
    codec.Reset();
    auto sourcePacket = codec.BuildPacket(0, 0);
    int64_t sampleNow = 60000000;
    const auto feedClock = [&](uint32_t generation, bool staleEcho = false) {
        sampleNow += 1000000;
        const int64_t t1 = staleEcho ? 60000000 : sampleNow - 1000;
        const int64_t t2 = sampleNow - 500, t3 = sampleNow - 500;
        std::memcpy(sourcePacket.data()+20, &t3, 8);
        std::memcpy(sourcePacket.data()+28, &t1, 8);
        std::memcpy(sourcePacket.data()+36, &t2, 8);
        std::memcpy(sourcePacket.data()+sourcePacket.size()-4, &generation, 4);
        codec.ProcessReceivedPacket(sourcePacket, "127.0.0.1", 7600, sampleNow);
    };
    for (int i=0;i<12;++i) feedClock(0);
    CC_CHECK(codec.HasTimingEstimate());
    shared.peerSchedule = {65536, 65537, sampleNow, sampleNow, 0, 0, 0, true, 1};
    feedClock(1);
    CC_CHECK(!codec.HasTimingEstimate());
    CC_CHECK_EQ(shared.peerSchedule.frame, 0u);
    for (int i=0;i<12;++i) feedClock(0);
    CC_CHECK(!codec.HasTimingEstimate());
    for (int i=0;i<12;++i) feedClock(1, true);
    CC_CHECK(!codec.HasTimingEstimate());
    for (int i=0;i<12;++i) feedClock(1);
    CC_CHECK(codec.HasTimingEstimate());
    cccaster::core::timer::testClockSource=1;
    feedClock(1);
    CC_CHECK(!codec.HasTimingEstimate());
    for (int i=0;i<12;++i) feedClock(1);
    CC_CHECK(codec.HasTimingEstimate());
    CC_CASE("異常な64bit時刻は差分演算より前に拒否する");
    const auto serial = codec.GetRttSampleSerial();
    const int64_t invalidTime = INT64_MAX;
    std::memcpy(sourcePacket.data()+20, &invalidTime, 8);
    codec.ProcessReceivedPacket(sourcePacket, "127.0.0.1", 7600, sampleNow);
    CC_CHECK_EQ(codec.GetRttSampleSerial(), serial);
    cccaster::core::timer::testClockSource=0;
    return cccaster::test::Summarize("sync_codec");
}
