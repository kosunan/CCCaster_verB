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
#include "core_dll/mbaa_mem/NativeHud.hpp"
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

// 一つ前の名前表示と同じArial通常体・黒縁。配置は元ゲームの最終合成先へ合わせる。
void DrawIdentity(bool selection, bool namesOnly = false) {
    using namespace cccaster::hud;
    using Runner = cccaster::domain::session::SceneRunner;
    if (!HudDisplay::Visible() || StateUiLogic::IsMappingWindowOpen()) return;
    const auto viewport=CurrentViewport();
    const float scale=Layout::Fit(viewport).scale;
    const auto at=[&](float x,float y) {return ImVec2(viewport.x+x*viewport.width/640,viewport.y+y*viewport.height/480);};
    auto* draw=ImGui::GetForegroundDrawList();
    draw->PushClipRect(at(0,0),at(640,480),true);
    const auto text=[&](float x,float y,float area,const char* input,float size,unsigned role,int align) {
        auto* font=Font(role,scale);
        std::string value=input;
        const float available=area*viewport.width/640;
        const auto width=[&](const std::string& s) {return font->CalcTextSizeA(size*scale,100000,0,s.c_str()).x;};
        if(width(value)>available) {
            while(!value.empty() && width(value+"...")>available)value.pop_back();
            value+="..."; // 名前は共有契約でASCIIへ正規化済み。
        }
        auto pos=at(x,y);pos.x+=(available-width(value))*align/2;
        const float edge=(std::max)(1.f,scale);
        for(const auto offset:{ImVec2(-edge,0),ImVec2(edge,0),ImVec2(0,-edge),ImVec2(0,edge)})
            draw->AddText(font,size*scale,{pos.x+offset.x,pos.y+offset.y},IM_COL32(0,0,0,255),value.c_str());
        draw->AddText(font,size*scale,pos,IM_COL32(255,255,255,255),value.c_str());
    };
    const auto names=Runner::PlayerNames();
    for(unsigned side=0;side<2;++side) {
        const char* name=side ? names.p2.data() : names.p1.data();
        if(selection) {
            text(side ? 396 : 28,0,216,name,20,PlayerNameFont,side ? 0 : 2);
        } else text(side ? 394 : 30,0,216,name,22,PlayerNameFont,1);
        if(!namesOnly)if(const auto image=Emblem(side)) {
            const auto center=at(side ? 378 : 262,11);
            const ImVec2 a{center.x-12*scale,center.y-6*scale},b{center.x+12*scale,center.y+6*scale};
            draw->AddRectFilled({a.x-scale,a.y-scale},{b.x+scale,b.y+scale},IM_COL32(0,0,0,220));
            draw->AddImage(image,a,b); // 未設定時は何も表示しない。P1/P2の代替文字は不要。
        }
    }
    if(!namesOnly) {
        const auto score=Runner::Score();
        char value[12];
        std::snprintf(value,sizeof(value),"%u - %u",(std::min)(score.p1Wins,999u),(std::min)(score.p2Wins,999u));
        text(281,1,78,value,18,PlayerNameFont,1);
    }
    draw->PopClipRect();
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
void DrawSelectionGuidance(int delay, const char* notice = nullptr, bool showDetails = true, bool controls = true) {
    using namespace cccaster::hud;
    const Canvas c;
    const auto r = Layout::SelectionGuide;
    c.Plate(r,Blue,true);
    if(controls) {
        c.Key(18,r.y+6,24,"F4");
        c.Text(50,r.y+7,"CONTROLLER SETUP",10,White);
        c.Key(380,r.y+6,84,"START / F1");
        c.Text(476,r.y+7,cccaster::domain::scene::selection_options::menu.open ? "CLOSE MENU" : "OPEN MENU",10,White);
    }
    // 同じ濃紺の帯・白枠・直立した太字で、独立した青い箱にしない。
    const float center=controls ? 253.f : 320.f;
    if(controls)for(const float x:{214.f,292.f})c.draw->AddLine(c.At(x,r.y+1),c.At(x,r.y+r.height-1),Line,c.S(1));
    static DelayChangeHighlight highlight;
    const auto now=cccaster::platform::RealMonotonicUs();
    const bool changed=highlight.Observe(delay,now);
    char label[16];FormatDelayLabel(label,sizeof(label),delay);
    auto* font=Font(1,c.layout.scale);
    const float size=changed ? 12.f : 13.f;
    const float length=font->CalcTextSizeA(c.S(size),10000,0,label).x;
    auto pos=c.At(center,r.y+(changed ? 2 : 5));pos.x-=length/2;
    c.draw->AddText(font,c.S(size),pos,changed ? Gold : White,label);
    if(changed) {
        c.Rule(center-36,r.y+r.height-2,72,Gold);
        auto* small=Font(3,c.layout.scale);
        auto changedAt=c.At(center,r.y+17);
        changedAt.x-=small->CalcTextSizeA(c.S(7),10000,0,"CHANGED").x/2;
        c.draw->AddText(small,c.S(7),changedAt,Gold,"CHANGED");
    }
    static const bool trace=std::getenv("CCCASTER_TEST_SELECTION_OPTIONS")!=nullptr;
    static int previous=-1,previousHighlight=-1;
    if(trace && (previous!=delay || previousHighlight!=int(changed))) {
        cccaster::domain::session::DebugLog("[DelayHud] d=%d highlight=%u qpcUs=%lld hud=%s",delay,unsigned(changed),now,HudDisplay::Name());
        previous=delay;previousHighlight=int(changed);
    }
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
    const char* lines[]{state, "F1: FRAME BAR ON / OFF", delay};
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
    const auto appMode = Runner::AppMode();
    if (!selection && !cccaster::game_interface::native_hud::BattleHudReady()) return;
    if (!selection && appMode != 1 && !HudDisplay::Visible()) return;
    // Offline versus keeps only delay and controls during character selection.
    // HUD shortcuts must not bring names, scores or battle metrics back.
    if (appMode == 5 && !selection) return;
    if (appMode == 0 || appMode == 2 || appMode == 4) DrawIdentity(selection,appMode==4);
    if (appMode == 4) return;
    if (appMode == 2) {
        if(selection && HudDisplay::Visible())DrawSelectionGuidance(int(Runner::SpectatorStatus().delay),nullptr,false,false);
        return;
    }
    if (appMode != 0 && appMode != 1 && appMode != 5) return;
    const bool training = appMode == 1;
    const int d = selection ? Settings::delay.load() : StateUiLogic::GetDelay();
    if (training && !selection) {
        if (FrameBarDisplay::Visible(1))
            DrawTrainingControls(Runner::TrainingStateNotice(), cccaster::core::timer::WasapiClock::GetInstance().IsFallback());
        return;
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
