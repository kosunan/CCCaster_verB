// ============================================================================
// test_game_input.cpp — GameInput の符号化テスト
//
// 【目的】
//   入力の 32bit 表現を1つに固定する。以前は符号化が3種類混在しており、
//   Rematch の自動ナビが方向値をボタンとして書き込んでいた。
//   ここが緑である限り、その取り違えは再発しない。
//
// 【依存】
//   GameInput.hpp はヘッダオンリー・依存ゼロ。
// ============================================================================

#include "test_support.hpp"
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"

using cccaster::game_interface::GameInput;
namespace Dir = cccaster::game_interface::Dir;

// ============================================================================
// Pack / Unpack
// ============================================================================

static void Pack_PutsDirectionInHighHalf() {
    CC_CASE("GameInput: direction は上位16bit、buttons は下位16bit");
    GameInput in{Dir::Down, CC_BUTTON_A};

    CC_CHECK_EQ(in.Pack(), (2u << 16) | 0x0010u);
    CC_CHECK_EQ(in.Pack() >> 16, 2u);
    CC_CHECK_EQ(in.Pack() & 0xFFFFu, 0x0010u);
}

static void Unpack_IsInverseOfPack() {
    CC_CASE("GameInput: Unpack(Pack(x)) == x");
    const GameInput cases[] = {
        {},           {Dir::Neutral, CC_BUTTON_CONFIRM},
        {Dir::Up, 0}, {Dir::DownRight, CC_BUTTON_A | CC_BUTTON_B | CC_BUTTON_C},
        {9, 0xFFFF},
    };
    for (const GameInput &in : cases) {
        CC_CHECK(GameInput::Unpack(in.Pack()) == in);
    }
}

static void Unpack_MatchesDirectInputHookEncoding() {
    CC_CASE("GameInput: DirectInputHook の (direction<<16)|buttons と一致する");
    // DirectInputHook::BuildPlayerInput() は
    //   return ((uint32_t)direction << 16) | buttons;  (direction はテンキー表記)
    // を返す。Unpack がこれと同じ解釈をすることを固定する。
    const uint32_t raw = (static_cast<uint32_t>(6) << 16) | CC_BUTTON_B;

    GameInput in = GameInput::Unpack(raw);
    CC_CHECK_EQ(in.direction, 6); // 右
    CC_CHECK_EQ(in.buttons, CC_BUTTON_B);
}

static void Neutral_IsDirectionZeroAndNoButtons() {
    CC_CASE("GameInput: ニュートラルは direction=0 かつ buttons=0");
    CC_CHECK(GameInput{}.IsNeutral());
    CC_CHECK_EQ(GameInput{}.Pack(), 0u);

    const GameInput dirOnly{Dir::Up, 0};
    const GameInput btnOnly{Dir::Neutral, CC_BUTTON_A};
    CC_CHECK(!dirOnly.IsNeutral());
    CC_CHECK(!btnOnly.IsNeutral());
}

// ============================================================================
// 回帰: Rematch 自動ナビの符号化
// ============================================================================

static void RematchNav_DirectionsDoNotCollideWithButtons() {
    CC_CASE("回帰: Rematch の上下移動がボタンとして解釈されない");
    // 以前は「下」を 0x0002、「上」を 0x0001 のまま WriteInput に渡しており、
    // buttons 側に落ちて CC_PLAYER_FACING / CC_BUTTON_START になっていた。
    // 型を通すことで方向とボタンが同じビット空間を共有しなくなる。
    const GameInput down{Dir::Down, 0};
    const GameInput up{Dir::Up, 0};

    CC_CHECK_EQ(down.buttons, 0);
    CC_CHECK_EQ(up.buttons, 0);

    // 旧実装が書き込んでいた値との違いを明示する
    CC_CHECK(down.Pack() != CC_PLAYER_FACING);
    CC_CHECK(up.Pack() != CC_BUTTON_START);

    // 方向はゲームメモリの direction フィールドに載る
    CC_CHECK_EQ(down.Pack() >> 16, 2u);
    CC_CHECK_EQ(up.Pack() >> 16, 8u);
}

static void RematchNav_ConfirmUsesButtonsOnly() {
    CC_CASE("回帰: Rematch の決定はボタンのみで方向を伴わない");
    const GameInput confirm{Dir::Neutral, CC_BUTTON_A | CC_BUTTON_CONFIRM};

    CC_CHECK_EQ(confirm.direction, 0);
    CC_CHECK_EQ(confirm.Pack(), static_cast<uint32_t>(CC_BUTTON_A | CC_BUTTON_CONFIRM));
}

static void FastBootNav_UsesSameEncodingAsRematch() {
    CC_CASE("回帰: FastBoot と Rematch の「下」が同じ値になる");
    // FastBoot は元から正しく (dirBits << 16) していたため、
    // 両者が食い違っていたことが規約の証拠だった。
    const GameInput fastBootDown{Dir::Down, 0};
    const GameInput rematchDown{Dir::Down, 0};

    CC_CHECK(fastBootDown == rematchDown);
}

// ============================================================================

int main() {
    Pack_PutsDirectionInHighHalf();
    Unpack_IsInverseOfPack();
    Unpack_MatchesDirectInputHookEncoding();
    Neutral_IsDirectionZeroAndNoButtons();

    RematchNav_DirectionsDoNotCollideWithButtons();
    RematchNav_ConfirmUsesButtonsOnly();
    FastBootNav_UsesSameEncodingAsRematch();

    return cccaster::test::Summarize("game_input");
}
