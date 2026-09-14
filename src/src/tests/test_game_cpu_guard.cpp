#include "core_dll/timing/GameCpuGuard.hpp"
#include "test_support.hpp"
#ifdef _WIN32
#include "core_dll/timing/GameCoreLease.hpp"
#include <thread>
#endif
int main() {
    using cccaster::core::timer::GameCpuMask;
    CC_CASE("CPU0の物理コアを除外しSMT相手も同時に外す");
    CC_CHECK(GameCpuMask(0xffff,3,8,16,1)==0xfffc);
    CC_CHECK(GameCpuMask(0xffff,0x101,8,16,1)==0xfefe);
    CC_CASE("許可範囲を拡張せず利用可能CPUがなければ無変更");
    CC_CHECK(GameCpuMask(0x35,3,8,16,1)==0x34);
    CC_CHECK(GameCpuMask(1,3,8,16,1)==0);
    CC_CHECK(GameCpuMask(0xc,3,8,16,1)==0xc);
    CC_CASE("未対応の多グループや小規模構成は既存配置を維持");
    CC_CHECK(GameCpuMask(0xffff,3,8,64,1)==0);
    CC_CHECK(GameCpuMask(0xffff,3,8,16,2)==0);
    CC_CHECK(GameCpuMask(0xf,3,2,4,1)==0);
    CC_CHECK(GameCpuMask(0xffff,0,8,16,1)==0);
    using cccaster::core::timer::ParseGameCpuPin;
    using cccaster::core::timer::GameCpuPinMask;
    CC_CASE("診断用CPU番号は0から31の十進数字だけ受理");
    CC_CHECK(ParseGameCpuPin(nullptr)==-1);
    CC_CHECK(ParseGameCpuPin("")==-1);
    CC_CHECK(ParseGameCpuPin("0")==0);
    CC_CHECK(ParseGameCpuPin("2")==2);
    CC_CHECK(ParseGameCpuPin("31")==31);
    CC_CHECK(ParseGameCpuPin("02")==2);
    CC_CHECK(ParseGameCpuPin("32")==-1);
    CC_CHECK(ParseGameCpuPin("-1")==-1);
    CC_CHECK(ParseGameCpuPin("+2")==-1);
    CC_CHECK(ParseGameCpuPin(" 2")==-1);
    CC_CHECK(ParseGameCpuPin("2 ")==-1);
    CC_CHECK(ParseGameCpuPin("2x")==-1);
    CC_CHECK(ParseGameCpuPin("0x2")==-1);
    CC_CHECK(ParseGameCpuPin("999999999999999999999999999999")==-1);
    CC_CASE("CPU固定は既存許可とCPU0除外を広げない");
    CC_CHECK(GameCpuPinMask(0xfffc,2)==4);
    CC_CHECK(GameCpuPinMask(0xfffc,4)==16);
    CC_CHECK(GameCpuPinMask(0xfffc,0)==0xfffc);
    CC_CHECK(GameCpuPinMask(0xfffc,1)==0xfffc);
    CC_CHECK(GameCpuPinMask(0xc,4)==0xc);
    CC_CHECK(GameCpuPinMask(0xfffc,-1)==0xfffc);
    CC_CHECK(GameCpuPinMask(0xfffc,32)==0xfffc);
    CC_CHECK(GameCpuPinMask(0,2)==0);
    CC_CHECK(GameCpuPinMask(0x80000004u,31)==0x80000000u);
#ifdef _WIN32
    CC_CASE("コア利用権は別スレッドと競合し解放後に取得できる");
    auto lease = cccaster::platform::AcquireGameCore(0x80000000u);
    CC_CHECK(lease != nullptr);
    bool acquired = false;
    auto attempt = [&] {
        auto other = cccaster::platform::AcquireGameCore(0x80000000u);
        acquired = other != nullptr;
        cccaster::platform::ReleaseGameCore(other);
    };
    std::thread(attempt).join();
    CC_CHECK(!acquired);
    cccaster::platform::ReleaseGameCore(lease);
    std::thread(attempt).join();
    CC_CHECK(acquired);
#endif
    return cccaster::test::Summarize("game_cpu_guard");
}
