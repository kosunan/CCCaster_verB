#include "core_dll/spectator/Playback.hpp"
#include "core_dll/engine/FrameControl.hpp"
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "shared_contracts/IpcData.hpp"

namespace cccaster::spectator {
using Phase = game_interface::GamePhase;
using Control = domain::session::FrameControl;
void Playback::Pace(bool fast) {
    if (fast != catching_) pace_.Stop();
    catching_ = fast;
    if (fast) {
        // V10共通の高速スキップ。処理予算・倍率上限・追加待機を設けない。
        Control::SetModeHighSpeedSkip();
        return;
    }
    Control::SetModeNormalSpeed();
    if (!pace_.IsRunning()) pace_.Start();
    pace_.WaitForNextTick(false);
}
bool Playback::Step(Phase phase) {
    auto &wire = Transport::Get();
    auto &mem = game_interface::GameMem();
    if (wire.State() == Status::Failed || wire.State() == Status::Disconnected || wire.State() == Status::Overflow)
        return false;
    if (phase != previous_) {
        if (phase != Phase::InGame && phase != Phase::Rematch) ending_ = false;
        if (phase == Phase::CharaSelect) {
            if (!mem.BeginIndependentSelect(false)) return false;
            selecting_ = true;
        }
        if (phase == Phase::Rematch) { mem.BeginIndependentRetry(); retrying_ = false; }
        if (phase == Phase::InGame)
            domain::session::DebugLog("[Spectator] INTRO ENTER qpc=%lld", platform::RealMonotonicUs());
        previous_ = phase;
    }
    if (retrying_ && phase == Phase::Rematch) {
        game_interface::GameInput confirm{};
        if (++nav_ % 8 == 0) confirm.buttons = CC_BUTTON_A | CC_BUTTON_CONFIRM;
        Control::WriteInput(confirm, {}); Pace(false); return true;
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
        platform::RealSleepMs(1);
    }
    if (!have_) { Control::WriteInput({}, {}); Pace(retrying_ && phase == Phase::Loading); return true; }
    for (unsigned events = 0; events < 8; ++events) {
        if (pending_.kind == Start || pending_.kind == Selection) {
            const auto incoming = pending_.Get<StartData>();
            if (phase == Phase::CharaSelect) {
                match = incoming;
                score = match.score;
                if (!selecting_) return false;
                if (!mem.SetSpectatorRules({match.roundsToWin, match.damageLevel, match.timerSpeed})) return false;
                const auto p1 = mem.DriveRemoteSelection(false, match.p1, ++nav_);
                const auto p2 = mem.DriveRemoteSelection(true, match.p2, nav_);
                mem.SetSelectionRelease(true, match.p1.stage);
                Control::WriteInput(p1, p2); Pace(true); return true;
            }
            if (phase != Phase::InGame) { Control::WriteInput({}, {}); Pace(phase == Phase::Loading); return true; }
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
            domain::session::DebugLog("[Spectator] START frame=%u p1=%u p2=%u stage=%u qpc=%lld", next_, match.p1.character, match.p2.character, match.p1.stage, platform::RealMonotonicUs());
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
            mem.SetRetryTarget(int(pending_.payload[0])); retrying_ = true; nav_ = 0;
            domain::session::DebugLog("[Spectator] RETRY target=%u", pending_.payload[0]);
            have_ = false; Control::WriteInput({}, {}); Pace(false); return true;
        } else if (pending_.kind == Input) {
            if (phase != Phase::InGame || pending_.frame != next_) return false;
            mem.BeginSimulation(next_);
            inputs = {pending_.payload[0], pending_.payload[1]};
            Control::WriteInput(game_interface::GameInput::Unpack(pending_.payload[0]),
                                game_interface::GameInput::Unpack(pending_.payload[1]));
            last_ = next_++;
            have_ = false;
            // 合流時に確定入力の末尾へ追いついたら描画を復帰する。
            // 以後のTCP到着揺れでは毎回描画OFFへ戻さない。
            const auto latest = wire.Latest();
            if (catchupPending_ && last_ >= latest) {
                catchupPending_ = false;
                domain::session::DebugLog("[Spectator] CAUGHT frame=%u latest=%u qpc=%lld", last_, latest, platform::RealMonotonicUs());
            }
            Pace(catchupPending_);
            return true;
        } else return false;
        have_ = wire.Take(pending_);
        if (!have_) return Step(phase); // イベント直後にも未着入力で前進させない。
    }
    return false;
}
}
