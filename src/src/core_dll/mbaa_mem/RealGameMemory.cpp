#include "core_dll/mbaa_mem/StartupSounds.hpp"
#include "core_dll/timing/SpinProbe.hpp"
#include "core_dll/common/VirtualControllerTest.hpp"
// ============================================================================
// RealGameMemory.cpp — 実メモリ読み書き（実装）
//
// ここが MbaaAddresses.hpp / MbaaInputDefs.hpp のアドレスに触れる唯一の場所
// （FastBoot のコード書換と起動時パッチを除く）。
// 入力書込みは FrameControl から移設したもので、処理内容は変えていない。
// ============================================================================

#include "core_dll/mbaa_mem/RealGameMemory.hpp"
#include "core_dll/mbaa_mem/NativeInputWrites.hpp"
#include "core_dll/mbaa_mem/RoundTest.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/mbaa_mem/CombatStress.hpp"
#include "core_dll/mbaa_mem/GaugeStress.hpp"
#include "core_dll/hook/DriverLockProbe.hpp"
#include "core_dll/mbaa_mem/SoundPrewarm.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/ScriptedInput.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/mbaa_mem/SpectatorIntroDraw.hpp"
#include "core_dll/mbaa_mem/NativeResolution.hpp"

#include <windows.h>
#include <cstring>

namespace cccaster::game_interface {

using cccaster::domain::session::DebugLog;

namespace {

/// 入力書込み先はポインタ経由。ゲーム側が未初期化だと NULL になる。
char *InputBasePtr() {
    return *reinterpret_cast<char **>(CC_PTR_TO_WRITE_INPUT_ADDR);
}

void LogNullInputBase() {
    static uint32_t s_nullCount = 0;
    if (s_nullCount++ % 120 == 0) {
        DebugLog("[RealGameMemory] Input base pointer is NULL (count=%u)", s_nullCount);
    }
}

} // namespace

bool RealGameMemory::IsAvailable() const {
    return !IsBadReadPtr(CC_GAME_MODE_ADDR, sizeof(uint32_t));
}

uint32_t RealGameMemory::GameMode() const {
    return *CC_GAME_MODE_ADDR;
}

uint8_t RealGameMemory::IntroState() const {
    return *CC_INTRO_STATE_ADDR;
}

int RealGameMemory::StageAnimation() const {
    // 0x431B69: 標準StageAnimationは[0x554140の設定構造体+0x164]。
    // 0x4B954E: 非0なら初期2更新以外の背景アニメーション更新を省略する。
    if (!game_build::RuntimeValidated() ||
        *reinterpret_cast<uintptr_t *>(0x554140) + 0x164 !=
            reinterpret_cast<uintptr_t>(CC_STAGE_ANIMATION_OFF_ADDR)) return -1;
    const auto value = *CC_STAGE_ANIMATION_OFF_ADDR;
    return value <= 1 ? int(value == 0) : -1;
}
bool RealGameMemory::SetStageAnimation(bool enabled) {
    if (GameMode() != CC_GAME_MODE_CHARA_SELECT || StageAnimation() < 0) return false;
    *CC_STAGE_ANIMATION_OFF_ADDR = enabled ? 0 : 1;
    DebugLog("[SelectionOptions] BACKGROUND animation=%s mode=%u", enabled ? "ON" : "OFF", GameMode());
    return true;
}
bool RealGameMemory::SelectionDelayEditable(bool host) const {
    // 0x4281B7..0x4281C1のカラー確定(mode4)以降は既存合意でも変更不可。
    return GameMode() == CC_GAME_MODE_CHARA_SELECT &&
        *(host ? CC_P1_SELECTOR_MODE_ADDR : CC_P2_SELECTOR_MODE_ADDR) < 4;
}

int RealGameMemory::DisplayOption(NativeDisplayOption option) const {
    const auto* definition = DisplayDefinition(option);
    // 同じ設定構造体の版・基点をStageAnimationと共通の条件で検証する。
    if (!definition || StageAnimation() < 0) return -1;
    const auto base = *reinterpret_cast<uintptr_t*>(0x554140);
    const auto value = *reinterpret_cast<uint32_t*>(base + definition->offset);
    return value < unsigned(definition->count) ? int(value) : -1;
}
bool RealGameMemory::SetDisplayOption(NativeDisplayOption option, int value) {
    const auto* definition = DisplayDefinition(option);
    if (!definition || value < 0 || value >= definition->count ||
        GameMode() != CC_GAME_MODE_CHARA_SELECT || DisplayOption(option) < 0) return false;
    const auto base = *reinterpret_cast<uintptr_t*>(0x554140);
    *reinterpret_cast<uint32_t*>(base + definition->offset) = value;
    DebugLog("[SelectionOptions] NATIVE option=%s value=%d", definition->name, value);
    return true;
}
ScreenResolution RealGameMemory::RenderResolution() const { return native_resolution::Read(); }
bool RealGameMemory::ChangeRenderResolution(int direction) { return native_resolution::Request(direction); }
bool RealGameMemory::SetRenderResolution(int width, int height) { return native_resolution::Restore(width, height); }

void RealGameMemory::SetTrainingHold(bool hold) {
    if (hold == trainingHold_) return;
    // 通常pauseはトレーニングメニューを開いてしまうため使用しない。
    // 0x423998で1減算した後も停止する2をセットし、次Presentで元に戻す。
    // 55DF00は同処理の「この更新で全体停止した」フラグ。保存前の値も保全する。
    auto *freeze = reinterpret_cast<uint32_t *>(0x562A48);
    auto *active = reinterpret_cast<uint32_t *>(0x55DF00);
    if (hold) {
        trainingFreezeBefore_ = *freeze;
        trainingFreezeActiveBefore_ = *active;
        *freeze = 2;
    } else {
        *freeze = trainingFreezeBefore_;
        *active = trainingFreezeActiveBefore_;
    }
    trainingHold_ = hold;
}

void RealGameMemory::PlaceTrainingCorner(int direction, int player) {
    // 標準リセットが戦闘状態を初期化した後、配置だけを変更する。
    // 子キャラは各陣営の親との相対位置を維持する。
    const int32_t positions[2] = {direction * (player == 0 ? 45056 : 61440),
                                  direction * (player == 1 ? 45056 : 61440)};
    for (int side = 0; side < 2; ++side) {
        const auto offset = side * CC_PLR_STRUCT_SIZE;
        const auto delta = positions[side] - *reinterpret_cast<int32_t *>(
            reinterpret_cast<char *>(CC_P1_X_POSITION_ADDR) + offset);
        for (int actor : {side, side + 2}) {
            const auto stride = actor * CC_PLR_STRUCT_SIZE;
            *reinterpret_cast<int32_t *>(reinterpret_cast<char *>(CC_P1_X_POSITION_ADDR) + stride) += delta;
            *reinterpret_cast<int32_t *>(reinterpret_cast<char *>(CC_P1_X_PREV_POS_ADDR) + stride) += delta;
            *reinterpret_cast<uint8_t *>(reinterpret_cast<char *>(CC_P1_FACING_FLAG_ADDR) + stride) =
                (side == player) == (direction < 0);
        }
    }
    *CC_CAMERA_X_ADDR = direction * 26624;
    DebugLog("[TrainingCorner] player=%d direction=%d p1x=%d p2x=%d camera=%d", player + 1,
             direction, *CC_P1_X_POSITION_ADDR, *CC_P2_X_POSITION_ADDR, *CC_CAMERA_X_ADDR);
}

uint32_t RealGameMemory::WorldTimer() const {
    return *CC_WORLD_TIMER_ADDR;
}

uint32_t RealGameMemory::RealTimer() const {
    return *CC_REAL_TIMER_ADDR;
}

uint32_t RealGameMemory::MenuStateCounter() const {
    return *CC_MENU_STATE_COUNTER_ADDR;
}

domain::session::MatchResultFacts RealGameMemory::ReadMatchResult() const {
    // 対象版の既存アドレスを再利用。勝数は既存snapshot保存とMEM診断の対象。
    // ゲームスレッド上、再計算完了後のRETRY入口でのみ読み取る。
    if (GameMode() != CC_GAME_MODE_RETRY ||
        IsBadReadPtr(CC_P1_WINS_ADDR, 4) || IsBadReadPtr(CC_P2_WINS_ADDR, 4) ||
        IsBadReadPtr(CC_WIN_COUNT_VS_ADDR, 4)) return {};
    return {*CC_P1_WINS_ADDR, *CC_P2_WINS_ADDR, *CC_WIN_COUNT_VS_ADDR, true};
}

void RealGameMemory::WriteInput(GameInput p1, GameInput p2) {
    if (cccaster::testing::gauge_stress::active) p1 = p2 = {};
    if (cccaster::testing::combat_stress::Active()) {
        p1.direction = p2.direction = Dir::Neutral;
    }
    char *base = InputBasePtr();
    if (!base) {
        LogNullInputBase();
        return;
    }
    // 入力採取時点ではなく、ゲームが消費するフレームの選択段階で判定する。
    // 旧CCCasterのキャラセレ終了防止と同じ条件。ムーン・カラー選択の戻る操作は残す。
    if (*CC_GAME_MODE_ADDR == CC_GAME_MODE_CHARA_SELECT) {
        constexpr auto keepButtons = static_cast<uint16_t>(~(CC_BUTTON_B | CC_BUTTON_CANCEL));
        if (*CC_P1_SELECTOR_MODE_ADDR == CC_SELECT_CHARA)
            p1.buttons &= keepButtons;
        if (*CC_P2_SELECTOR_MODE_ADDR == CC_SELECT_CHARA)
            p2.buttons &= keepButtons;
    }
    *reinterpret_cast<uint32_t *>(base + CC_P1_OFFSET_DIRECTION) = p1.direction;
    *reinterpret_cast<uint16_t *>(base + CC_P1_OFFSET_BUTTONS) = p1.buttons;
    *reinterpret_cast<uint32_t *>(base + CC_P2_OFFSET_DIRECTION) = p2.direction;
    *reinterpret_cast<uint16_t *>(base + CC_P2_OFFSET_BUTTONS) = p2.buttons;
}

void InstallRealGameMemory() {
    static RealGameMemory s_real;
    InstallGameMemory(&s_real);
}

bool RealGameMemory::ReadRng(RngState &state) const {
    if (IsBadReadPtr(CC_RNG_STATE0_ADDR, 4) || IsBadReadPtr(CC_RNG_STATE1_ADDR, 4) ||
        IsBadReadPtr(CC_RNG_STATE2_ADDR, 4) || IsBadReadPtr(CC_RNG_STATE3_ADDR, 220))
        return false;
    state[0] = *CC_RNG_STATE0_ADDR;
    state[1] = *CC_RNG_STATE1_ADDR;
    state[2] = *CC_RNG_STATE2_ADDR;
    std::memcpy(state.data() + 3, CC_RNG_STATE3_ADDR, 220);
    return true;
}
bool RealGameMemory::WriteRng(const RngState &state) {
    if (IsBadWritePtr(CC_RNG_STATE0_ADDR, 4) || IsBadWritePtr(CC_RNG_STATE1_ADDR, 4) ||
        IsBadWritePtr(CC_RNG_STATE2_ADDR, 4) || IsBadWritePtr(CC_RNG_STATE3_ADDR, 220))
        return false;
    *CC_RNG_STATE0_ADDR = state[0];
    *CC_RNG_STATE1_ADDR = state[1];
    *CC_RNG_STATE2_ADDR = state[2];
    std::memcpy(CC_RNG_STATE3_ADDR, state.data() + 3, 220);
    return true;
}

} // namespace cccaster::game_interface

#include "core_dll/rollback/GameSnapshotLayout.hpp"
#include "core_dll/rollback/ReplayRoundLocation.hpp"
#include "core_dll/rollback/ReplayCursorBounds.hpp"
#include "core_dll/rollback/IntroSoundClock.hpp"
#include "core_dll/mbaa_mem/BattleProgress.hpp"
#include "core_dll/rollback/ReplayEffects.hpp"
namespace cccaster::game_interface {
static cccaster::sync::PointerSnapshot &SnapshotDumper() {
    static cccaster::sync::PointerSnapshot dumper;
    static const bool configured = [&] {
        std::vector<cccaster::sync::SnapshotNode> nodes(std::begin(cccaster::sync::GameSnapshotLayout),
                                                        std::end(cccaster::sync::GameSnapshotLayout));
        // Present境界ではKO演出のカウントダウンと速度補間も戻す必要がある。
        // 0x423B43/0x423B52で減算、0x472A20で速度補間。旧フレームフック用の表にはない。
        // 0x423B16: 共有ヒットストップ中はKOスロータイマーを減らさない。
        nodes.push_back({-1, 0x557D2A, 0, 1});
        nodes.push_back({-1, 0x563574, 0, 2});
        nodes.push_back({-1, 0x562A70, 0, 4});
        nodes.push_back({-1, 0x55DEC0, 0, 4});
        nodes.push_back({-1, 0x55DEF0, 0, 4});
        nodes.push_back({-1, 0x55DF24, 0, 4});
        nodes.push_back({-1, reinterpret_cast<uintptr_t>(cccaster::sync::IntroSoundClock::until.data()),
                         0, sizeof(cccaster::sync::IntroSoundClock::until)});
        // 0x42397Aで進み、0x473400/0x4751D6で結果計算に使うシミュレーションF。
        nodes.push_back({-1, 0x55D1CC, 0, 4});
        // 状態初期化の0x45EF90等が積み、0x4DE200が再生後に消去する未処理要求。
        // 初期化直後の保存でも、再計算側に同じ要求と音声開始Fを渡す。
        nodes.push_back({-1, 0x76E008, 0, 1500});
        // 0x45D700の拡大演出設定、0x44AF20の進行。倍率だけ戻すと次Fで補間がずれる。
        nodes.push_back({-1, 0x564AFC, 0, 4});
        nodes.push_back({-1, 0x564B04, 0, 8});
        nodes.push_back({-1, 0x564B20, 0, 4});
        // EF152（0x45D700）の透過演出。0x4B7060が進めるモードと濃度も戻す。
        nodes.push_back({-1, 0x558604, 0, 4});
        nodes.push_back({-1, 0x562A50, 0, 4});
        // 0x42FD40等が使う第3乱数列。旧表はindex（0x563864）だけで列を保存していない。
        nodes.push_back({-1, 0x563868, 0, 224});
        // EF11発動直後の停止開始フラグ。0x423900で消去する前段EFでも参照される。
        nodes.push_back({-1, 0x5595BC, 0, 4});
        return dumper.Configure(nodes);
    }();
    (void)configured;
    return dumper;
}

#pragma pack(push, 1)
struct ReplayState {
    uint8_t bytes[8];
};
struct ReplayContainer {
    uint32_t unknown0;
    ReplayState *states;
    char *end;
    uint32_t unknown1;
    int total, total2, index;
    uint32_t frameInState; // 同じ入力が続く区間内の再生経過。標準0x444D52でリセット。
};
struct ReplayRound {
    char unknown[0x120];
    ReplayContainer *inputs;
    char tail[0x10];
    uint32_t *rngBegin, *rngEnd, *rngCapacity; // 標準serializer 0x445D14のvector
};
#pragma pack(pop)
struct ReplayCursor {
    uint32_t round = 0; // 論理ラウンドindex+1。保存時のアドレスではない。
    int index = 0, total = 0, total2 = 0; // 0x440991..0x4409A3の空コンテナ初期値。
    uint32_t endOffset = 0;
    bool hasLast = false;
    ReplayState last{};
    uint32_t rngCount = 0; // 先頭プレイヤーのスロットでラウンド全体の追記位置を保持
};
using ReplayCursors = std::array<ReplayCursor, 4>;
static uint32_t CurrentReplayRoundIndex() {
    return *reinterpret_cast<const uint32_t *>(0x77BFA4);
}
static bool CurrentReplayRound(ReplayRound *&round) {
    static_assert(sizeof(ReplayRound) == 0x140);
    const auto begin = *reinterpret_cast<const uintptr_t *>(0x77BF98);
    const auto end = *reinterpret_cast<const uintptr_t *>(CC_REPROUND_TBL_ENDPTR_ADDR);
    const auto location = cccaster::sync::LocateReplayRound(begin, end, CurrentReplayRoundIndex());
    round = reinterpret_cast<ReplayRound *>(location.address);
    return location.valid && (!round || !IsBadReadPtr(round, sizeof(ReplayRound)));
}
static bool ReadReplayCursors(ReplayCursors &cursors, bool restartPlayback = false) {
    ReplayRound *round = nullptr;
    if (!CurrentReplayRound(round)) return false;
    for (auto &saved : cursors) saved.round = CurrentReplayRoundIndex() + 1;
    // intro=2では次ラウンドが未作成。前ラウンド末尾を保存しない。
    if (!round || !round->inputs)
        return true;
    const auto rngBytes = uintptr_t(round->rngEnd) - uintptr_t(round->rngBegin);
    if (rngBytes % 4 || rngBytes > 16000000 ||
        (rngBytes && IsBadReadPtr(round->rngBegin, rngBytes))) return false;
    cursors[0].rngCount = static_cast<uint32_t>(rngBytes / 4);
    if (IsBadReadPtr(round->inputs, 4 * sizeof(ReplayContainer)))
        return false;
    for (int i = 0; i < 4; ++i) {
        const auto &c = round->inputs[i];
        const auto length = uintptr_t(c.end) - uintptr_t(c.states);
        if (length > 16000000 || (!restartPlayback && c.index > 1000000)) {
            DebugLog("[ReplayCursor] capture invalid player=%d states=%p end=%p index=%d total=%d", i,
                     c.states, c.end, c.index, c.total);
            return false;
        }
        auto &saved = cursors[i];
        // 抽選で選ばれた録画は、前の再生終了でindexが末尾の次にある場合がある。
        // 先頭再生では現在indexでなく、存在する最初の入力から検証する。
        saved.index = restartPlayback ? 0 : c.index;
        saved.total = c.total;
        saved.total2 = c.total2;
        saved.endOffset = static_cast<uint32_t>(length);
        // 再戦開始直後はindex=0でも格納件数0。この空状態も有効なスナップショット。
        if (length) {
            if (!c.states || saved.index < 0 || length < sizeof(ReplayState) * size_t(saved.index + 1) ||
                IsBadReadPtr(&c.states[saved.index], 8)) {
                DebugLog("[ReplayCursor] capture bounds player=%d length=%u index=%d total=%d", i,
                         unsigned(length), c.index, c.total);
                return false;
            }
            saved.last = c.states[saved.index];
            saved.hasLast = true;
        }
    }
    return true;
}
static bool WriteReplayCursors(const ReplayCursors &cursors, bool playback = false) {
    ReplayRound *round = nullptr;
    if (!CurrentReplayRound(round)) return false;
    for (const auto &saved : cursors) {
        if (saved.round != CurrentReplayRoundIndex() + 1) return false;
        if ((!round || !round->inputs) && (saved.hasLast || saved.endOffset || saved.rngCount)) return false;
    }
    if (!round || !round->inputs) return true;
    const auto rngBytes = uintptr_t(round->rngEnd) - uintptr_t(round->rngBegin);
    if (rngBytes % 4 || rngBytes > 16000000 || cursors[0].rngCount > rngBytes / 4) return false;
    // メモリを戻す前に4プレイヤー分を検証。再確保されたバッファは現在のポインターを使う。
    for (int i = 0; i < 4; ++i) {
        const auto &saved = cursors[i];
        if (IsBadReadPtr(round->inputs, 4 * sizeof(ReplayContainer))) {
            DebugLog("[ReplayCursor] restore round player=%d saved=%u current=%u intro=%u", i,
                unsigned(saved.round), CurrentReplayRoundIndex() + 1, *CC_INTRO_STATE_ADDR);
            return false;
        }
        const auto &c = round->inputs[i];
        const auto length = uintptr_t(c.end) - uintptr_t(c.states);
        if (length > 16000000 || length < saved.endOffset ||
            (length && (!c.states || IsBadWritePtr(c.states, length)))) {
            DebugLog("[ReplayCursor] restore length player=%d saved=%u current=%u intro=%u", i,
                saved.endOffset, unsigned(length), *CC_INTRO_STATE_ADDR);
            return false;
        }
        if (!cccaster::sync::CanRestoreReplayCursor(saved.index, saved.endOffset, saved.hasLast,
                                                    c.index, length, playback)) {
            DebugLog("[ReplayCursor] restore index player=%d saved=%d current=%d intro=%u", i,
                saved.index, c.index, *CC_INTRO_STATE_ADDR);
            return false;
        }
    }
    for (int i = 0; i < 4; ++i) {
        const auto &saved = cursors[i];
        auto &c = round->inputs[i];
        const auto length = uintptr_t(c.end) - uintptr_t(c.states);
        if (!playback && length > saved.endOffset)
            std::memset(reinterpret_cast<char *>(c.states) + saved.endOffset, 0, length - saved.endOffset);
        if (!playback && saved.hasLast)
            c.states[saved.index] = saved.last;
        // TrainingのDUMMYは、保存時の途中位置ではなく録画の先頭から再生する。
        // total2は録画の総フレーム数なので保持し、記録内容には触れない。
        c.index = playback ? 0 : saved.index;
        c.total = playback ? 0 : saved.total;
        c.total2 = saved.total2;
        if (playback) c.frameInState = 0;
        if (!playback) c.end = c.states ? reinterpret_cast<char *>(c.states) + saved.endOffset : nullptr;
    }
    // vectorの容量と現在のバッファは保持。訂正再計算で古い予測の乱数列を上書きする。
    if (!playback) round->rngEnd = round->rngBegin ? round->rngBegin + cursors[0].rngCount : nullptr;
    return true;
}
size_t RealGameMemory::SnapshotSize() const {
    return cccaster::sync::InstallReplayEffects() ? SnapshotDumper().Size() + sizeof(ReplayCursors) : 0;
}
bool RealGameMemory::SaveSnapshot(std::span<char> data) {
    const auto size = SnapshotDumper().Size();
    if (data.size() != size + sizeof(ReplayCursors))
        return false;
    ReplayCursors cursors{};
    if (!ReadReplayCursors(cursors))
        return false;
    std::memcpy(data.data() + size, &cursors, sizeof(cursors));
    return SnapshotDumper().Save(data.first(size));
}
bool RealGameMemory::LoadSnapshot(std::span<char> data) {
    const auto size = SnapshotDumper().Size();
    if (data.size() != size + sizeof(ReplayCursors))
        return false;
    ReplayCursors cursors{};
    std::memcpy(&cursors, data.data() + size, sizeof(cursors));
    if (!WriteReplayCursors(cursors))
        return false;
    return SnapshotDumper().Load(data.first(size));
}

namespace {
// I+18..677: raw4枠と0x41F0C0が更新する3組の論理入力。
// vtable/DirectInputオブジェクトは保存しない。元の入力変換を実行した実値だけを戻す。
struct PresentationInputState {
    uintptr_t base;
    std::array<char, 0x660> values;
};
}
size_t RealGameMemory::PresentationSnapshotSize() const {
    const auto size = SnapshotSize();
    return size ? size + sizeof(PresentationInputState) : 0;
}
void RealGameMemory::SetPresentationPreview(bool enabled) {
    // 音声履歴・音声時計も進めない既存の表示専用経路。
    cccaster::sync::SetIntroPreviewEffects(enabled);
}
bool RealGameMemory::SavePresentationSnapshot(std::span<char> data) {
    const auto size = SnapshotSize();
    auto* input = InputBasePtr();
    if (!size || data.size() != size + sizeof(PresentationInputState) || !input ||
        IsBadReadPtr(input + 0x18, 0x660) || !SaveSnapshot(data.first(size))) return false;
    PresentationInputState state{};
    state.base = reinterpret_cast<uintptr_t>(input);
    std::memcpy(state.values.data(), input + 0x18, state.values.size());
    std::memcpy(data.data() + size, &state, sizeof(state));
    return true;
}
bool RealGameMemory::LoadPresentationSnapshot(std::span<char> data) {
    const auto size = SnapshotSize();
    if (!size || data.size() != size + sizeof(PresentationInputState)) return false;
    PresentationInputState state{};
    std::memcpy(&state, data.data() + size, sizeof(state));
    auto* input = InputBasePtr();
    if (!input || state.base != reinterpret_cast<uintptr_t>(input) ||
        IsBadWritePtr(input + 0x18, state.values.size())) return false;
    if (!LoadSnapshot(data.first(size)) || input != InputBasePtr()) return false;
    std::memcpy(input + 0x18, state.values.data(), state.values.size());
    return true;
}

// Trainingは戦闘状態だけを保存し、現在の敵設定・ダミー録画と独立させる。
// 通信の記録末尾を戻すSaveSnapshot/LoadSnapshotの形式は変更しない。
static bool DummyPlayback() {
    int16_t status = 0;
    std::memcpy(&status, CC_DUMMY_STATUS_ADDR, sizeof(status));
    return status == CC_DUMMY_STATUS_DUMMY;
}
static bool ReselectTrainingDummySlot() {
    // 標準の0x477920は空スロットを除外し、REPLAY SLOTのランダム設定で抽選する。
    // 呼出規約は引数なし・EAX戻り値。選択結果は0x74D5CCにも格納される。
    constexpr uint8_t expected[] = {0x83,0x3D,0x28,0xC2,0x77,0x00,0x01,
                                    0xA1,0xD8,0xD5,0x74,0x00};
    if (std::memcmp(reinterpret_cast<const void *>(0x477920), expected, sizeof(expected))) return false;
    const auto selected = reinterpret_cast<uint32_t (__cdecl *)()>(0x477920)();
    *reinterpret_cast<uint32_t *>(0x77BFA4) = selected;
    return true;
}
bool RealGameMemory::RestartTrainingRecording() {
    if (!IsTrainingRecording()) return false;
    ReplayRound *round = nullptr;
    if (!CurrentReplayRound(round) || !round || !round->inputs ||
        IsBadWritePtr(round, sizeof(ReplayRound))) return false;
    // 現在のスロットだけを空の記録へ戻す。確保済みバッファと他スロットは保持。
    // 全コンテナを検証してから記録末尾・総F・再生位置・乱数末尾を0へ戻す。
    ReplayCursors empty{};
    for (auto &cursor : empty) cursor.round = CurrentReplayRoundIndex() + 1;
    if (!WriteReplayCursors(empty)) return false;
    for (int i = 0; i < 4; ++i) round->inputs[i].frameInState = 0;
    // 標準の録画クリア0x444C9D/0x444CA3と同じラウンド内カウンタ。
    std::memset(reinterpret_cast<char *>(round) + 0x12C, 0, 4);
    std::memset(reinterpret_cast<char *>(round) + 0x80, 0, 4);
    return true;
}
size_t RealGameMemory::TrainingSnapshotSize() const {
    return cccaster::sync::InstallReplayEffects() ? SnapshotDumper().Size() : 0;
}
bool RealGameMemory::SaveTrainingSnapshot(std::span<char> data) {
    return SnapshotDumper().Save(data);
}
bool RealGameMemory::LoadTrainingSnapshot(std::span<char> data) {
    if (data.size() != SnapshotDumper().Size()) return false;
    // 0x477BD0の録画開始でP1/P2のCPU操作フラグが入れ替わる。
    // 保存時の値で上書きすると録画側がCPU扱いになり入力・記録が停止する。
    // 子キャラも含め現在の操作設定を保持する。戦闘中の入力値は通常どおり復元。
    constexpr uintptr_t inputModeBase = 0x555137, actorStride = 0xAFC;
    std::array<uint8_t, 4> inputModes{};
    for (size_t i = 0; i < inputModes.size(); ++i)
        inputModes[i] = *reinterpret_cast<const uint8_t *>(inputModeBase + i * actorStride);
    const bool randomPlayback = DummyPlayback() && *reinterpret_cast<const uint32_t *>(0x77C228) == 1;
    uint32_t liveSlotRngIndex = 0;
    std::array<uint32_t, 56> liveSlotRngTable{};
    if (IsTrainingRecording()) {
        if (!RestartTrainingRecording()) return false;
    } else if (DummyPlayback()) {
        // ランダム設定なら、一巡前のFN2でも標準処理で抽選し直す。
        const auto previousRound = CurrentReplayRoundIndex();
        const auto previousSelection = *reinterpret_cast<const uint32_t *>(0x74D5CC);
        if (randomPlayback && !ReselectTrainingDummySlot()) return false;
        // 内容・長さ・総フレーム数を保ち、選ばれた録画を先頭へ戻す。
        ReplayCursors current{};
        if (!ReadReplayCursors(current, true) || !WriteReplayCursors(current, true)) {
            *reinterpret_cast<uint32_t *>(0x77BFA4) = previousRound;
            *reinterpret_cast<uint32_t *>(0x74D5CC) = previousSelection;
            return false;
        }
        liveSlotRngIndex = *reinterpret_cast<const uint32_t *>(0x563864);
        if (randomPlayback)
            std::memcpy(liveSlotRngTable.data(), reinterpret_cast<void *>(0x563868), sizeof(liveSlotRngTable));
        DebugLog("[TrainingDummy] restart random=%d slot=%u rngIndex=%u", int(randomPlayback),
                 CurrentReplayRoundIndex(), liveSlotRngIndex);
    }
    if (!SnapshotDumper().Load(data)) return false;
    for (size_t i = 0; i < inputModes.size(); ++i)
        *reinterpret_cast<uint8_t *>(inputModeBase + i * actorStride) = inputModes[i];
    // FN2のランダム再生選択は現在の抽選履歴を保持する。通常のロールバックでは全て戻す。
    if (randomPlayback) {
        *reinterpret_cast<uint32_t *>(0x563864) = liveSlotRngIndex;
        std::memcpy(reinterpret_cast<void *>(0x563868), liveSlotRngTable.data(), sizeof(liveSlotRngTable));
    }
    return true;
}
} // namespace cccaster::game_interface

namespace cccaster::game_interface {
bool RealGameMemory::CanPredict() const {
    if (GameMode() != CC_GAME_MODE_IN_GAME || IntroState() != 0)
        return false;
    const bool p1 = *CC_P1_PUPPET_STATE_ADDR ? *CC_P3_NO_INPUT_FLAG_ADDR : *CC_P1_NO_INPUT_FLAG_ADDR;
    const bool p2 = *CC_P2_PUPPET_STATE_ADDR ? *CC_P4_NO_INPUT_FLAG_ADDR : *CC_P2_NO_INPUT_FLAG_ADDR;
    return !(p1 && p2);
}
bool RealGameMemory::CanRollback() const {
    return AllowsRollback(ClassifyBattle(GameMode() == CC_GAME_MODE_IN_GAME, IntroState(), !CanPredict()));
}
void RealGameMemory::AlignIntroRng() {
    // 0x4683EE..0x468426の演出用乱数は通常RNGとは別系統。
    // 合流済みRNGのindex/55語を複製するが、戦闘RNGそのものは進めない。
    *reinterpret_cast<uint32_t *>(0x563948) = *CC_RNG_STATE2_ADDR;
    *reinterpret_cast<uint32_t *>(0x56394C) = 0;
    std::memcpy(reinterpret_cast<void *>(0x563950), CC_RNG_STATE3_ADDR, 220);
}
} // namespace cccaster::game_interface

namespace cccaster::game_interface {
bool RealGameMemory::BeginReplay(uint32_t from, uint32_t target) {
    cccaster::sync::BeginReplayEffects(from, target);
    return true;
}
void RealGameMemory::EndReplay() {
    cccaster::sync::EndReplayEffects();
}
bool RealGameMemory::SetIntroPreview(bool active) {
    if (active && !game_memory::spectator_intro_draw::Prepare()) return false;
    cccaster::sync::SetIntroPreviewEffects(active);
    return true;
}
bool RealGameMemory::ConfigureInputWriteMonitor(bool enabled) {
    return native_input_writes::Configure(enabled);
}
cccaster::sync::InputWriteHistory* RealGameMemory::InputWrites() { return native_input_writes::History(); }
void RealGameMemory::BeginSimulation(uint32_t f) {
    native_input_writes::Begin(f);
    // 起動時に設定する試験オプション。毎FのCRT環境変数参照を締切後に持ち込まない。
    static const bool quickRetry = std::getenv("CCCASTER_TEST_RETRY_QUICK") != nullptr;
    static const bool quickKo = std::getenv("CCCASTER_TEST_ROUND_KO") != nullptr;
    static const bool quickDraw = std::getenv("CCCASTER_TEST_ROUND_DRAW") != nullptr;
    static const auto roundTestFrame = cccaster::testing::RoundTestFrame(std::getenv("CCCASTER_TEST_ROUND_END_FRAME"));
    static const auto roundTestLastEpoch = cccaster::testing::RoundTestFrame(std::getenv("CCCASTER_TEST_ROUND_END_MAX_EPOCH"), 0);
    using Probe = cccaster::diagnostics::SpinProbe;
    const bool probe = Probe::Enabled() && Probe::pending && Probe::sample.frame == f;
    auto *sample = probe ? &Probe::sample : nullptr;
    if (probe) sample->simEntry = Probe::Now();
    // 再戦疎通の短時間実機試験専用。通常対戦では無効。
    // 再計算でも同じFで適用し、ゲーム本来の時間切れ・勝敗・再戦遷移を通す。
    if ((cccaster::testing::IsScriptedInputEnabled() || cccaster::testing::IsVirtualControllerTest()) && (quickRetry || quickKo || quickDraw) &&
        CanPredict() && cccaster::testing::RoundTestDue(f, roundTestFrame, roundTestLastEpoch)) {
        if (quickKo) {
            // 時間切れと分けたKO経路の自動試験。ゲームスレッドでのみ変更する。
            if (*CC_P2_HEALTH_ADDR) {
                *CC_P1_HEALTH_ADDR = *CC_P1_RED_HEALTH_ADDR = 11400;
                *CC_P2_HEALTH_ADDR = *CC_P2_RED_HEALTH_ADDR = 0;
                cccaster::diagnostics::DeferredNumericLog::Log("[RetryTest] KO frame=%u", f);
            }
        } else if (*CC_ROUND_TIMER_ADDR > 1) {
            *CC_ROUND_TIMER_ADDR = 1;
            *CC_P1_HEALTH_ADDR = *CC_P1_RED_HEALTH_ADDR = 11400;
            *CC_P2_HEALTH_ADDR = *CC_P2_RED_HEALTH_ADDR = quickDraw ? 11400 : 5000;
            cccaster::diagnostics::DeferredNumericLog::Log("[RetryTest] SHORTEN frame=%u draw=%d", f, int(quickDraw));
        }
    }
    if (probe) sample->retryEnd = Probe::Now();
    cccaster::testing::combat_stress::Begin(CanPredict(), f);
    if (probe) sample->stressEnd = Probe::Now();
    cccaster::testing::gauge_stress::Begin(CanPredict(), f);
    if (probe) sample->gaugeEnd = Probe::Now();
    cccaster::diagnostics::driver_lock::SetFrame(f);
    cccaster::sync::BeginSimulationEffects(f);
    if (probe) sample->effectsEnd = Probe::Now();
}
bool RealGameMemory::PrepareBattleAudio() {
    cccaster::game_memory::startup_sounds::PrepareBattle();
    using SoundClock = cccaster::sync::IntroSoundClock;
    SoundClock::until.fill(0);
    SoundClock::duration.fill(0);
    // 起動合流中に音源の実データ長を読む。再計算・締切直前にはCOM照会しない。
    if (GameMode() == CC_GAME_MODE_IN_GAME && IntroState() != 0) {
        const auto objects = reinterpret_cast<uintptr_t *>(0x76C6F8);
        for (unsigned i = 0; i < SoundClock::duration.size(); ++i) {
            if (!objects[i]) continue;
            const auto buffers = *reinterpret_cast<IDirectSoundBuffer ***>(objects[i] + 4);
            const auto count = *reinterpret_cast<const int *>(objects[i] + 0x10);
            if (!count) continue;
            if (!buffers || count < 1 || count > 64) return false;
            for (int j = 0; j < count; ++j) {
                if (!buffers[j]) continue;
                DSBCAPS caps{}; caps.dwSize = sizeof(caps);
                WAVEFORMATEX format{};
                DWORD frequency = 0;
                if (FAILED(buffers[j]->GetCaps(&caps)) ||
                    FAILED(buffers[j]->GetFormat(&format, sizeof(format), nullptr)) ||
                    FAILED(buffers[j]->GetFrequency(&frequency)) || !format.nBlockAlign || !frequency)
                    return false;
                const auto frames = SoundClock::Frames(caps.dwBufferBytes, format.nBlockAlign, frequency);
                if (frames > SoundClock::duration[i]) SoundClock::duration[i] = frames;
            }
        }
        uint32_t hash = 2166136261u;
        for (auto frames : SoundClock::duration) hash = (hash ^ frames) * 16777619u;
        DebugLog("[IntroSoundClock] initialized=1 sources=%u hash=%u", unsigned(SoundClock::duration.size()), hash);
    }
    if (cccaster::testing::IsScriptedInputEnabled() || cccaster::testing::IsVirtualControllerTest()) {
        const auto p1 = reinterpret_cast<const uint32_t *>(0x74D83C);
        const auto p2 = reinterpret_cast<const uint32_t *>(0x74D868);
        DebugLog("[Select] LOADED p1=%u/%u/%u p2=%u/%u/%u stage=%u",
                 p1[1], p1[4], p1[0], p2[1], p2[4], p2[0], *CC_STAGE_SELECTOR_ADDR);
        // 0x4B6BF0がロード成功後に残すステージ番号とファイル名。selectorだけの確認にしない。
        // 0x4B6B50で一時バッファは解放される。展開後のフラグを検査する。
        DebugLog("[StageAsset] selected=%u loaded=%u expanded=%u file=%.259s",
            *CC_STAGE_SELECTOR_ADDR, *reinterpret_cast<const uint32_t *>(0x54CE90),
            *reinterpret_cast<const uint32_t *>(0x76E7B4),
            reinterpret_cast<const char *>(0x74FDA0));
    }
    if (std::getenv("CCCASTER_DISABLE_SOUND_PREWARM")) {
        DebugLog("[SoundPrewarm] disabled=1");
        return true;
    }
    // フェーズ合流前、戦闘開始演出だけ。再計算や通常の戦闘更新には入れない。
    if (GameMode() != CC_GAME_MODE_IN_GAME || IntroState() == 0) {
        DebugLog("[SoundPrewarm] skipped=no_intro");
        return true;
    }
    auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    constexpr unsigned char expected[] = {0x8b,0x47,0x04,0x56,0x8b,0x30};
    if (base != 0x400000 ||
        !game_build::RuntimeValidated() ||
        std::memcmp(reinterpret_cast<void *>(0x40f3a0), expected, sizeof(expected))) {
        DebugLog("[SoundPrewarm] skipped=image_mismatch");
        return true;
    }
    const auto started = cccaster::platform::RealMonotonicTicks();
    unsigned prepared = 0, skippedBuffers = 0;
    auto objects = reinterpret_cast<uintptr_t *>(0x76c6f8);
    for (unsigned i = 0; i < 1500; ++i) {
        if (!objects[i]) continue;
        auto list = *reinterpret_cast<IDirectSoundBuffer ***>(objects[i] + 4);
        const auto count = *reinterpret_cast<int *>(objects[i] + 0x10);
        if (!list || count < 1 || count > 64) continue;
        for (int j = 0; j < count; ++j) {
            if (!list[j]) continue;
            const auto result = sound_prewarm::Prepare(*list[j]);
            if (result == sound_prewarm::Result::FailedRestoration) {
                DebugLog("[SoundPrewarm] failed=restoration sound=%u slot=%d", i, j);
                return false;
            }
            if (result == sound_prewarm::Result::Prepared) ++prepared; else ++skippedBuffers;
        }
    }
    DebugLog("[SoundPrewarm] prepared=%u skipped=%u ticks=%lld restored=1", prepared, skippedBuffers,
        cccaster::platform::RealMonotonicTicks() - started);
    return true;
}
void RealGameMemory::PrepareStartupResources() {
    // 大分類InGameには標準REP一覧(mode26)も含まれる。実戦闘(mode1)でのみ先読みする。
    if (GameMode() != CC_GAME_MODE_IN_GAME) return;
    cccaster::game_memory::startup_sounds::PrepareBattle();
}
} // namespace cccaster::game_interface
