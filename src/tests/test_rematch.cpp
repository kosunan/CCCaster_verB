#include "test_support.hpp"
#include "core_dll/engine/RematchChoice.hpp"
#include "core_dll/engine/RetryInputGate.hpp"
#include "core_dll/sync/RetrySelection.hpp"
using namespace cccaster::domain::scene;
using namespace cccaster::game_interface;
const GameInput confirm{0, CC_BUTTON_A};
void open(RematchChoice &r) {
    for (int i = 0; i < 31; ++i)
        r.Step({}, {});
}
int main() {
    using R = cccaster::core::sync::RetrySelection;
    CC_CASE("本来のメニューの確定通知は双方ONCEとACKがそろうまで待つ");
    R a{65536, 1, 0}, b{65536, 0, 1};
    CC_CHECK_EQ(a.Result(b), -1);
    b.choice = 1;
    CC_CHECK(!a.CanRelease(b));
    a.ack = 1;
    CC_CHECK(a.CanRelease(b));
    CC_CHECK(b.CanRelease(a));
    CC_CASE("片側キャラセレは相手の選択を待たず受信確認だけを待つ");
    a = {65536, 2, 0}; b = {65536, 0, 0};
    CC_CHECK(!a.CanRelease(b));
    b.ack = 2;
    CC_CHECK(a.CanRelease(b));
    CC_CHECK(b.CanRelease(a));
    CC_CHECK_EQ(a.Result(b), 1);
    CC_CASE("通知損失・順序逆転・古い世代で確定を取り消さない");
    R received{65536, 0, 0};
    CC_CHECK(received.Accept(a));
    CC_CHECK(received.Accept({65536, 0, 0}));
    CC_CHECK_EQ(received.choice, 2u);
    CC_CHECK(!received.Accept({65536, 1, 0}));
    CC_CHECK(!received.Accept({0, 0, 0}));
    CC_CHECK(!received.Accept({65536, 3, 0}));
    CC_CHECK(!received.Accept({65537, 0, 0}));
    CC_CHECK(received.Accept({131072, 0, 0}));
    CC_CHECK_EQ(received.choice, 0u);
    CC_CHECK(!a.CanRelease(received));
    CC_CASE("カーソル移動直後と画面入口の押しっぱなしを遮断する");
    RetryInputGate gate;
    for (int i=0; i<60; ++i) CC_CHECK(gate.Apply(confirm).IsNeutral());
    gate.Apply({});
    CC_CHECK_EQ(gate.Apply(confirm).buttons, CC_BUTTON_A | CC_BUTTON_CONFIRM);
    CC_CHECK_EQ(gate.Apply({Dir::Down, CC_BUTTON_A}).buttons, 0);
    CC_CHECK_EQ(gate.Apply(confirm).buttons, 0);
    CC_CHECK_EQ(gate.Apply(confirm).buttons, 0);
    CC_CHECK_EQ(gate.Apply(confirm).buttons, CC_BUTTON_A | CC_BUTTON_CONFIRM);
    CC_CHECK_EQ(gate.Apply({0, CC_BUTTON_CONFIRM}).buttons, CC_BUTTON_CONFIRM);
    CC_CHECK_EQ(gate.Apply({0, CC_BUTTON_B | CC_BUTTON_CANCEL | CC_BUTTON_START}).buttons, 0);
    CC_CASE("両者ワンスまで待つ");
    RematchChoice r;
    open(r);
    r.Step(confirm, {});
    CC_CHECK_EQ(r.result, -1);
    for (int i = 0; i < 2000; ++i)
        r.Step(confirm, {});
    CC_CHECK_EQ(r.result, -1);
    r.Step({}, confirm);
    CC_CHECK_EQ(r.result, 0);
    CC_CASE("どちらかがキャラセレなら未決定の相手を待たない");
    for (int who = 0; who < 2; ++who) {
        r.Reset();
        open(r);
        r.Step(who == 0 ? GameInput{Dir::Down, 0} : GameInput{},
               who == 1 ? GameInput{Dir::Down, 0} : GameInput{});
        r.Step(who == 0 ? confirm : GameInput{}, who == 1 ? confirm : GameInput{});
        CC_CHECK_EQ(r.result, 1);
    }
    CC_CASE("同時選択はキャラセレ優先");
    r.Reset();
    open(r);
    r.Step({}, {Dir::Down, 0});
    r.Step(confirm, confirm);
    CC_CHECK_EQ(r.result, 1);
    CC_CASE("持ち越し入力では確定しない");
    r.Reset();
    for (int i = 0; i < 100; ++i)
        r.Step(confirm, confirm);
    CC_CHECK_EQ(r.result, -1);
    r.Step({}, {});
    r.Step(confirm, confirm);
    CC_CHECK_EQ(r.result, 0);
    CC_CASE("保存・キャンセル・スタートは選択しない");
    r.Reset();
    open(r);
    for (int i = 0; i < 50; ++i) {
        r.Step({Dir::Down, CC_BUTTON_B | CC_BUTTON_START}, {});
        r.Step({}, {});
    }
    CC_CHECK_EQ(r.players[0].cursor, 1);
    CC_CHECK_EQ(r.result, -1);
    CC_CASE("世代リセットで前回の同意を破棄");
    r.Reset();
    CC_CHECK_EQ(r.players[0].choice, -1);
    CC_CHECK_EQ(r.result, -1);
    return cccaster::test::Summarize("rematch");
}
