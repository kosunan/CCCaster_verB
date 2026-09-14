#include "test_support.hpp"
#include "core_dll/timing/ClockContinuity.hpp"
#include <cstdlib>
#include <thread>
#include "core_dll/timing/FrameCadence.hpp"
using cccaster::core::timer::ClockContinuity;

int main() {
    CC_CASE("60Hzを整数µsに丸めず、60Fで正確に1秒進める");
    cccaster::core::timer::FrameCadence cadence;
    cadence.ResetTicks(17);
    for (int i = 1; i <= 360000; ++i) {
        cadence.Advance(16666);
        CC_CHECK_EQ(cadence.NextTicks(), 17 + int64_t(i) * 1000000);
    }
    for (int scale : {2, 3, 4, 16, 64}) {
        cadence.ResetTicks(17);
        for (int i = 0; i < 60 * scale; ++i)
            cadence.Advance(16666 / scale, scale);
        CC_CHECK_EQ(cadence.NextTicks(), 60000017);
    }
    CC_CASE("公開途中の時計読取りは待たず、完全な旧/新モデルだけを使う");
    cccaster::core::timer::ClockProjection published;
    published.Publish({100, 200, 0, 0});
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int64_t i = 101; i < 50000; ++i)
            published.Publish({i, i * 2, 0, 0});
        done = true;
    });
    cccaster::core::timer::ClockAnchor cached;
    do {
        published.Read(cached);
        CC_CHECK_EQ(cached.time, cached.qpc * 2);
        CC_CHECK_EQ(cached.AtTicks(cached.qpc + 7), cached.time + 7);
    } while (!done.load());
    writer.join();

    CC_CASE("音声調律も1/60µsで計算しµs未満を捨てない");
    ClockContinuity fine(60);
    CC_CHECK_EQ(fine.Read(60000007, 6000000), 60000007);
    CC_CHECK_EQ(fine.Read(60000013, 6000006), 60000013);
    CC_CHECK_EQ(fine.Anchor().AtTicks(60000019), 60000019);
    for (int64_t t = 600000; t <= 600000000; t += 600000)
        fine.Read(60000007 + t, 6000000 + t + t / 10000);
    CC_CHECK(!fine.IsFallback());
    CC_CHECK(std::abs(fine.Anchor().AtTicks(660000007) - (660000007 + 60000)) <= 6600);
    CC_CASE("音声の開始と10msの調律境界では出力がジャンプしない");
    ClockContinuity boundary;
    CC_CHECK_EQ(boundary.Read(1000000, 0), 1000000);
    CC_CHECK_EQ(boundary.Read(1001000, 10000), 1001000);
    CC_CHECK_EQ(boundary.Read(1011000, 20150), 1011000);
    CC_CHECK_EQ(boundary.Read(1011000, 20000), 1011000);
    CC_CHECK(!boundary.IsFallback());

    CC_CASE("±150usの標本ノイズを60Hzの出力段差に変えない");
    ClockContinuity noisy;
    int64_t previous = noisy.Read(1000000, 100000);
    for (int i = 1; i <= 600; ++i) {
        const int64_t elapsed = int64_t(i) * 16667;
        const int64_t noise = i % 2 ? 150 : -150;
        const auto current = noisy.Read(1000000 + elapsed, 100000 + elapsed + noise);
        CC_CHECK(std::abs(current - previous - 16667) <= 4);
        previous = current;
    }
    CC_CHECK(!noisy.IsFallback());

    CC_CASE("1usのスピン読みと100usの読みで補正が増幅・消失しない");
    ClockContinuity dense, sparse;
    dense.Read(1000000, 100000);
    sparse.Read(1000000, 100000);
    for (int64_t t = 1; t <= 500000; ++t) {
        // 同じ10ms境界の標本を両側が観測する。呼出し回数だけを変える。
        const int64_t audio = 100000 + t + (t / 10000 % 2 ? 150 : -150);
        const auto d = dense.Read(1000000 + t, audio);
        if (t % 100 == 0)
            CC_CHECK(std::abs(d - sparse.Read(1000000 + t, audio)) <= 1);
    }
    CC_CHECK(!dense.IsFallback() && !sparse.IsFallback());

    CC_CASE("持続する±100ppmの音声速度を追いQPC固定動作にはしない");
    for (int sign : {-1, 1}) {
        ClockContinuity drift;
        const auto start = drift.Read(1000000, 100000);
        int64_t atNine = 0, end = 0;
        for (int64_t t = 10000; t <= 10000000; t += 10000) {
            end = drift.Read(1000000 + t, 100000 + t + sign * (t / 10000));
            if (t == 9000000)
                atNine = end;
        }
        CC_CHECK(!drift.IsFallback());
        CC_CHECK(std::abs((end - atNine) - (1000000 + sign * 100)) <= 3);
        CC_CHECK(std::abs((end - start) - (10000000 + sign * 1000)) <= 110);
    }

    CC_CASE("出力の平滑化で生音声の250ms/1%超の速度異常を隠さない");
    for (int sign : {-1, 1}) {
        ClockContinuity fault;
        fault.Read(1000000, 100000);
        fault.Read(1249000, 100000 + 249000 + sign * 4980);
        CC_CHECK(!fault.IsFallback());
        const auto failed = fault.Read(1250000, 100000 + 250000 + sign * 5000);
        CC_CHECK(fault.IsFallback());
        CC_CHECK_EQ(fault.Read(2250000, 90000000) - failed, 1000000);
    }

    CC_CASE("調律上限を超える持続残差も明示fallbackにし音声追従を装わない");
    ClockContinuity excessive;
    excessive.Read(1000000, 100000);
    for (int64_t t = 10000; t <= 4000000; t += 10000)
        excessive.Read(1000000 + t, 100000 + t + t / 200); // 0.5%: 生1%監視内。
    CC_CHECK(excessive.IsFallback());

    CC_CASE("音声の欠測は連続的かつ固定のQPC切替になる");
    ClockContinuity missing;
    missing.Read(1000000, 100000);
    missing.Read(1010000, 110150);
    const auto lost = missing.Read(1020000, 0);
    CC_CHECK(missing.IsFallback());
    CC_CHECK_EQ(missing.Read(2020000, 90000000) - lost, 1000000);
    CC_CASE("ゲーム高速時計はworkerの1ms待ちなしでQPCを1000倍投影する");
    cccaster::core::timer::ClockAnchor game{10000000, 10000000, 0, 999000000};
    CC_CHECK_EQ(game.AtTicks(10000001), 10001000);
    CC_CHECK_EQ(game.AtTicks(10000100), 10100000);
    CC_CHECK_EQ(game.AtTicks(10010000), 20000000);
    const auto switchAt = 10012345;
    cccaster::core::timer::ClockAnchor normal{switchAt, game.AtTicks(switchAt), 0, 0};
    CC_CHECK_EQ(normal.AtTicks(switchAt), game.AtTicks(switchAt));
    CC_CHECK_EQ(normal.AtTicks(switchAt + 10000000) - normal.time, 10000000);
    return cccaster::test::Summarize("clock_continuity");
}
