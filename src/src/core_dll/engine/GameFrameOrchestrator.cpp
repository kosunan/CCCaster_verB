#include "core_dll/timing/FrameTiming.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/timing/SpinProbe.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include <utility>
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <cstdlib>
#include "core_dll/sync/InputTimeline.hpp"
// ============================================================================
// GameFrameOrchestrator.cpp — DxHookコールバック統合（実装）
//
// 【3つの処理】
//   1. DLLロジック実行   — OnPresent()
//   2. ImGui描画          — OnEndScene()
//   3. 高速スキップ       — OnEndScene()/OnPresentSkip()
// ============================================================================

#include <windows.h>
#include "core_dll/engine/GameFrameOrchestrator.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include "core_dll/hook/DxHook.hpp"
#include "core_dll/hook/WndProcHook.hpp"
#include "core_dll/hook/DirectInputHook.hpp"
#include "core_dll/ui/UIManager.hpp"
#include "core_dll/ui/LearningOverlay.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/sync/NetplaySession.hpp"
#include "core_dll/timing/SpeedFlags.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>
#include <cstdio>

/// デバッグログ出力関数（dllmain.cpp で定義）
void HookLog(const char *msg);

using namespace cccaster::domain::session;

// ─── ImGui 状態管理 ─────────────────────────────────
static bool s_imguiInitialized = false;

// ─── フレーム単位描画ガード ─────────────────────────
// MBAAは1Fに多数のEndSceneを呼ぶ（2026-09-11実測中央値113回、HUDを除く）。
// 最初のEndSceneでImGuiを描画しても後続のゲーム描画で上書きされるため、
// EndSceneではデータ準備のみ行い、OnPresentで最終描画する。
static bool s_imguiFrameReady = false;

// ============================================================================
// Register — DxHook にコールバックを登録
// ============================================================================
void GameFrameOrchestrator::Register() {
    cccaster::game_interface::DxHook::SetEndSceneCallback(OnEndScene);
    cccaster::game_interface::DxHook::SetPresentCallback(OnPresent);
    cccaster::game_interface::DxHook::SetAfterPresentCallback(OnAfterPresent);
    cccaster::game_interface::DxHook::SetPresentSkipCallback(OnPresentSkip);
    cccaster::game_interface::DxHook::SetPreResetCallback(OnPreReset);
    cccaster::game_interface::DxHook::SetPostResetCallback(OnPostReset);
    HookLog("[GameFrameOrchestrator] Callbacks registered to DxHook.");
}

// ============================================================================
// Shutdown — ImGui 破棄
// ============================================================================
void GameFrameOrchestrator::Shutdown() {
    cccaster::core::timer::FrameTiming::presentDueTicks = 0;
    cccaster::core::timer::FrameTiming::Simulation().Reset();
    if (s_imguiInitialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        s_imguiInitialized = false;
        HookLog("[GameFrameOrchestrator] ImGui shut down.");
    }

    // DirectInput フック解除
    cccaster::game_interface::DirectInputHook::Shutdown();
}

// ============================================================================
// OnAfterPresent — 完成画像の提示後に次フレームの入力を準備（1F1回）
// ============================================================================
//   1. SceneRunner::Step() — ゲームセッションロジック
//   2. DirectInputHook::Poll() — ジョイスティック状態取得
void GameFrameOrchestrator::OnAfterPresent(LPDIRECT3DDEVICE9 pDevice) {
    (void)pDevice;
    cccaster::core::timer::FrameTiming::releaseDueTicks = 0;
    cccaster::core::timer::FrameTiming::releaseFrame = 0;
    SceneRunner::FlushCadence();
    cccaster::diagnostics::DeferredNumericLog::Flush();
    cccaster::diagnostics::SpinProbe::Flush();
    // ネット対戦の生入力は独立時計が採取する。UIのエッジ履歴だけPresentごとに進める。
    if (!SceneRunner::IsReplaying()) {
        cccaster::game_interface::WndProcHook::PumpMessages();
        if (!cccaster::core::sync::InputTimeline::GetInstance().IsActive())
            cccaster::game_interface::DirectInputHook::Poll();
        cccaster::game_interface::DirectInputHook::PollUi();
        // 再検出は設定を行うキャラクター選択中だけ。対戦中のDirectInput列挙を避ける。
        auto &inputMem = cccaster::game_interface::GameMem();
        if (inputMem.IsAvailable() && (inputMem.GameMode() == CC_GAME_MODE_CHARA_SELECT ||
            (SceneRunner::AppMode() == 4 && inputMem.GameMode() == CC_GAME_MODE_REPLAY)))
            cccaster::game_interface::DirectInputHook::UpdateHotplug();
    }

    // SceneRunner: ゲームスレッド上で1F分のロジック処理
    if (SceneRunner::IsReady()) {
        const auto before = cccaster::core::netplay::NetplaySession::GetState().appliedFrame.load();
        SceneRunner::Step();
        using Probe = cccaster::diagnostics::SpinProbe;
        if (Probe::pending) Probe::sample.callbackEntry = Probe::Now();
        const auto after = cccaster::core::netplay::NetplaySession::GetState().appliedFrame.load();
        using Timing = cccaster::core::timer::FrameTiming;
        auto &mem = cccaster::game_interface::GameMem();
        const bool localSelect = mem.IsAvailable() && mem.GameMode() == CC_GAME_MODE_CHARA_SELECT &&
                                 cccaster::core::sync::InputTimeline::GetInstance().IsActive();
        if (mem.IsAvailable() && (mem.GameMode() == CC_GAME_MODE_IN_GAME || localSelect) && after != before) {
            // 最終ゲートを使う通常戦闘は、ゲームへ戻る時の生QPCを次Presentで取り込む。
            if (Probe::pending) Probe::sample.observeBegin = Probe::Now();
            if (!Timing::releaseDueTicks) {
                Timing::releaseUs = cccaster::platform::RealMonotonicUs();
                Timing::Simulation().Capture(Timing::releaseUs, after, false);
            }
            // observeBegin/Endは集計ではなく定数時間の標本保持区間。集計は次Present冒頭。
            if (Probe::pending) Probe::sample.observeEnd = Probe::Now();
        } else {
            Timing::releaseUs = 0;
            Timing::presentDueTicks = 0;
            if (!mem.IsAvailable() || mem.GameMode() != CC_GAME_MODE_IN_GAME)
                Timing::Simulation().Reset();
        }
    }
    if (cccaster::diagnostics::SpinProbe::pending)
        cccaster::diagnostics::SpinProbe::sample.callbackEnd = cccaster::diagnostics::SpinProbe::Now();
}

void GameFrameOrchestrator::OnPresent(LPDIRECT3DDEVICE9 pDevice) {
    // 通常更新の解放時刻は変えず、統計走査・除算・整形だけ次の待機前へ移す。
    // 再計算Presentでも一度だけ消費し、再計算そのものを標本に加えない。
    using Timing = cccaster::core::timer::FrameTiming;
    if (Timing::releasedTicks) {
        Timing::releaseUs = Timing::releasedTicks / 60;
        Timing::Simulation().Capture(Timing::releaseUs, Timing::releaseFrame, false);
        Timing::releasedTicks = 0;
    }
    auto &actual = Timing::Simulation();
    const bool simulationReport = actual.ObserveCaptured();
    static const bool simulationTrace = std::getenv("CCCASTER_FRAME_TIMING_TRACE") != nullptr;
    if (simulationTrace && simulationReport && !cccaster::diagnostics::SpinProbe::Enabled())
        DebugLog("[SimulationTiming] lastUs=%lld minUs=%lld maxUs=%lld fps=%.3f workUs=%lld",
                 actual.last, actual.minimum, actual.maximum, actual.gameFps, Timing::workUs);
    // ── ImGui 最終描画（全ゲーム描画の後、Present直前） ──
    // EndScene で準備したImGuiドローデータを、バックバッファの最上位レイヤーとして描画。
    // これによりゲームの後続描画パスに上書きされない。
    if (s_imguiFrameReady && !cccaster::core::SpeedFlags::RenderSkip().load()) {
        pDevice->BeginScene();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        pDevice->EndScene();
        s_imguiFrameReady = false;
    }
    // HUD合成もWORKへ含め、元Presentを呼ぶ直前の境界で表示要求間隔を測る。
    if (Timing::releaseUs) {
        Timing::workUs = cccaster::platform::RealMonotonicUs() - Timing::releaseUs;
        Timing::releaseUs = 0;
    }
    auto &timing = Timing::Get();
    auto &mem = cccaster::game_interface::GameMem();
    const auto presentDue = std::exchange(Timing::presentDueTicks, int64_t{0});
    const bool skipped = cccaster::core::SpeedFlags::RenderSkip().load();
    static const bool paceTrace = std::getenv("CCCASTER_PACE_TRACE") != nullptr;
    int64_t presentWait = 0, presentLate = 0, prepareUs = 0;
    static const bool reportedBudget = [] {
        DebugLog("[PaceConfig] simulationGuardUs=%lld simulationSpinGuardUs=%lld presentBudgetUs=%lld spinGuardUs=%lld",
                 Timing::SimulationGuardUs, Timing::SimulationSpinGuardUs,
                 Timing::PresentBudgetUs(), Timing::PresentSpinGuardUs);
        return true;
    }();
    (void)reportedBudget;
    if (mem.IsAvailable() && (mem.GameMode() == CC_GAME_MODE_IN_GAME ||
        (mem.GameMode() == CC_GAME_MODE_CHARA_SELECT &&
         cccaster::core::sync::InputTimeline::GetInstance().IsActive()))) {
        if (presentDue && !skipped && !SceneRunner::IsReplaying() &&
            cccaster::core::sync::InputTimeline::GetInstance().IsActive()) {
            const auto entered = paceTrace ? cccaster::platform::RealMonotonicUs() : 0;
            auto now = cccaster::core::timer::WasapiClock::GetTimeTicks();
            const auto remaining = (presentDue - now) / 60;
            prepareUs = (now - presentDue) / 60 + Timing::PresentBudgetUs();
            // 実対戦で校正した絶対締切までの残りだけ待つ。超過済みは即提示。
            // 処理後の16ms待機や、遅れたPresentを原点にした締切延長はしない。
            if (presentDue > now && remaining <= Timing::PresentBudgetUs()) {

                if (remaining > Timing::PresentSpinGuardUs)
                    cccaster::platform::PreciseWaitUs(remaining - Timing::PresentSpinGuardUs);
                // 短区間は相関QPCで待つ。音声APIや共有mutexをスピン回数だけ呼ばない。

                // 最終判定はWASAPI由来時計。補間速度差で早く提示しない。
                while ((now = cccaster::core::timer::WasapiClock::GetTimeTicks()) < presentDue)
                    cccaster::platform::CpuRelax();
            }
            if (paceTrace) {
                presentWait = cccaster::platform::RealMonotonicUs() - entered;
                presentLate = std::max(int64_t{0}, (now - presentDue) / 60);
            }
        }
        static const bool skipTrace = std::getenv("CCCASTER_CLOCK_FOLLOW_TRACE") != nullptr;
        if (skipTrace && skipped)
            DebugLog("[RenderSkip] f=%u mode=%u",
                cccaster::core::netplay::NetplaySession::GetState().appliedFrame.load(),
                unsigned(mem.GameMode()));
        const bool report =
            timing.Observe(cccaster::platform::RealMonotonicUs(),
                           cccaster::core::netplay::NetplaySession::GetState().appliedFrame.load(), skipped);
        if (paceTrace && !skipped)
            DebugLog(
                "[DisplayPace] f=%u interval=%lld work=%lld wait=%lld late=%lld prepare=%lld budget=%lld",
                cccaster::core::netplay::NetplaySession::GetState().appliedFrame.load(), timing.last,
                Timing::workUs, presentWait, presentLate, prepareUs, Timing::PresentBudgetUs());
        static const bool trace = std::getenv("CCCASTER_FRAME_TIMING_TRACE") != nullptr;
        if (trace && report)
            DebugLog("[FrameTiming] lastUs=%lld minUs=%lld maxUs=%lld displayFps=%.3f gameFps=%.3f skips=%u",
                     timing.last, timing.minimum, timing.maximum, timing.displayFps, timing.gameFps,
                     timing.Skips());
    } else {
        timing.Reset();
    }
}

// ============================================================================
// OnPresentSkip — 高速モード時は元Presentをスキップ（描画転送なし）
// ============================================================================
bool GameFrameOrchestrator::OnPresentSkip(LPDIRECT3DDEVICE9 pDevice) {
    (void)pDevice;
    return cccaster::core::SpeedFlags::RenderSkip().load(std::memory_order_acquire);
}

// ============================================================================
// OnEndScene — ImGui描画（バックバッファ時のみ）+ 高速スキップ
// ============================================================================
//
// 【高速モード】
//   RenderSkip=true → ImGui描画をスキップして即リターン
//
// 【通常モード】
//   1. ImGui 遅延初期化（初回のみ）
//   2. バックバッファ判定（オフスクリーンへの多数の描画と区別）
//   3. バックバッファ一致時のみ ImGui 描画
void GameFrameOrchestrator::OnEndScene(LPDIRECT3DDEVICE9 pDevice) {
    // ── ImGui 遅延初期化（初回のみ）──
    if (!s_imguiInitialized) {
        cccaster::diagnostics::startup::Mark("ui_begin");
        HookLog("[GameFrameOrchestrator] Initializing ImGui context...");
        D3DDEVICE_CREATION_PARAMETERS params;
        pDevice->GetCreationParameters(&params);

        char buf[128];
        snprintf(buf, sizeof(buf), "[GameFrameOrchestrator] hFocusWindow: %p", params.hFocusWindow);
        HookLog(buf);

        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        ImGuiIO &io = ImGui::GetIO();
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 14.0f);
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\tahoma.ttf", 24.0f);

        ImGui_ImplWin32_Init(params.hFocusWindow);
        ImGui_ImplDX9_Init(pDevice);

        cccaster::game_interface::WndProcHook::Initialize(params.hFocusWindow);
        cccaster::diagnostics::startup::Mark("input_begin");
        cccaster::game_interface::DirectInputHook::Initialize(params.hFocusWindow);
        cccaster::diagnostics::startup::Mark("input_end");

        s_imguiInitialized = true;
        cccaster::diagnostics::startup::Mark("ui_end");
        HookLog("[GameFrameOrchestrator] ImGui + InputHook initialized.");
    }

    // 描画スキップは入力/WndProc初期化を妨げない。
    if (cccaster::core::SpeedFlags::RenderSkip().load(std::memory_order_acquire))
        return;

    // 準備済みなら以後の描画先にかかわらず何もしない。COM照会・AddRef/Releaseも省く。
    if (s_imguiFrameReady)
        return;

    // ── バックバッファ判定 → ImGui 描画 ──
    LPDIRECT3DSURFACE9 pRenderTarget = nullptr;
    LPDIRECT3DSURFACE9 pBackBuffer = nullptr;

    bool isBackBuffer = false;
    if (SUCCEEDED(pDevice->GetRenderTarget(0, &pRenderTarget))) {
        if (SUCCEEDED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer))) {
            if (pRenderTarget == pBackBuffer) {
                isBackBuffer = true;
            }
            if (pBackBuffer)
                pBackBuffer->Release();
        }
        if (pRenderTarget)
            pRenderTarget->Release();
    }

    if (isBackBuffer) {
        // 1Fに1回だけデータ準備（MBAAは1Fに多数のEndSceneを呼ぶため）
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // GameMode → UiPhase 変換 → UIManager::Render
        auto uiPhase = cccaster::domain::ui::UiPhase::None;
        if (cccaster::game_interface::GameMem().IsAvailable()) {
            uint32_t gameMode = cccaster::game_interface::GameMem().GameMode();
            switch (gameMode) {
            case CC_GAME_MODE_CHARA_SELECT:
                uiPhase = cccaster::domain::ui::UiPhase::CharaSelect;
                break;
            case CC_GAME_MODE_RETRY:
                if (cccaster::core::netplay::NetplaySession::GetInstance().IsRunning())
                    uiPhase = cccaster::domain::ui::UiPhase::Rematch;
                break;
            case CC_GAME_MODE_IN_GAME:
                uiPhase = cccaster::domain::ui::UiPhase::InGame;
                break;
            default:
                break;
            }

            // GameMode 変化時のデバッグログ
            static uint32_t lastLoggedGameMode = 0xFFFFFFFF;
            if (gameMode != lastLoggedGameMode) {
                char buf2[128];
                snprintf(buf2, sizeof(buf2), "[GameFrameOrchestrator] GameMode changed: %u", gameMode);
                HookLog(buf2);
                lastLoggedGameMode = gameMode;
            }
        }

        // ── NetplaySession → オーバーレイ ステータス供給 ──
        if (cccaster::core::netplay::NetplaySession::GetInstance().IsRunning()) {
            auto &sync = cccaster::core::netplay::NetplaySession::GetInstance();
            float thetaMs = static_cast<float>(sync.GetThetaUs() - sync.GetBaselineTheta()) / 1000.0f;

            cccaster::domain::ui::StateUiLogic::SetFps(cccaster::core::timer::FrameTiming::Get().displayFps);
            cccaster::domain::ui::StateUiLogic::SetFrameTimeUs(
                cccaster::core::timer::FrameTiming::Get().last);
            cccaster::domain::ui::StateUiLogic::SetTimeOffsetMs(thetaMs);
        }

        try {
            cccaster::domain::ui::UIManager::Render(uiPhase);
            if (uiPhase == cccaster::domain::ui::UiPhase::InGame) {
                cccaster::domain::ui::LearningOverlay::Draw(SceneRunner::AppMode(), SceneRunner::FrameAdvantage());
                cccaster::domain::ui::LearningOverlay::DrawFrameBar(SceneRunner::AppMode(), SceneRunner::FrameBar());
            }
        } catch (...) {
            // UI描画中のクラッシュを吸収（フレームを壊さない）
        }

        ImGui::EndFrame();
        ImGui::Render();
        // RenderDrawData は OnPresent で実行（全ゲーム描画の後に最上位レイヤーとして描画）
        s_imguiFrameReady = true;
    }
}

// ============================================================================
// OnPreReset — Reset 前コールバック: ImGui D3D9 リソース解放
// ============================================================================
void GameFrameOrchestrator::OnPreReset(LPDIRECT3DDEVICE9 pDevice) {
    (void)pDevice;
    s_imguiFrameReady = false;
    cccaster::core::timer::FrameTiming::Get().Reset();
    cccaster::core::timer::FrameTiming::Simulation().Reset();
    cccaster::core::timer::FrameTiming::presentDueTicks = 0;
    cccaster::core::timer::FrameTiming::releaseUs = 0;
    cccaster::core::timer::FrameTiming::releasedTicks = 0;
    cccaster::core::timer::FrameTiming::releaseDueTicks = 0;
    if (s_imguiInitialized) {
        ImGui_ImplDX9_InvalidateDeviceObjects();
    }
}

// ============================================================================
// OnPostReset — Reset 後コールバック: ImGui D3D9 リソース再生成
// ============================================================================
void GameFrameOrchestrator::OnPostReset(LPDIRECT3DDEVICE9 pDevice) {
    (void)pDevice;
    if (s_imguiInitialized) {
        ImGui_ImplDX9_CreateDeviceObjects();
    }
}
