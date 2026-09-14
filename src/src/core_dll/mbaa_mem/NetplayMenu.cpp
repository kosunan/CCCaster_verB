#include "core_dll/mbaa_mem/RealGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include <windows.h>
#include <MinHook.h>
#include <cstring>
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/ScriptedInput.hpp"
namespace {
int target = -1;
bool retryActive = false;
int retryChoice = -1, retryCursor = -1;
bool selectActive = false, selectHost = false, stageChosen = false, selectRelease = false;
uint32_t chosenStage = 0, agreedStage = 0;
}
extern "C" {
void *cccaster_stage_original = nullptr;
__attribute__((force_align_arg_pointer)) uint32_t __cdecl cccaster_stage_result(uint32_t result) {
    if (!selectActive || *CC_GAME_MODE_ADDR != CC_GAME_MODE_CHARA_SELECT) return result;
    if (selectHost && !stageChosen && result == 255) {
        // 0は実ステージではなくランダム指定。通常は0x42F016でロード時に
        // 各端末が抽選するため、独立操作で進んだ乱数から別々の背景になる。
        // 元ゲームの抽選（使用可能ステージ・除外設定込み）をホストだけで先行し、
        // 実番号を最終選択として交換する。非0なのでロード側は再抽選しない。
        if (*CC_STAGE_SELECTOR_ADDR == 0) {
            reinterpret_cast<void (__cdecl *)()>(0x42F140)();
            cccaster::domain::session::DebugLog("[Select] RANDOM resolved=%u", *CC_STAGE_SELECTOR_ADDR);
        }
        stageChosen = true;
        chosenStage = *CC_STAGE_SELECTOR_ADDR;
    }
    if (selectRelease) {
        *CC_STAGE_SELECTOR_ADDR = agreedStage;
        cccaster::domain::session::DebugLog("[Select] COMMIT p1=%u/%u/%u p2=%u/%u/%u stage=%u",
            *CC_P1_CHARACTER_ADDR, *CC_P1_MOON_SELECTOR_ADDR, *CC_P1_COLOR_SELECTOR_ADDR,
            *CC_P2_CHARACTER_ADDR, *CC_P2_MOON_SELECTOR_ADDR, *CC_P2_COLOR_SELECTOR_ADDR, agreedStage);
        return 255;
    }
    // 確認待ちでも背景・描画・ローカル入力時計は通常更新を続ける。
    // 最終確定後のキャンセルによる、片側だけのキャラセレ再初期化を防ぐ。
    return 0;
}
__attribute__((naked)) void cccaster_stage_hook() {
    __asm__ __volatile__("pushfl\n\tpushal\n\tpushl %eax\n\tcall _cccaster_stage_result\n\t"
                         "addl $4,%esp\n\tmovl %eax,28(%esp)\n\tpopal\n\tpopfl\n\t"
                         "jmp *_cccaster_stage_original\n\t");
}
void *cccaster_menu_original = nullptr;
__attribute__((force_align_arg_pointer)) void __cdecl cccaster_menu_observe(uint32_t *menu, uint32_t *command) {
    if (*CC_GAME_MODE_ADDR != CC_GAME_MODE_RETRY)
        return;
    if (!retryActive) return;
    if (*command && cccaster::testing::IsScriptedInputEnabled())
        cccaster::domain::session::DebugLog("[RetryMenu] COMMAND value=%u cursor=%u", *command, menu[0x40 / 4]);
    // 元ゲームの項目有効フラグ。保存・終了をカーソルが飛ばす。
    const auto begin = menu[0x4c / 4], end = menu[0x50 / 4];
    if (begin && end >= begin && (end - begin) / 4 <= 16) {
        auto items = reinterpret_cast<uint32_t **>(begin);
        for (uint32_t i = 2; i < (end - begin) / 4; ++i)
            items[i][0x0c / 4] = 0;
    }
    auto &cursor = menu[0x40 / 4];
    if (target >= 0) cursor = static_cast<uint32_t>(target);
    else if (retryChoice >= 0) cursor = static_cast<uint32_t>(retryChoice);
    else if (cursor > 1) cursor = 0;
    if (retryCursor != int(cursor)) {
        retryCursor = int(cursor);
        cccaster::domain::session::DebugLog("[RetryMenu] CURSOR index=%u", cursor);
    }
    // 0x4299F5で判定される元ゲームの決定結果。入力や派生状態には書かない。
    if (target < 0) {
        if (*command == 1 && retryChoice < 0) {
            retryChoice = int(cursor);
            cccaster::domain::session::DebugLog("[RetryMenu] CONFIRM index=%d", retryChoice);
        }
        *command = 0; // 相手の確定・ACKが届くまで遷移だけを保留。
    } else if (*command != 1) *command = 0;
}
__attribute__((naked)) void cccaster_menu_hook() {
    // 元ESP+0x1cが決定結果。pushfl/pushalの36byteを加算して参照する。
    __asm__ __volatile__("pushfl\n\tpushal\n\tleal 64(%esp),%eax\n\tpushl %eax\n\tpushl %esi\n\t"
                         "call _cccaster_menu_observe\n\taddl $8,%esp\n\tpopal\n\tpopfl\n\t"
                         "jmp *_cccaster_menu_original\n\t");
}
}
namespace cccaster::game_interface {
std::array<uint32_t, 3> RealGameMemory::SpectatorRules() const {
    return {*CC_WIN_COUNT_VS_ADDR, *CC_DAMAGE_LEVEL_ADDR, *CC_TIMER_SPEED_ADDR};
}
bool RealGameMemory::SetSpectatorRules(const std::array<uint32_t, 3> &rules) {
    if (!IGameMemory::SetSpectatorRules(rules) || GameMode() != CC_GAME_MODE_CHARA_SELECT) return false;
    *CC_WIN_COUNT_VS_ADDR = rules[0];
    *CC_DAMAGE_LEVEL_ADDR = rules[1];
    *CC_TIMER_SPEED_ADDR = rules[2];
    return true;
}
bool RealGameMemory::ConfigureNetplayMenu() {
    static bool installed = false;
    *CC_AUTO_REPLAY_SAVE_ADDR = 0;
    if (installed)
        return true;
    void *address = reinterpret_cast<void *>(0x4299CB);
    const unsigned char expected[] = {0x85, 0xc9, 0x8b, 0x7e, 0x40};
    if (std::memcmp(address, expected, sizeof(expected)))
        return false;
    auto status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (MH_CreateHook(address, reinterpret_cast<void *>(cccaster_menu_hook), &cccaster_menu_original) !=
        MH_OK)
        return false;
    if (MH_EnableHook(address) != MH_OK)
        return false;
    installed = true;
    return true;
}
void RealGameMemory::SetRetryTarget(int value) {
    target = value;
    if (value < 0) retryActive = false;
}
void RealGameMemory::BeginIndependentRetry() {
    retryActive = true;
    target = retryChoice = retryCursor = -1;
}
int RealGameMemory::ReadRetryChoice() const { return retryChoice; }
bool RealGameMemory::BeginIndependentSelect(bool host) {
    static bool installed = false;
    if (!installed) {
        auto address = reinterpret_cast<void *>(0x42725B);
        const unsigned char expected[] = {0x83, 0xf8, 0xff, 0x74, 0x17};
        if (std::memcmp(address, expected, sizeof(expected))) return false;
        const unsigned char randomEntry[] = {0x83, 0xec, 0x08, 0x80, 0x3d, 0x0a, 0xd2, 0x55, 0x00, 0x00};
        if (std::memcmp(reinterpret_cast<void *>(0x42F140), randomEntry, sizeof(randomEntry))) return false;
        if (MH_CreateHook(address, reinterpret_cast<void *>(cccaster_stage_hook),
                          &cccaster_stage_original) != MH_OK || MH_EnableHook(address) != MH_OK) return false;
        installed = true;
    }
    selectActive = true;
    selectHost = host;
    stageChosen = selectRelease = false;
    chosenStage = agreedStage = 0;
    return true;
}
bool RealGameMemory::ReadLocalSelection(bool host, core::sync::SelectionState &state) {
    // 実機回帰試験専用。両キャラ確定後、通常の決定入力でランダムを選ぶ。
    if (host && !stageChosen && testing::IsScriptedInputEnabled() &&
        std::getenv("CCCASTER_TEST_RANDOM_STAGE") &&
        *CC_P1_SELECTOR_MODE_ADDR == 5 && *CC_P2_SELECTOR_MODE_ADDR == 5)
        *CC_STAGE_SELECTOR_ADDR = 0;
    uint32_t *s = host ? CC_P1_SELECTOR_MODE_ADDR : CC_P2_SELECTOR_MODE_ADDR;
    if (!state.confirmed && s[0] >= 4 && s[0] <= 5) {
        state.selector = s[3]; state.character = s[4]; state.moon = s[5]; state.color = s[6];
        state.confirmed = 1;
        ++state.revision;
        if (!state.Valid()) return false;
        domain::session::DebugLog("[Select] LOCAL epoch=%u char=%u moon=%u color=%u selector=%u",
                                  state.epoch, state.character, state.moon, state.color, state.selector);
    }
    if (host && stageChosen && !state.stageConfirmed) {
        state.stage = chosenStage;
        state.stageConfirmed = 1;
        ++state.revision;
    }
    if (state.confirmed) {
        s[4] = state.character; s[5] = state.moon; s[6] = state.color;
    }
    return true;
}
GameInput RealGameMemory::DriveRemoteSelection(bool host, const core::sync::SelectionState &state, uint32_t frame) {
    if (!state.confirmed) return {};
    uint32_t *s = host ? CC_P2_SELECTOR_MODE_ADDR : CC_P1_SELECTOR_MODE_ADDR;
    // 旧spectate自動選択と同じ公開selector経路。派生入力には書き込まない。
    // ランダムカーソルは、送信元で抽選済みの実キャラのカーソルへ解決する。
    s[3] = core::sync::SelectionState::CharacterCell(state.character);
    s[4] = state.character; s[5] = state.moon; s[6] = state.color;
    if (auto base = *reinterpret_cast<uint32_t *>(0x74D808))
        *reinterpret_cast<uint32_t *>(base + (host ? 1 : 0) * 0x1dc + 0x38) = 0;
    return s[0] < 4 && frame % 8 == 0 ? GameInput{0, CC_BUTTON_A | CC_BUTTON_CONFIRM} : GameInput{};
}
void RealGameMemory::SetSelectionRelease(bool release, uint32_t stage) {
    selectRelease = release;
    agreedStage = stage;
    if (!selectHost && release) *CC_STAGE_SELECTOR_ADDR = stage;
}
} // namespace cccaster::game_interface
