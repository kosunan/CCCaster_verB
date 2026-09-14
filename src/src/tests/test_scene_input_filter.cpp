#include "core_dll/engine/LocalInputGate.hpp"
// ============================================================================
// test_scene_input_filter.cpp — 画面ごとの入力制約のテスト
//
// 【意義】
//   証言①「キャラセレがたまにズレる」は通信のずれではなく、同じ入力列から
//   両者が違う結果を出すことで起きる。この制約が壊れると再発する。
//
// 【依存】
//   GamePhase(enum) と GameInput のみ。ゲームメモリを触らないので L1 で回る。
// ============================================================================

#include "test_support.hpp"
#include "core_dll/engine/SceneInputFilter.hpp"
#include "core_dll/mbaa_mem/GameInput.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"

using cccaster::domain::scene::SceneInputFilter;
using cccaster::game_interface::GameInput;
using cccaster::game_interface::GamePhase;
namespace Dir = cccaster::game_interface::Dir;

namespace {

/// 1フレーム分の入力を通す
GameInput Feed(GamePhase phase, uint16_t dir, uint16_t btn) {
    const GameInput in{dir, btn};
    return GameInput::Unpack(SceneInputFilter::Apply(phase, in.Pack()));
}

/// ニュートラルで n フレーム進める
void Idle(GamePhase phase, int n) {
    for (int i = 0; i < n; ++i)
        Feed(phase, Dir::Neutral, 0);
}

} // namespace

// ============================================================================
// InGame — メニューへ抜けるボタンを通さない
// ============================================================================

static void InGame_StripsMenuEscapeButtons() {
    CC_CASE("InGame: START/FN1/FN2 を落とす");
    SceneInputFilter::Reset();

    const GameInput out =
        Feed(GamePhase::InGame, Dir::Down, CC_BUTTON_START | CC_BUTTON_FN1 | CC_BUTTON_FN2 | CC_BUTTON_A);

    CC_CHECK_EQ(out.buttons, CC_BUTTON_A); // A は残る
    CC_CHECK_EQ(out.direction, Dir::Down); // 方向は触らない
}

static void InGame_KeepsAttackButtons() {
    CC_CASE("InGame: 攻撃ボタンはそのまま通す");
    SceneInputFilter::Reset();

    const uint16_t all = CC_BUTTON_A | CC_BUTTON_B | CC_BUTTON_C | CC_BUTTON_D;
    const GameInput out = Feed(GamePhase::InGame, Dir::DownRight, all);

    CC_CHECK_EQ(out.buttons, all);
    CC_CHECK_EQ(out.direction, Dir::DownRight);
}

static void InGame_DoesNotSealConfirmAfterDirectionChange() {
    CC_CASE("InGame: 方向転換直後でも攻撃を封印しない");
    // 封印はメニュー系画面だけの制約。対戦中に効いたら操作不能になる。
    SceneInputFilter::Reset();

    Feed(GamePhase::InGame, Dir::Right, 0);
    const GameInput out = Feed(GamePhase::InGame, Dir::Left, CC_BUTTON_A);

    CC_CHECK_EQ(out.buttons, CC_BUTTON_A);
}

// ============================================================================
// CharaSelect — カーソル移動直後の封印
// ============================================================================

static void CharaSelect_SealsConfirmRightAfterCursorMove() {
    CC_CASE("CharaSelect: カーソル移動と同一フレームの決定を落とす");
    SceneInputFilter::Reset();
    Idle(GamePhase::CharaSelect, 10);

    // 移動と決定が同じフレームに乗ったケース
    const GameInput out = Feed(GamePhase::CharaSelect, Dir::Right, CC_BUTTON_A);

    CC_CHECK_EQ(out.buttons, 0);            // 決定は落ちる
    CC_CHECK_EQ(out.direction, Dir::Right); // 移動は通る
}

static void CharaSelect_SealLastsForConfiguredFrames() {
    CC_CASE("CharaSelect: 封印は DIR_SEAL_FRAMES 続き、その後は通る");
    SceneInputFilter::Reset();
    Idle(GamePhase::CharaSelect, 10);

    Feed(GamePhase::CharaSelect, Dir::Right, 0); // 移動（封印開始）

    // 封印中
    for (uint32_t i = 1; i < SceneInputFilter::DIR_SEAL_FRAMES; ++i) {
        const GameInput sealed = Feed(GamePhase::CharaSelect, Dir::Right, CC_BUTTON_A);
        CC_CHECK_EQ(sealed.buttons, 0);
    }

    // 封印明け
    const GameInput out = Feed(GamePhase::CharaSelect, Dir::Right, CC_BUTTON_A);
    CC_CHECK_EQ(out.buttons, CC_BUTTON_A);
}

static void CharaSelect_SealsCancelToo() {
    CC_CASE("CharaSelect: 移動直後はキャンセルも落とす");
    SceneInputFilter::Reset();
    Idle(GamePhase::CharaSelect, 10);

    const GameInput out = Feed(GamePhase::CharaSelect, Dir::Up, CC_BUTTON_B | CC_BUTTON_CANCEL);
    CC_CHECK_EQ(out.buttons, 0);
}

static void CharaSelect_ReturningToNeutralIsNotAMove() {
    CC_CASE("CharaSelect: ニュートラルへ戻すのは移動ではない");
    // レバーを離しただけで決定が消えると、実操作で決定できなくなる。
    SceneInputFilter::Reset();
    Idle(GamePhase::CharaSelect, 10);

    Feed(GamePhase::CharaSelect, Dir::Right, 0);
    Idle(GamePhase::CharaSelect, 5); // 封印は明けている

    const GameInput out = Feed(GamePhase::CharaSelect, Dir::Neutral, CC_BUTTON_A);
    CC_CHECK_EQ(out.buttons, CC_BUTTON_A);
}

static void CharaSelect_HoldingSameDirectionDoesNotReseal() {
    CC_CASE("CharaSelect: 同じ方向を押し続けても封印し直さない");
    SceneInputFilter::Reset();
    Idle(GamePhase::CharaSelect, 10);

    Feed(GamePhase::CharaSelect, Dir::Right, 0);
    Idle(GamePhase::CharaSelect, 0);
    for (uint32_t i = 0; i < SceneInputFilter::DIR_SEAL_FRAMES; ++i) {
        Feed(GamePhase::CharaSelect, Dir::Right, 0); // 押しっぱなし
    }

    const GameInput out = Feed(GamePhase::CharaSelect, Dir::Right, CC_BUTTON_A);
    CC_CHECK_EQ(out.buttons, CC_BUTTON_A);
}

// ============================================================================
// CharaSelect — 決定の連打ガード
// ============================================================================

static void CharaSelect_DropsRapidRepeatedConfirm() {
    CC_CASE("CharaSelect: ガード中の連続決定を落とす");
    // 高速連打で項目を飛ばすのを防ぐ。1フレームの到着差で
    // 決定回数が変わると両者の結果が食い違う。
    SceneInputFilter::Reset();
    Idle(GamePhase::CharaSelect, 10);

    const GameInput first = Feed(GamePhase::CharaSelect, Dir::Neutral, CC_BUTTON_A);
    CC_CHECK_EQ(first.buttons, CC_BUTTON_A); // 1回目は通る

    for (uint32_t i = 1; i < SceneInputFilter::CONFIRM_GUARD_FRAMES; ++i) {
        const GameInput dropped = Feed(GamePhase::CharaSelect, Dir::Neutral, CC_BUTTON_A);
        CC_CHECK_EQ(dropped.buttons, 0);
    }
}

static void CharaSelect_AllowsConfirmAfterGuardExpires() {
    CC_CASE("CharaSelect: ガードが明ければ次の決定は通る");
    SceneInputFilter::Reset();
    Idle(GamePhase::CharaSelect, 10);

    Feed(GamePhase::CharaSelect, Dir::Neutral, CC_BUTTON_A);
    Idle(GamePhase::CharaSelect, SceneInputFilter::CONFIRM_GUARD_FRAMES);

    const GameInput out = Feed(GamePhase::CharaSelect, Dir::Neutral, CC_BUTTON_A);
    CC_CHECK_EQ(out.buttons, CC_BUTTON_A);
}

static void CharaSelect_ConfirmViaConfirmButtonIsGuardedToo() {
    CC_CASE("CharaSelect: CONFIRM ボタン単体でもガードが効く");
    SceneInputFilter::Reset();
    Idle(GamePhase::CharaSelect, 10);

    const GameInput first = Feed(GamePhase::CharaSelect, Dir::Neutral, CC_BUTTON_CONFIRM);
    CC_CHECK_EQ(first.buttons, CC_BUTTON_CONFIRM);

    const GameInput second = Feed(GamePhase::CharaSelect, Dir::Neutral, CC_BUTTON_CONFIRM);
    CC_CHECK_EQ(second.buttons, 0);
}

// ============================================================================
// 状態管理
// ============================================================================

static void Rematch_UsesSameConstraintsAsCharaSelect() {
    CC_CASE("Rematch: キャラセレと同じ制約が効く");
    SceneInputFilter::Reset();
    Idle(GamePhase::Rematch, 10);

    const GameInput out = Feed(GamePhase::Rematch, Dir::Down, CC_BUTTON_A);
    CC_CHECK_EQ(out.buttons, 0);
}

static void OtherPhases_PassThrough() {
    CC_CASE("Loading / MainMenu は素通し");
    SceneInputFilter::Reset();

    const uint16_t btn = CC_BUTTON_A | CC_BUTTON_START;
    CC_CHECK_EQ(Feed(GamePhase::Loading, Dir::Up, btn).buttons, btn);
    CC_CHECK_EQ(Feed(GamePhase::MainMenu, Dir::Up, btn).buttons, btn);
}

static void Reset_ClearsHistory() {
    CC_CASE("Reset: 履歴を捨てるので封印もガードも解ける");
    SceneInputFilter::Reset();
    Idle(GamePhase::CharaSelect, 10);

    Feed(GamePhase::CharaSelect, Dir::Right, 0); // 封印中にする
    Feed(GamePhase::CharaSelect, Dir::Neutral, CC_BUTTON_A);

    SceneInputFilter::Reset();

    const GameInput out = Feed(GamePhase::CharaSelect, Dir::Neutral, CC_BUTTON_A);
    CC_CHECK_EQ(out.buttons, CC_BUTTON_A);
}

static void Filter_IsPureFunctionOfPhaseAndHistory() {
    CC_CASE("同じ履歴・同じ入力なら結果は必ず一致する");
    // 両者が同じ値を受け取るための最低条件。ここが崩れると決定性が壊れる。
    const uint16_t seq[][2] = {
        {Dir::Right, 0},
        {Dir::Right, CC_BUTTON_A},
        {Dir::Neutral, CC_BUTTON_A},
        {Dir::Left, CC_BUTTON_CONFIRM},
        {Dir::Neutral, 0},
        {Dir::Neutral, CC_BUTTON_A},
    };

    uint32_t firstRun[6] = {};
    SceneInputFilter::Reset();
    for (int i = 0; i < 6; ++i) {
        firstRun[i] = Feed(GamePhase::CharaSelect, seq[i][0], seq[i][1]).Pack();
    }

    SceneInputFilter::Reset();
    for (int i = 0; i < 6; ++i) {
        const uint32_t again = Feed(GamePhase::CharaSelect, seq[i][0], seq[i][1]).Pack();
        CC_CHECK_EQ(again, firstRun[i]);
    }
}

// ============================================================================

static void MappingWindow_BlocksInputUntilReleased() {
    CC_CASE("設定画面では方向とボタンを遮断し、閉じた後は解放を待つ");
    cccaster::domain::scene::LocalInputGate gate;
    const GameInput pressed{Dir::Down, CC_BUTTON_A | CC_BUTTON_CONFIRM};
    CC_CHECK(gate.Apply(pressed, false) == pressed);
    CC_CHECK(gate.Apply(pressed, true).IsNeutral());
    CC_CHECK(gate.Apply({}, true).IsNeutral());
    CC_CHECK(gate.Apply(pressed, false).IsNeutral());
    CC_CHECK(gate.Apply({Dir::Down, 0}, false).IsNeutral());
    CC_CHECK(gate.Apply({}, false).IsNeutral());
    CC_CHECK(gate.Apply(pressed, false) == pressed);
}

int main() {
    MappingWindow_BlocksInputUntilReleased();
    InGame_StripsMenuEscapeButtons();
    InGame_KeepsAttackButtons();
    InGame_DoesNotSealConfirmAfterDirectionChange();

    CharaSelect_SealsConfirmRightAfterCursorMove();
    CharaSelect_SealLastsForConfiguredFrames();
    CharaSelect_SealsCancelToo();
    CharaSelect_ReturningToNeutralIsNotAMove();
    CharaSelect_HoldingSameDirectionDoesNotReseal();

    CharaSelect_DropsRapidRepeatedConfirm();
    CharaSelect_AllowsConfirmAfterGuardExpires();
    CharaSelect_ConfirmViaConfirmButtonIsGuardedToo();

    Rematch_UsesSameConstraintsAsCharaSelect();
    OtherPhases_PassThrough();
    Reset_ClearsHistory();
    Filter_IsPureFunctionOfPhaseAndHistory();

    return cccaster::test::Summarize("scene_input_filter");
}
