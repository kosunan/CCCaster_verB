#include "core_dll/ui/State_Ui_View.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/sync/SettingsCommands.hpp"
#include "core_dll/timing/FrameTiming.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include "core_dll/engine/SelectionOptions.hpp"
#include "core_dll/ui/HudResources.hpp"
#include "core_dll/ui/HudTheme.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <string>
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace cccaster::domain::ui {
namespace {
constexpr ImU32 normal = cccaster::hud::White;
constexpr ImU32 accent = cccaster::hud::Blue;
constexpr ImU32 warning = cccaster::hud::Gold;
constexpr ImU32 critical = cccaster::hud::Red;

LatencyWarningSnapshot ReadLatencyWarning(int delay, int rollback) {
    // 通信スレッドとの瞬間競合では直前の評価を維持し、警告を1Fだけ点滅させない。
    static LatencyWarningSnapshot cached{};
    const auto current = StateUiLogic::GetLatencyWarning(delay, rollback);
    if (current.evaluated)
        cached = current;
    return cached;
}

void DrawDelay(int delay, bool highlight = false) {
    using namespace cccaster::hud;
    const Canvas c;
    char settings[16]; FormatDelayLabel(settings,sizeof(settings),delay);
    const auto r = Layout::Settings;
    c.Plate(r,highlight ? Gold : Blue,true);
    if (highlight) {
        c.Fill({r.x+1,r.y+1,r.width-2,r.height-3},Gold);
        c.Plate({r.x+1,r.y+r.height+2,r.width-2,13},Gold);
        c.Text(r.x+6,r.y+r.height+4,"CHANGED",8,Gold,3,r.width-12);
    }
    auto* font = Font(1,c.layout.scale);
    const float width = font->CalcTextSizeA(c.S(Layout::SettingsSize),10000,0,settings).x;
    c.draw->AddText(font,c.S(Layout::SettingsSize),
        {c.At(320,r.y+2).x-width/2,c.At(0,r.y+2).y},highlight ? Ink : White,settings);
}

void DrawBattleIdentity(int delay, bool namesOnly = false, bool showDelay = true) {
    using namespace cccaster::hud;
    const auto names = cccaster::domain::session::SceneRunner::PlayerNames();
    const auto score = cccaster::domain::session::SceneRunner::Score();
    const Canvas c;
    const auto scale = c.layout.scale;
    c.draw->PushClipRect(c.At(0,0),c.At(640,480),true);
    for (unsigned player = 0; player < 2; ++player) {
        const bool right = player == 1;
        const auto color = right ? Blue : Red;
        const auto r = Layout::Player(player);
        c.Plate(r,color,true);
        const float emblemX = right ? r.x+r.width-28 : r.x+2;
        c.Fill({emblemX,17,26,13},Ink);
        if (const auto texture = Emblem(player))
            c.draw->AddImage(texture,c.At(emblemX+1,17),c.At(emblemX+25,29));
        else c.Text(emblemX+6,17,right ? "P2" : "P1",9,color);
        const auto wins = Layout::Wins(player);
        const float nameLeft = right ? (namesOnly ? r.x+8 : wins.x+wins.width+8) : r.x+34;
        const float nameRight = right ? emblemX-6 : (namesOnly ? r.x+r.width-8 : wins.x-8);
        const char* name = right ? names.p2.data() : names.p1.data();
        if (!*name) name = right ? "PLAYER 2" : "PLAYER 1";
        c.Text(right ? nameRight : nameLeft,16,name,Layout::NameSize,White,0,nameRight-nameLeft,right);
        if (!namesOnly) {
            char value[4];
            std::snprintf(value,sizeof(value),"%u",(std::min)(right ? score.p2Wins : score.p1Wins,999u));
            c.Fill(wins,right ? IM_COL32(13,62,95,255) : IM_COL32(78,25,48,255));
            const float edgeX = right ? wins.x+wins.width+2 : wins.x-2;
            c.draw->AddLine(c.At(edgeX,r.y+4),c.At(edgeX,r.y+r.height-4),color,(std::max)(1.f,scale));
            auto* font = Font(2,scale);
            const float width = font->CalcTextSizeA(c.S(18),10000,0,value).x;
            c.draw->AddText(font,c.S(18),{c.At(wins.x+wins.width/2,wins.y-1).x-width/2,c.At(0,wins.y-1).y},White,value);
        }
    }
    if (!namesOnly && showDelay) DrawDelay(delay);
    c.draw->PopClipRect();
}

void DrawBattleMetrics(const HudFixedValues &value, bool qpcFallback) {
    const auto layout = cccaster::hud::CurrentLayout();
    const float scale = layout.scale;
    auto *draw = ImGui::GetForegroundDrawList();
    auto *font = ImGui::GetFont();
    char line[192], ping[8], jitter[9], frame[10];
    std::snprintf(line, sizeof(line), "RTT %7s  JIT %8s  1F %9s%s",
                  value.ping, value.jitter, value.frame,
                  qpcFallback ? "  QPC" : "");
    std::snprintf(ping, sizeof(ping), "%7s", value.ping);
    std::snprintf(jitter, sizeof(jitter), "%8s", value.jitter);
    std::snprintf(frame, sizeof(frame), "%9s", value.frame);
    const float size = 9.0f * scale;
    const ImVec2 extent = font->CalcTextSizeA(size, 100000.0f, 0.0f, line);
    const float x = layout.X(320) - extent.x * 0.5f;
    const float y = layout.Y(480) - extent.y - 3.0f * scale;
    draw->AddRectFilled(ImVec2(x - 5.0f * scale, y - 1.0f * scale),
                        ImVec2(x + extent.x + 5.0f * scale, layout.Y(480)),
                        IM_COL32(8, 13, 30, 230));
    draw->AddLine(ImVec2(x - 5*scale,y-scale),ImVec2(x+extent.x+5*scale,y-scale),cccaster::hud::Line,scale);
    float partX = x;
    const auto part = [&](const char *text, ImU32 color) {
        draw->AddText(font, size, ImVec2(partX, y), color, text);
        partX += font->CalcTextSizeA(size, 100000.0f, 0.0f, text).x;
    };
    part("RTT ", normal); part(ping, accent);
    part("  JIT ", normal); part(jitter, accent);
    part("  1F ", normal); part(frame, accent);
    if (qpcFallback) part("  QPC", warning);
}

void DrawBattleLatencyWarning(const LatencyWarningSnapshot &latency) {
    if (latency.severity == LatencyWarningSeverity::None)
        return;
    const auto layout = cccaster::hud::CurrentLayout();
    const float scale = layout.scale;
    auto *draw = ImGui::GetForegroundDrawList();
    auto *font = ImGui::GetFont();
    char prefix[128], complete[192];
    std::snprintf(prefix, sizeof(prefix), "%s  RTT SPIKE %.0fms  |  ",
                  latency.severity == LatencyWarningSeverity::Heavy ? "SEVERE" : "WARNING",
                  latency.spikeRttMs);
    std::snprintf(complete, sizeof(complete), "%sCHECK CONNECTION", prefix);
    float size = 10.0f * scale;
    const float maxWidth = 616.0f * scale;
    ImVec2 extent = font->CalcTextSizeA(size, 100000.0f, 0.0f, complete);
    if (extent.x > maxWidth && extent.x > 0.0f) {
        size *= maxWidth / extent.x;
        extent = font->CalcTextSizeA(size, 100000.0f, 0.0f, complete);
    }
    const float x = layout.X(320) - extent.x * 0.5f;
    const float y = layout.Y(451);
    draw->AddRectFilled(ImVec2(x - 6.0f * scale, y - 2.0f * scale),
                        ImVec2(x + extent.x + 6.0f * scale, y + extent.y + 2.0f * scale),
                        IM_COL32(20, 10, 25, 240));
    const ImU32 severityColor = latency.severity == LatencyWarningSeverity::Heavy ? critical : warning;
    draw->AddText(font, size, ImVec2(x, y), severityColor, prefix);
    const float actionX = x + font->CalcTextSizeA(size, 100000.0f, 0.0f, prefix).x;
    draw->AddText(font, size, ImVec2(actionX, y), warning, "CHECK CONNECTION");
}


// 英語の短い案内。上部と同じ実ビューポート・フォント倍率を使う。
void DrawSelectionGuidance(int delay, const char* notice = nullptr, bool showDetails = true) {
    using namespace cccaster::hud;
    const Canvas c;
    const auto r = Layout::SelectionGuide;
    c.Plate(r,Blue,true);
    c.Key(18,r.y+6,24,"F4");
    c.Text(50,r.y+7,"CONTROLLER SETUP",10,White);
    c.Key(350,r.y+6,84,"START / F1");
    c.Text(446,r.y+7,cccaster::domain::scene::selection_options::menu.open ? "CLOSE MENU" : "OPEN MENU",10,White);
    // 常設の補助文を除き、保存直後や通信上の通知だけ一時表示する。
    const char* message = notice ? notice :
        HudDisplay::Detailed() && StateUiLogic::IsControllerConfirmationActive() ? "CONTROLLER READY" : nullptr;
    if (showDetails && HudDisplay::Detailed()) {
        // 詳細は警告だけでなく実測値を常設。左の通知と右の数値の領域を分ける。
        c.Fill({8,r.y-19,624,16},IM_COL32(8,13,30,220));
        if (message) c.Text(18,r.y-17,message,10,notice ? Gold : Green,3,246);
        const auto net = StateUiLogic::GetNetworkMetrics();
        const bool online = cccaster::domain::session::SceneRunner::AppMode() == 0;
        char metrics[128];
        FormatBattleHudLine(metrics,sizeof(metrics),online && net.available,net.stale,
            net.latestRttMs,online && net.jitterAvailable,net.jitterMs,delay,0,
            cccaster::core::timer::FrameTiming::Simulation().last,false);
        c.Text(622,r.y-17,metrics,9,White,3,348,true);
    }
}

void DrawTrainingControls(cccaster::domain::session::TrainingStateEvent event, bool fallback) {
    using Event = cccaster::domain::session::TrainingStateEvent;
    const bool recording = cccaster::game_interface::GameMem().IsTrainingRecording();
    const char* state = recording ? "FN1: SAVE | FN2: RESTART REC"
        : cccaster::game_interface::GameMem().IsTrainingDummy() ? "FN1: SAVE | FN2: LOAD"
        : cccaster::domain::session::SceneRunner::HasTrainingState() ? "FN1: SAVE | FN2: RESET+LOAD" : "FN1: SAVE | FN2: RESET";
    switch (event) {
    case Event::Saved: state = "STATE SAVED"; break;
    case Event::Holding: state = "SAVED - RELEASE FN1"; break;
    case Event::Loaded: state = recording ? "STATE LOADED / REC RESTARTED" : "STATE LOADED"; break;
    case Event::Empty: state = "NO STATE - PRESS FN1 TO SAVE"; break;
    case Event::SaveFailed: state = "SAVE FAILED - SLOT KEPT"; break;
    case Event::LoadFailed: state = "LOAD FAILED - SLOT KEPT"; break;
    case Event::Cleared: state = "SAVED STATE CLEARED"; break;
    case Event::RecordingRestarted: state = "RECORDING RESTARTED"; break;
    case Event::RecordingRestartFailed: state = "RECORDING RESTART FAILED"; break;
    default: break;
    }
    char delay[64];
    std::snprintf(delay, sizeof(delay), fallback ? "Ctrl+0-8: DELAY %d | QPC" : "Ctrl+0-8: DELAY %d", StateUiLogic::GetDelay());
    const char* lines[]{state, "F1: HUD / FRAME BAR", delay};
    const auto layout = cccaster::hud::CurrentLayout();
    auto r = cccaster::hud::Layout::TrainingGuide;
    const int rows = 3;
    const float scale = layout.scale;
    auto* draw = ImGui::GetForegroundDrawList();
    auto* font = cccaster::hud::Font(3, scale);
    const auto at = [&](float x, float y) { return ImVec2(layout.X(x), layout.Y(y)); };
    cccaster::hud::Canvas{}.Plate(r,cccaster::hud::Line);
    draw->PushClipRect(at(r.x, r.y), at(r.x + r.width, r.y + r.height), true);
    for (int row = 0; row < rows; ++row) {
        float size = cccaster::hud::Layout::GuideSize * scale;
        auto extent = font->CalcTextSizeA(size, 10000, 0, lines[row]);
        const float available = (r.width - 8) * scale;
        if (extent.x > available) { size *= available / extent.x; extent = font->CalcTextSizeA(size, 10000, 0, lines[row]); }
        const auto color = fallback || (row == 0 && (event == Event::Empty || event == Event::SaveFailed || event == Event::LoadFailed))
            ? warning : row == 0 ? accent : normal;
        draw->AddText(font, size, ImVec2(std::round(layout.X(320) - extent.x * .5f), layout.Y(r.y + 2 + row * 12)), color, lines[row]);
    }
    draw->PopClipRect();
}

void DrawHud(bool selection) {
    using Settings = cccaster::core::sync::SettingsCommands;
    using Runner = cccaster::domain::session::SceneRunner;
    // キャラ選択中の設定案内はHUDの表示モードにかかわらず残す。
    if (!selection && !HudDisplay::Visible()) return;
    const auto appMode = Runner::AppMode();
    // Offline versus keeps only delay and controls during character selection.
    // HUD shortcuts must not bring names, scores or battle metrics back.
    if (appMode == 5 && !selection) return;
    if (appMode == 4) {
        if (HudDisplay::Visible()) DrawBattleIdentity(0, true);
        return;
    }
    if (appMode == 2) {
        if (HudDisplay::Visible()) DrawBattleIdentity(Runner::SpectatorStatus().delay);
        return;
    }
    if (appMode != 0 && appMode != 1 && appMode != 5) return;
    const bool training = appMode == 1;
    const int d = selection ? Settings::delay.load() : StateUiLogic::GetDelay();
    static DelayChangeHighlight delayHighlight;
    const auto now = cccaster::platform::RealMonotonicUs();
    const bool highlighted = delayHighlight.Observe(d,now);
    if (appMode != 5 && HudDisplay::Visible()) DrawBattleIdentity(d, false, !selection);
    // キャラ選択ではHiddenでも残す。要求中の値ではなく、双方で確定したDを見る。
    if (selection) {
        DrawDelay(d,highlighted);
        static const bool trace = std::getenv("CCCASTER_TEST_SELECTION_OPTIONS") != nullptr;
        static int previousDelay = -1, previousHighlight = -1;
        if (trace && (previousDelay != d || previousHighlight != int(highlighted))) {
            cccaster::domain::session::DebugLog("[DelayHud] d=%d highlight=%u qpcUs=%lld hud=%s",
                d,unsigned(highlighted),now,HudDisplay::Name());
            previousDelay = d;
            previousHighlight = int(highlighted);
        }
    }
    if (selection && (appMode == 5 || !HudDisplay::Detailed())) {
        DrawSelectionGuidance(d, nullptr, appMode != 5);
        return;
    }
    if (!HudDisplay::Detailed()) return;

    const bool fallback = cccaster::core::timer::WasapiClock::GetInstance().IsFallback();
    if (training) {
        if (selection) DrawSelectionGuidance(d, fallback ? "CLOCK: QPC FALLBACK" : nullptr);
        else DrawTrainingControls(Runner::TrainingStateNotice(), fallback);
        return;
    }
    const int r = selection ? Settings::rollback.load() : StateUiLogic::GetRollback();
    const auto latency = ReadLatencyWarning(d, r);
    if (selection) {
        DrawSelectionGuidance(d, fallback ? "CLOCK: QPC FALLBACK" :
            latency.severity != LatencyWarningSeverity::None ? "RTT SPIKE: CHECK CONNECTION" : nullptr);
        return;
    }
    const auto net = StateUiLogic::GetNetworkMetrics();
    // 同じ完成画像の再提示間隔ではなく、通常ゲーム更新の実QPC間隔を表示する。
    const auto &timing = cccaster::core::timer::FrameTiming::Simulation();
    DrawBattleLatencyWarning(latency);
    DrawBattleMetrics(FormatHudFixedValues(net.available, net.stale, net.latestRttMs,
                                            net.jitterAvailable, net.jitterMs, d, r,
                                            timing.last), fallback);
}
} // namespace
void StateUiView::DrawCharaSelectBar() { DrawHud(true); }
void StateUiView::DrawInGameBar() { DrawHud(false); }
void StateUiView::DrawRematchBar() { /* 再戦画面にはHUDを重ねない。 */ }
void StateUiView::DrawDelayPopup() { DrawHud(true); }
void StateUiView::DrawRollbackPopup() { DrawHud(true); }
} // namespace cccaster::domain::ui
