#include "core_dll/ui/TrainingStandbyView.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/ui/HudTheme.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "shared_contracts/TrainingStandby.hpp"
#include <atomic>
#include <mutex>

namespace cccaster::domain::ui::training_standby_view {
namespace {
training_standby::Channel channel;
training_standby::State state;
std::mutex viewMutex;
std::atomic<bool> active{false};
std::atomic<unsigned> keyAction{0};
bool initialized = false, armed = false, accept = false;
uint32_t previous = 0;
uint64_t openedAt = 0;
}
bool Active() { return active.load(); }
bool Key(unsigned key, bool repeat) {
    if (!Active()) return false;
    if (!repeat && (key == VK_F5 || key == VK_F6)) keyAction = key == VK_F5 ? 1 : 2;
    return true;
}
bool Step(game_interface::GameInput input) {
    if (!initialized) { initialized = true; channel.OpenEnvironment(); }
    training_standby::State next{};
    channel.Read(next);
    const auto now = GetTickCount64();
    const bool live = next.Live(now, std::time(nullptr));
    std::lock_guard lock(viewMutex);
    if (active && !live) session::DebugLog("[TrainingStandby] HIDE request=%s", state.request);
    if (next.generation != state.generation || (live && !active)) {
        armed = false; accept = false; previous = input.Pack(); openedAt = now; keyAction = 0;
        if (live) session::DebugLog("[TrainingStandby] SHOW request=%s name=%s", next.request, next.name);
    }
    state = next; active = live;
    if (!live) { keyAction = 0; return false; }
    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    if (foregroundPid != GetCurrentProcessId()) { armed = false; keyAction = 0; return true; }
    if (input.IsNeutral() && now - openedAt >= 300) armed = true;
    const auto keys = keyAction.exchange(0);
    const auto before = game_interface::GameInput::Unpack(previous);
    previous = input.Pack();
    if (!armed || state.reply) return true;
    if (input.direction != before.direction) {
        if (input.direction == 4) accept = true;
        if (input.direction == 6) accept = false;
    }
    const auto pressed = input.buttons & ~before.buttons;
    const unsigned decision = keys ? keys : (pressed & CC_BUTTON_B) ? 2 :
        (pressed & (CC_BUTTON_A | CC_BUTTON_CONFIRM)) ? (accept ? 1 : 2) : 0;
    if (decision && channel.Reply(state.generation, decision)) {
        state.reply = decision;
        session::DebugLog("[TrainingStandby] REPLY request=%s decision=%s", state.request, decision == 1 ? "ACCEPT" : "DECLINE");
    }
    return true;
}
bool Draw() {
    if (!Active()) return false;
    std::lock_guard lock(viewMutex);
    hud::Canvas c;
    c.Fill({0,0,640,480}, IM_COL32(0,0,0,145));
    c.Plate({90,145,460,190}, hud::Gold, true);
    c.Text(110,163,"INCOMING MATCH REQUEST",17,hud::Gold);
    c.Text(110,197,state.name,20,hud::White,0,420);
    c.Plate({110,241,198,36},hud::Green,accept);
    c.Plate({332,241,198,36},hud::Red,!accept);
    c.Text(129,250,"ACCEPT  [F5]",14,hud::White);
    c.Text(348,250,"DECLINE  [F6]",14,hud::White);
    c.Text(110,292,state.reply ? "SENDING REPLY..." : "LEFT / RIGHT: SELECT    A: OK    B: DECLINE",10,hud::Muted);
    c.Text(110,313,"Accepting closes training and starts a new game.",10,hud::Muted);
    return true;
}
}
