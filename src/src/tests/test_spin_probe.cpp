#include "core_dll/timing/SpinProbe.hpp"
#include "test_support.hpp"
#include <string>
#include <vector>
std::vector<std::string> lines;
void HookLog(const char *s) { lines.emplace_back(s); }
int main() {
    using namespace cccaster::diagnostics;
    CC_CASE("監視の空白を読取り中とループ間へ分解する");
    SpinMetrics m;
    m.Observe(100, 110, 900, 1000);
    m.Observe(140, 160, 950, 1000);
    m.Observe(161, 170, 1001, 1000);
    CC_CHECK(m.maxGap == 50 && m.gapRead == 20 && m.gapBetween == 30);
    CC_CHECK(m.maxGapBegin == 110 && m.maxGapEnd == 160);
    CC_CHECK(m.gapEndLate == -50 && m.exitLate == 1);
    CC_CHECK(m.exitGap == 10 && m.exitRead == 9 && m.exitBetween == 1);
    CC_CHECK(m.reads == 3 && m.maxRead == 20 && m.maxBetween == 30);
    CC_CASE("時計採取後の中断が次の外側空白から隠れる場合を分離する");
    SpinClockMetrics clock;
    clock.Observe(100, 900, 240, 1000, 0, 0, 0);
    clock.Observe(245, 1045, 250, 1000, 0, 0, 0);
    CC_CHECK(clock.innerGap == 145 && clock.audioGap == 145);
    CC_CHECK(clock.previousAge == 140 && clock.lastAge == 5);
    CC_CHECK(clock.innerGap == (250 - 240) + clock.previousAge - clock.lastAge);
    CC_CHECK(clock.maxAge == 140 && clock.ageLate == -100);
    CC_CHECK(clock.maxAgeBegin == 100 && clock.maxAgeEnd == 240);
    CC_CHECK(clock.maxShift == 0 && clock.maxClamp == 0);
    CC_CASE("整形を遅延し、変更前の値と符号を保持する");
    DeferredNumericLog::Prepare();
    int signedValue = -17;
    DeferredNumericLog::Log("%d %u %lld", signedValue, uint32_t{0xffffffff}, int64_t{-1234567890123});
    signedValue = 99;
    CC_CHECK(lines.empty());
    DeferredNumericLog::Flush();
    CC_CHECK(lines.size() == 1 && lines[0] == "-17 4294967295 -1234567890123");
    CC_CASE("上限を超えても上書きせず欠落数を後から記録する");
    lines.clear();
    DeferredNumericLog::active = true;
    for (unsigned i = 0; i < 35; ++i) DeferredNumericLog::Log("%u", i);
    CC_CHECK(lines.empty() && DeferredNumericLog::count == 32 && DeferredNumericLog::dropped == 3);
    DeferredNumericLog::Flush();
    CC_CHECK(lines.size() == 33 && lines[31] == "31" && lines[32] == "[DeferredTraceDropped] count=3");
    CC_CASE("入力待ちは整形せず固定長採取し短い待機と溢れを区別する");
    lines.clear();
    SpinProbe::RecordInputWait(10,1,100,101);
    CC_CHECK(SpinProbe::inputWaitCount == 0);
    for (unsigned i=0;i<9;++i) SpinProbe::RecordInputWait(10+i,1,100,60100);
    CC_CHECK(lines.empty() && SpinProbe::inputWaitCount == 8 && SpinProbe::inputWaitDropped == 1);
    CC_CHECK(SpinProbe::inputWaits[7].frame == 17);
    return cccaster::test::Summarize("spin_probe");
}
