#include "test_support.hpp"
#include "core_dll/sync/SettingsCommands.hpp"
using S = cccaster::core::sync::SettingsCommands;
int main() {
    CC_CASE("設定操作は入力へ一度だけ載り、確定するまで次の要求を待つ");
    S::Reset(2, 4);
    CC_CHECK(S::Request(false, 3));
    CC_CHECK(!S::Request(true, 2));
    auto command = S::Capture();
    CC_CHECK(command != 0);
    CC_CHECK_EQ(S::Capture(), 0u);
    CC_CHECK(S::pending.load() != 0);
    CC_CHECK(S::Apply(command | 0x00060010u, true));
    CC_CHECK_EQ(S::delay.load(), 3);
    CC_CHECK_EQ(S::pending.load(), 0u);
    CC_CHECK_EQ((command | 0x00060010u) & S::GameMask, 0x00060010u);
    CC_CASE("合計超過を拒否し、P1とP2の同時操作を一定順に処理する");
    CC_CHECK(S::Request(true, 8));
    S::Apply(S::Capture(), true);
    CC_CHECK_EQ(S::rollback.load(), 4);
    S::Apply(0xa4000000u, false);
    S::Apply(0xb2000000u, true);
    CC_CHECK_EQ(S::delay.load(), 4);
    CC_CHECK_EQ(S::rollback.load(), 2);
    CC_CHECK(!S::Request(false, 9));
    CC_CASE("境界まで届かなかった操作を持ち越さない");
    S::Request(false, 1);
    S::Boundary();
    CC_CHECK_EQ(S::Capture(), 0u);
    CC_CHECK_EQ(S::pending.load(), 0u);
    return cccaster::test::Summarize("settings_commands");
}
