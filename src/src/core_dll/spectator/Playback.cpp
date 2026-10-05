#include "core_dll/spectator/Playback.hpp"
#include "core_dll/spectator/PlaybackPacing.hpp"
#include "core_dll/engine/FrameControl.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/timing/IdlePresentation.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "shared_contracts/IpcData.hpp"
#ifdef _WIN32
#include "core_dll/hook/WndProcHook.hpp"
#endif

namespace cccaster::spectator {
using Phase = game_interface::GamePhase;
using Control = domain::session::FrameControl;
void Playback::PrepareDrawing(Phase phase) {
    // Start/確定入力の到着待ちへ入る前に、ロード後最初の画像を提示する。
    // 2試合以上前の戦闘再生など、通常の追いつき描画方針は維持する。
    if (phase == Phase::InGame && (stageRematch_ || previous_ == Phase::Loading)) {
        Control::WriteInput({}, {});
        Control::SetModeNormalSpeed();
    }
    if ((!stageRematch_ && (phase == Phase::CharaSelect || phase == Phase::Loading)) ||
        IsIntroPlayback(phase, game_interface::GameMem().IntroState()))
        core::SpeedFlags::RenderSkip().store(false, std::memory_order_release);
}
void Playback::Pace(bool fast, bool visible) {
    // RANDOM ONCEの内部選び直しだけは隠す。イントロの先頭はPrepareDrawingで復帰する。
    const bool skipDrawing = fast && (!visible || stageRematch_);
    if (fast != catching_ || skipDrawing != drawingSkipped_)
        domain::session::DebugLog("[Spectator] PACE fast=%u draw=%u frame=%u match=%u latestMatch=%u",
            unsigned(fast), unsigned(!skipDrawing), last_, match.score.revision, Transport::Get().LatestMatch());
    if (fast != catching_) pace_.Stop();
    catching_ = fast;
    drawingSkipped_ = skipDrawing;
    if (fast) {
        // V10共通の高速スキップ。処理予算・倍率上限・追加待機を設けない。
        Control::SetModeHighSpeedSkip();
        if (!skipDrawing) core::SpeedFlags::RenderSkip().store(false, std::memory_order_release);
        return;
    }
    Control::SetModeNormalSpeed();
    if (!pace_.IsRunning()) pace_.Start();
    pace_.WaitForNextTick(false);
}
bool Playback::Select(bool release) {
    auto &mem = game_interface::GameMem();
    if (!selecting_ || !mem.SetSpectatorRules({match.roundsToWin, match.damageLevel, match.timerSpeed})) return false;
    const auto p1 = mem.DriveRemoteSelection(false, match.p1, ++nav_);
    const auto p2 = mem.DriveRemoteSelection(true, match.p2, nav_);
    mem.SetSelectionRelease(release, match.p1.stage);
    Control::WriteInput(p1, p2);
    // 通常選択は描画ON。RANDOM ONCEの内部選択はPaceで描画OFFにする。
    Pace(stageRematch_ || release);
    return true;
}
bool Playback::Step(Phase phase) {
    auto &wire = Transport::Get();
    auto &mem = game_interface::GameMem();
    if (introPreview_.Pending()) {
        // このStepは先頭画像をPresentした後。同期する状態を更新前へ戻す。
        if (!introPreview_.Restore(mem)) return false;
        domain::session::DebugLog("[Spectator] INTRO WAIT wt=%u qpc=%lld", mem.WorldTimer(), platform::RealMonotonicUs());
    }
    if (wire.State() == Status::Failed || wire.State() == Status::Disconnected || wire.State() == Status::Overflow)
        return false;
    if (phase != previous_) {
        if (previous_ == Phase::Loading)
            domain::session::DebugLog("[Spectator] LOAD END frames=%u elapsedUs=%lld",
                loadingFrames_, platform::RealMonotonicUs() - loadingStarted_);
        if (phase == Phase::Loading) {
            loadingFrames_ = 0;
            loadingStarted_ = platform::RealMonotonicUs();
            domain::session::DebugLog("[Spectator] LOAD BEGIN qpc=%lld", loadingStarted_);
        }
        if (phase != Phase::InGame && phase != Phase::Rematch) ending_ = false;
        if (phase == Phase::CharaSelect) {
            if (!mem.BeginIndependentSelect(false)) return false;
            selecting_ = true;
            selectionPrepared_ = false;
            nav_ = 0;
            domain::session::DebugLog("[Spectator] SELECT ENTER qpc=%lld", platform::RealMonotonicUs());
        }
        if (phase == Phase::Rematch) { mem.BeginIndependentRetry(); retrying_ = false; }
        if (phase == Phase::InGame) {
            if (!mem.SetStageRematchFastPath(false)) return false;
            stageRematch_ = false;
            domain::session::DebugLog("[Spectator] INTRO ENTER qpc=%lld", platform::RealMonotonicUs());
        }
        previous_ = phase;
        if (phase == Phase::InGame) {
            if (!introPreview_.Begin(mem)) return false;
            domain::session::DebugLog("[Spectator] INTRO PREVIEW wt=%u qpc=%lld", mem.WorldTimer(), platform::RealMonotonicUs());
            Pace(true);
            return true;
        }
    }
    if (phase == Phase::Loading) {
        ++loadingFrames_;
        // 通知の有無によらず、元ゲームのロード完了判定を通して演出を自動決定。
        // InGameではこの入力を送らず、最初の画像を提示してStart・確定入力を待つ。
        const auto confirm = LoadingConfirm(phase, loadingFrames_);
        Control::WriteInput(confirm, confirm);
        Pace(true);
        return true;
    }
    if (retrying_ && phase == Phase::Rematch) {
        game_interface::GameInput confirm{};
        if (++nav_ % 8 == 0) confirm.buttons = CC_BUTTON_A | CC_BUTTON_CONFIRM;
        Control::WriteInput(confirm, {}); Pace(stageRematch_); return true;
    }
    if (ending_ && phase == Phase::InGame) {
        Control::WriteInput({}, {}); Pace(false); return true;
    }
    // 対戦シーンの入力欠落時はゲーム更新へ戻らない。観戦端末だけが待つ。
    const auto begin = platform::RealMonotonicUs();
    for (;;) {
        if (!have_) have_ = wire.Take(pending_);
        if (have_ || phase != Phase::InGame) break;
        if (!wire.Active() || platform::RealMonotonicUs() - begin > 30000000) return false;
        if (platform::IsAbortRequested()) return false;
#ifdef _WIN32
        // Step内でStart/確定入力を待つ間も、自窓の移動・閉じる操作を受け付ける。
        // ゲーム更新へは戻らず、イントロ先頭画像と確定再生位置を維持する。
        game_interface::WndProcHook::PumpMessages();
#endif
        // 観戦の受信待ちには対戦者の精密な締切は不要。スピンせずCPUを返す。
        // 毎回必ず休止し、窓への大量投稿があっても受信確認で回り続けない。
        // 待機中も完成画像だけを提示する。ゲーム状態・入力は進めない。
        for (unsigned i = 0; i != 20; ++i) {
            core::timer::IdlePresentation::Pump(10000);
            platform::PreciseWaitUs(500);
        }
    }
    if (!have_) {
        if (phase == Phase::CharaSelect && selectionPrepared_) return Select(false);
        Control::WriteInput({}, {}); Pace(stageRematch_ || phase == Phase::Loading); return true;
    }
    for (unsigned events = 0; events < 8; ++events) {
        if (pending_.kind == Selecting) {
            if (phase != Phase::CharaSelect && !(stageRematch_ && phase == Phase::Rematch)) {
                Control::WriteInput({}, {}); Pace(stageRematch_); return true;
            }
            match = pending_.Get<StartData>(); score = match.score;
            selectionPrepared_ = true;
            domain::session::DebugLog("[Spectator] SELECT PREPARE frame=%u p1=%u p2=%u stage=%u qpc=%lld",
                pending_.frame, match.p1.confirmed, match.p2.confirmed, match.p1.stageConfirmed, platform::RealMonotonicUs());
        } else if (pending_.kind == Start || pending_.kind == Selection) {
            const auto incoming = pending_.Get<StartData>();
            if (stageRematch_ && phase == Phase::Rematch && pending_.kind == Selection) {
                match = incoming; score = match.score;
                // ONCEでは初回に適用したルールを保持する。設定書込みはキャラ選択専用。
                if (mem.SpectatorRules() != std::array<uint32_t, 3>{match.roundsToWin, match.damageLevel, match.timerSpeed} ||
                    !mem.CommitStageRematch(match.p1.stage)) return false;
                retrying_ = true; nav_ = 0;
                // Selectionはイントロ到達まで保持し、従来どおりStart待機へ渡す。
                Control::WriteInput({}, {}); Pace(true); return true;
            }
            if (phase == Phase::CharaSelect) {
                match = incoming;
                score = match.score;
                return Select(true);
            }
            if (phase != Phase::InGame) {
                Control::WriteInput({}, {});
                Pace(stageRematch_); return true;
            }
            if (pending_.kind == Selection) {
                have_ = false;
                domain::session::DebugLog("[Spectator] INTRO PREPARED qpc=%lld", platform::RealMonotonicUs());
                return Step(phase); // 両プレイヤーのepoch barrier通過を通知するStartまで進めない。
            }
            match = incoming; score = match.score;
            selecting_ = retrying_ = ending_ = false;
            if (!mem.SnapshotSize() || !mem.PrepareBattleAudio() || !mem.WriteRng(match.rng)) return false;
            mem.AlignIntroRng(); next_ = pending_.frame;
            catchupPending_ = true;
            domain::session::DebugLog("[Spectator] START frame=%u p1=%u p2=%u stage=%u qpc=%lld match=%u latestMatch=%u", next_, match.p1.character, match.p2.character, match.p1.stage, platform::RealMonotonicUs(), match.score.revision, wire.LatestMatch());
        } else if (pending_.kind == Epoch) {
            if (phase != Phase::InGame || mem.IntroState() != 2) return false;
            if (!mem.PrepareBattleAudio() || !mem.WriteRng(pending_.Get<game_interface::RngState>())) return false;
            mem.AlignIntroRng(); next_ = pending_.frame;
            catchupPending_ = true;
            domain::session::DebugLog("[Spectator] EPOCH frame=%u", next_);
        } else if (pending_.kind == Result) {
            score = pending_.Get<Score>();
            ending_ = true;
            domain::session::DebugLog("[Spectator] SCORE revision=%u p1=%u p2=%u", score.revision, score.p1, score.p2);
            have_ = false; Control::WriteInput({}, {}); Pace(false); return true;
        } else if (pending_.kind == Retry) {
            if (phase != Phase::Rematch) {
                // 元ゲーム終了演出の最後の1Fはホストと一致しない場合がある。
                Control::WriteInput({}, {}); Pace(false); return true;
            }
            stageRematch_ = pending_.payload[0] == 2;
            if (!mem.SetStageRematchFastPath(stageRematch_)) return false;
            if (!stageRematch_) mem.SetRetryTarget(int(pending_.payload[0]));
            // RANDOM ONCEは選択確定通知を待ってから通常ONCEを解放する。
            retrying_ = !stageRematch_; nav_ = 0;
            domain::session::DebugLog("[Spectator] RETRY target=%u", pending_.payload[0]);
            have_ = false; Control::WriteInput({}, {}); Pace(stageRematch_); return true;
        } else if (pending_.kind == Input) {
            if (phase != Phase::InGame || pending_.frame != next_) return false;
            mem.BeginSimulation(next_);
            inputs = {pending_.payload[0], pending_.payload[1]};
            Control::WriteInput(game_interface::GameInput::Unpack(pending_.payload[0]),
                                game_interface::GameInput::Unpack(pending_.payload[1]));
            last_ = next_++;
            have_ = false;
            // 合流時に確定入力の末尾へ追いついたら高速追いつきを解除する。
            // 以後のTCP到着揺れでは毎回追いつき処理へ戻さない。
            const auto latest = wire.Latest();
            if (catchupPending_ && last_ >= latest) {
                catchupPending_ = false;
                domain::session::DebugLog("[Spectator] CAUGHT frame=%u latest=%u qpc=%lld", last_, latest, platform::RealMonotonicUs());
            }
            // 確定入力を順番に再生してイントロの状態/RNGを保つ。
            // 追いつきとイントロの待機省略を維持する。
            const bool introFast = IsIntroPlayback(phase, mem.IntroState());
            if (introFast != introFast_) {
                introFast_ = introFast;
                domain::session::DebugLog("[Spectator] INTRO FAST enabled=%u frame=%u latest=%u qpc=%lld",
                    unsigned(introFast), last_, latest, platform::RealMonotonicUs());
            }
            // 入力が未着なら上の受信待ちに留まり、予測やA追加入力は行わない。
            // 末尾へ追いついた後もイントロ中は60Hz待機を追加しない。
            // 描画OFFは2試合以上前の再生だけ。1試合差に縮まれば追いつき中でも描画ONへ。
            // Resultで増える表示用scoreではなく、再生中Startの試合番号で比較する。
            Pace(catchupPending_ || introFast,
                introFast || !SkipCatchupDrawing(catchupPending_, match.score.revision, wire.LatestMatch()));
            return true;
        } else return false;
        have_ = wire.Take(pending_);
        if (!have_) return Step(phase); // イベント直後にも未着入力で前進させない。
    }
    return false;
}
}
