#include "test_support.hpp"
#include "../../InputInjector.hpp"
int main() {
    CC_CASE("テンキー方向はORではなく9方向として合成する");
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            ControllerState c{};
            c.isPressedLeft = x < 0;
            c.isPressedRight = x > 0;
            c.isPressedUp = y > 0;
            c.isPressedDown = y < 0;
            CC_CHECK_EQ(toGameInput(c).direction, (x || y) ? 5 + x + 3 * y : 0);
        }
    CC_CASE("反対方向は相殺し、Eと他のボタンを失わない");
    ControllerState c{};
    c.isPressedLeft = c.isPressedRight = c.isPressedUp = c.isPressedDown = true;
    c.isPressedA = c.isPressedB = c.isPressedC = c.isPressedD = c.isPressedE = true;
    auto v = toGameInput(c);
    CC_CHECK_EQ(v.direction, 0);
    CC_CHECK_EQ(v.buttons, CC_BUTTON_A | CC_BUTTON_B | CC_BUTTON_C | CC_BUTTON_D | CC_BUTTON_E);
    CC_CHECK_EQ(toGameInput(c).Pack(), v.Pack()); // 同じHoldからPushを捏造しない。
    return cccaster::test::Summarize("input_conversion");
}
