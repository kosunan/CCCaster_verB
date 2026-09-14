// ============================================================================
// test_phase_monitor.cpp — PhaseMonitor をゲーム無しで検証する
//
// 【意義】
//   このファイルが通ること自体が seam(B-2) の成果。
//   これまで PhaseMonitor は *CC_GAME_MODE_ADDR を直読みしていたため、
//   MBAA を起動しないと1行も検証できなかった。
//
// 【特に固定したいこと】
//   introState の意味はヘッダの doc コメントと実装で正反対だった
//   (ヘッダ: 0=イントロ前/2=完了、実装: 0=対戦中/2=演出中)。
//   実装が正しいことをテストで固定し、ドキュメントの誤りが再発しても
//   ここが赤くなって気づけるようにする。
// ============================================================================

#include "test_support.hpp"
#include "fake_game_memory.hpp"
#include "core_dll/mbaa_mem/GamePhaseDetector.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"

using cccaster::game_interface::GamePhase;
using cccaster::game_interface::PhaseMonitor;
using cccaster::test::FakeGameMemory;
using cccaster::test::ScopedGameMemory;

// ============================================================================
// seam そのもの
// ============================================================================

static void Seam_UninstalledReturnsZerosInsteadOfCrashing() {
    CC_CASE("seam: 未設置なら実アドレスを触らず 0 を返す");
    // ここでクラッシュしないこと自体が確認事項。テストバイナリには
    // RealGameMemory をリンクしていないため、直読みなら即死する。
    cccaster::game_interface::InstallGameMemory(nullptr);

    CC_CHECK(!cccaster::game_interface::GameMem().IsAvailable());
    CC_CHECK_EQ(PhaseMonitor::GetRawGameMode(), 0u);
    CC_CHECK_EQ(PhaseMonitor::GetIntroState(), 0);
}

static void Seam_InstalledFakeIsVisibleThroughPhaseMonitor() {
    CC_CASE("seam: 差し替えた値が PhaseMonitor から見える");
    FakeGameMemory fake;
    ScopedGameMemory scope(&fake);

    fake.gameMode = CC_GAME_MODE_CHARA_SELECT;
    CC_CHECK_EQ(PhaseMonitor::GetRawGameMode(), CC_GAME_MODE_CHARA_SELECT);

    fake.gameMode = CC_GAME_MODE_IN_GAME;
    CC_CHECK_EQ(PhaseMonitor::GetRawGameMode(), CC_GAME_MODE_IN_GAME);
}

// ============================================================================
// ゲームモード → GamePhase 変換
// ============================================================================

static void Phase_MapsEveryKnownGameMode() {
    CC_CASE("PhaseMonitor: 既知のゲームモードを GamePhase に写す");
    FakeGameMemory fake;
    ScopedGameMemory scope(&fake);

    struct Row {
        uint32_t mode;
        GamePhase phase;
    };
    const Row rows[] = {
        {CC_GAME_MODE_TITLE, GamePhase::MainMenu},
        {CC_GAME_MODE_STARTUP, GamePhase::MainMenu},
        {CC_GAME_MODE_CHARA_SELECT, GamePhase::CharaSelect},
        {CC_GAME_MODE_LOADING, GamePhase::Loading},
        {CC_GAME_MODE_LOADING_DEMO, GamePhase::Loading},
        {CC_GAME_MODE_IN_GAME, GamePhase::InGame},
        {CC_GAME_MODE_REPLAY, GamePhase::InGame},
        {CC_GAME_MODE_RETRY, GamePhase::Rematch},
    };

    for (const Row &r : rows) {
        fake.gameMode = r.mode;
        CC_CHECK_EQ(static_cast<int>(PhaseMonitor::GetCurrentPhase()), static_cast<int>(r.phase));
    }
}

static void Phase_UnknownModeIsUnknown() {
    CC_CASE("PhaseMonitor: 未知のモードは Unknown");
    FakeGameMemory fake;
    ScopedGameMemory scope(&fake);

    fake.gameMode = 9999;
    CC_CHECK_EQ(static_cast<int>(PhaseMonitor::GetCurrentPhase()), static_cast<int>(GamePhase::Unknown));

    // MAIN(25) はメニュー階層だが GamePhase には写されない
    fake.gameMode = CC_GAME_MODE_MAIN;
    CC_CHECK_EQ(static_cast<int>(PhaseMonitor::GetCurrentPhase()), static_cast<int>(GamePhase::Unknown));
}

static void Phase_HelpersAgreeWithGetCurrentPhase() {
    CC_CASE("PhaseMonitor: IsInXxx ヘルパは GetCurrentPhase と一致する");
    FakeGameMemory fake;
    ScopedGameMemory scope(&fake);

    fake.gameMode = CC_GAME_MODE_CHARA_SELECT;
    CC_CHECK(PhaseMonitor::IsInCharaSelect());
    CC_CHECK(!PhaseMonitor::IsInGame());

    fake.gameMode = CC_GAME_MODE_LOADING;
    CC_CHECK(PhaseMonitor::IsLoading());

    fake.gameMode = CC_GAME_MODE_RETRY;
    CC_CHECK(PhaseMonitor::IsInRematch());

    fake.gameMode = CC_GAME_MODE_TITLE;
    CC_CHECK(PhaseMonitor::IsInMainMenu());
}

// ============================================================================
// introState — 意味が逆に文書化されていた箇所
// ============================================================================

static void IntroState_TwoMeansIntroPlayingNotFinished() {
    CC_CASE("introState: 2 は「イントロ演出中」であって「完了」ではない");
    FakeGameMemory fake;
    ScopedGameMemory scope(&fake);
    fake.gameMode = CC_GAME_MODE_IN_GAME;

    fake.introState = 2; // イントロ演出中
    CC_CHECK(!PhaseMonitor::IsRoundActive());

    fake.introState = 1; // pre-game
    CC_CHECK(!PhaseMonitor::IsRoundActive());

    fake.introState = 0; // 対戦進行中
    CC_CHECK(PhaseMonitor::IsRoundActive());
}

static void IntroState_RoundActiveRequiresInGamePhase() {
    CC_CASE("introState: InGame 以外では introState=0 でも RoundActive にならない");
    FakeGameMemory fake;
    ScopedGameMemory scope(&fake);
    fake.introState = 0;

    fake.gameMode = CC_GAME_MODE_CHARA_SELECT;
    CC_CHECK(!PhaseMonitor::IsRoundActive());

    fake.gameMode = CC_GAME_MODE_RETRY;
    CC_CHECK(!PhaseMonitor::IsRoundActive());

    fake.gameMode = CC_GAME_MODE_IN_GAME;
    CC_CHECK(PhaseMonitor::IsRoundActive());
}

// ============================================================================
// 書き込みの記録（決定性テストの土台）
// ============================================================================

static void Fake_RecordsEveryWrittenInput() {
    CC_CASE("FakeGameMemory: 書き込まれた入力を全フレーム記録する");
    using GameInput = cccaster::game_interface::GameInput;
    namespace Dir = cccaster::game_interface::Dir;

    FakeGameMemory fake;
    ScopedGameMemory scope(&fake);

    auto &mem = cccaster::game_interface::GameMem();
    mem.WriteInput(GameInput{Dir::Down, 0}, GameInput{});
    mem.WriteInput(GameInput{}, GameInput{Dir::Up, CC_BUTTON_A});

    CC_CHECK_EQ(fake.written.size(), 2u);
    CC_CHECK_EQ(fake.written[0].p1.direction, Dir::Down);
    CC_CHECK_EQ(fake.written[0].p2.direction, Dir::Neutral);
    CC_CHECK_EQ(fake.written[1].p2.direction, Dir::Up);
    CC_CHECK_EQ(fake.written[1].p2.buttons, CC_BUTTON_A);
}

static void Scope_RestoresPreviousInstallation() {
    CC_CASE("ScopedGameMemory: 抜けたら未設置に戻る");
    {
        FakeGameMemory fake;
        ScopedGameMemory scope(&fake);
        fake.gameMode = CC_GAME_MODE_IN_GAME;
        CC_CHECK_EQ(PhaseMonitor::GetRawGameMode(), CC_GAME_MODE_IN_GAME);
    }
    CC_CHECK(!cccaster::game_interface::GameMem().IsAvailable());
    CC_CHECK_EQ(PhaseMonitor::GetRawGameMode(), 0u);
}

// ============================================================================

int main() {
    Seam_UninstalledReturnsZerosInsteadOfCrashing();
    Seam_InstalledFakeIsVisibleThroughPhaseMonitor();

    Phase_MapsEveryKnownGameMode();
    Phase_UnknownModeIsUnknown();
    Phase_HelpersAgreeWithGetCurrentPhase();

    IntroState_TwoMeansIntroPlayingNotFinished();
    IntroState_RoundActiveRequiresInGamePhase();

    Fake_RecordsEveryWrittenInput();
    Scope_RestoresPreviousInstallation();

    return cccaster::test::Summarize("phase_monitor");
}
