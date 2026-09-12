#include "tests/test_support.hpp"
#include "core_dll/sync/FramePacing.hpp"
#include "core_dll/timing/FrameCadence.hpp"
#include "core_dll/sync/FrameSequence.hpp"
#include "core_dll/sync/BoundedWait.hpp"
#include "core_dll/sync/MatchInputBuffer.hpp"
using namespace cccaster::core::sync;
int main() {
    CC_CASE("60刻みで1秒、処理待ちでは期限を再基準化しない");
    cccaster::core::timer::FrameCadence cadence;
    cadence.Reset(100);
    for (int i = 0; i < 60; ++i)
        cadence.Advance(16666);
    CC_CHECK_EQ(cadence.NextUs(), 1000100);
    const uint64_t token = (uint64_t(65536) << 32) | 3;
    CC_CASE("進行差補正は同一世代、鮮度、方向、上限を守る");
    CC_CHECK_EQ(FramePacing::Correction(token, token, 65600, 65600, 0, 20000), 0);
    CC_CHECK(FramePacing::Correction(token, token, 65604, 65600, 0, 20000) > 0);
    CC_CHECK(FramePacing::Correction(token, token, 65600, 65604, 0, 20000) < 0);
    CC_CHECK_EQ(FramePacing::Correction(token, token + 1, 65604, 65600, 0, 20000), 0);
    CC_CHECK_EQ(FramePacing::Correction(token, token, 65604, 65600, 250001, 20000), 0);
    CC_CHECK_EQ(FramePacing::Correction(token, token, 66000, 65600, 0, 0), 500);
    FrameSequence sequence;
    CC_CASE("遅延先行と消費番号を分離し、再消費と欠番を拒否する");
    CC_CHECK(sequence.Begin(6));
    const auto first = sequence.Next();
    CC_CHECK_EQ(sequence.Capture(), first + 6);
    CC_CHECK(!sequence.Commit(first + 1));
    CC_CHECK(sequence.Commit(first));
    CC_CHECK(!sequence.Commit(first));
    CC_CHECK_EQ(sequence.Next(), first + 1);
    CC_CHECK(!sequence.Begin(9));
    CC_CHECK_EQ(sequence.Next(), first + 1);
    CC_CHECK(sequence.Begin(0));
    CC_CHECK_EQ(sequence.Next(), 2 * FrameSequence::STRIDE + 1);
    CC_CHECK_EQ(sequence.Capture(), sequence.Next());

    CC_CASE("再戦到達の証明は直後の同一画面だけ、消費番号は変更しない");
    const auto oldNext = sequence.Next();
    const auto rematchToken = (uint64_t(sequence.Base() + FrameSequence::STRIDE) << 32) | 5;
    CC_CHECK(sequence.PeerEnteredNextPhase(rematchToken, 5));
    CC_CHECK(!sequence.PeerEnteredNextPhase(rematchToken, 2));
    CC_CHECK(!sequence.PeerEnteredNextPhase((uint64_t(sequence.Base()) << 32) | 5, 5));
    CC_CHECK(!sequence.PeerEnteredNextPhase(rematchToken + (uint64_t(FrameSequence::STRIDE) << 32), 5));
    CC_CHECK_EQ(sequence.Next(), oldNext);

    CC_CASE("入力順序の入れ替わりでも未着の消費番号を飛ばさない");
    auto &b = MatchInputBuffer::GetInstance();
    b.Initialize(0, 2, 4);
    b.WriteLocal(100, 17, 0, false);
    b.WriteLocal(101, 19, 0, false);
    b.SetGameReadFrame(100);
    b.ConfirmRemote(101, 23);
    uint32_t p1 = 0, p2 = 0;
    CC_CHECK(!b.TryReadForGame(true, p1, p2));
    CC_CHECK_EQ(b.GetReadPos(), 100);
    b.ConfirmRemote(100, 29);
    CC_CHECK(b.TryReadForGame(true, p1, p2));
    CC_CHECK_EQ(p1, 17);
    CC_CHECK_EQ(p2, 29);
    b.SetGameReadFrame(101);
    CC_CHECK(b.TryReadForGame(false, p1, p2));
    CC_CHECK_EQ(p1, 23);
    CC_CHECK_EQ(p2, 19);

    CC_CASE("周回前の遅延パケットは今の入力を破壊しない");
    b.ConfirmRemote(100 + MatchInputBuffer::RING_SIZE, 31);
    b.ConfirmRemote(100, 29);
    CC_CHECK(b.TryGetRemoteInput(100 + MatchInputBuffer::RING_SIZE, p1));
    CC_CHECK_EQ(p1, 31);
    CC_CHECK(!b.TryGetRemoteInput(100, p1));

    CC_CASE("未着時はCPUを返し、到着すれば同じ番号で再開する");
    int64_t now = 0;
    int sleeps = 0;
    auto clock = [&] { return now; };
    auto sleep = [&] {
        now += 1000;
        ++sleeps;
    };
    auto result = WaitBounded(
        10000, [&] { return now >= 3000; }, [] { return false; }, [] { return true; }, clock, sleep);
    CC_CHECK_EQ(result, WaitResult::Ready);
    CC_CHECK_EQ(sleeps, 3);
    CC_CASE("相手が生存していても必要入力が来なければ期限で終わる");
    now = 0;
    sleeps = 0;
    result = WaitBounded(3000, [] { return false; }, [] { return false; }, [] { return true; }, clock, sleep);
    CC_CHECK_EQ(result, WaitResult::Timeout);
    CC_CHECK_EQ(now, 3000);
    CC_CASE("待機中の中断と切断は期限を待たない");
    now = 0;
    result = WaitBounded(
        3000000, [] { return false; }, [&] { return now == 1000; }, [] { return true; }, clock, sleep);
    CC_CHECK_EQ(result, WaitResult::Cancelled);
    CC_CHECK_EQ(now, 1000);
    now = 0;
    result = WaitBounded(
        3000000, [] { return false; }, [] { return false; }, [&] { return now < 2000; }, clock, sleep);
    CC_CHECK_EQ(result, WaitResult::Disconnected);
    CC_CHECK_EQ(now, 2000);
    return cccaster::test::Summarize("input_gate");
}
