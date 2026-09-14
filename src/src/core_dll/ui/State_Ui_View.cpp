#include "core_dll/ui/State_Ui_View.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/sync/SettingsCommands.hpp"
#include "core_dll/timing/FrameTiming.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include <imgui.h>
#include <algorithm>
#include <cstdio>

namespace cccaster::domain::ui {
namespace {
constexpr ImU32 normal = IM_COL32(222, 233, 243, 255);
constexpr ImU32 accent = IM_COL32(112, 231, 243, 255);
constexpr ImU32 warning = IM_COL32(255, 200, 116, 255);
constexpr ImU32 critical = IM_COL32(255, 96, 96, 255);
constexpr ImU32 border = IM_COL32(65, 91, 112, 220);

float HudScale() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    return std::max(0.1f, std::min(display.x / 640.0f, display.y / 480.0f));
}

LatencyWarningSnapshot ReadLatencyWarning(int delay, int rollback) {
    // 通信スレッドとの瞬間競合では直前の評価を維持し、警告を1Fだけ点滅させない。
    static LatencyWarningSnapshot cached{};
    const auto current = StateUiLogic::GetLatencyWarning(delay, rollback);
    if (current.evaluated)
        cached = current;
    return cached;
}

void DrawBattleIdentity(int delay, int rollback) {
    const auto names = cccaster::domain::session::SceneRunner::PlayerNames();
    const auto score = cccaster::domain::session::SceneRunner::Score();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = HudScale();
    auto *draw = ImGui::GetForegroundDrawList();
    auto *font = ImGui::GetFont();
    const float size = 12.0f * scale;
    const float y = 4.0f * scale;
    const float padX = 6.0f * scale;
    const float padY = 2.0f * scale;
    const auto drawName = [&](const char *text, float anchor, bool rightAligned, float maxNameWidth) {
        float textSize = size;
        const float measured = font->CalcTextSizeA(textSize, 100000.0f, 0.0f, text).x;
        if (measured > maxNameWidth && measured > 0)
            textSize *= maxNameWidth / measured;
        const ImVec2 extent = font->CalcTextSizeA(textSize, 100000.0f, 0.0f, text);
        const float x = rightAligned ? anchor - extent.x : anchor;
        draw->AddRectFilled(ImVec2(x - padX, y - padY),
                            ImVec2(x + extent.x + padX, y + extent.y + padY),
                            IM_COL32(8, 16, 27, 190), 3.0f * scale);
        draw->AddText(font, textSize, ImVec2(x + scale, y + scale), IM_COL32(0, 0, 0, 190), text);
        draw->AddText(font, textSize, ImVec2(x, y), normal, text);
    };

    char scoreText[32];
    std::snprintf(scoreText, sizeof(scoreText), "%u - %u",
                  std::min(score.p1Wins, 999u), std::min(score.p2Wins, 999u));
    const float scoreSize = 14.0f * scale;
    const ImVec2 extent = font->CalcTextSizeA(scoreSize, 100000.0f, 0.0f, scoreText);
    const float x = (display.x - extent.x) * 0.5f;
    draw->AddRectFilled(ImVec2(x - 8.0f * scale, y - padY),
                        ImVec2(x + extent.x + 8.0f * scale, y + extent.y + padY),
                        IM_COL32(8, 16, 27, 218), 3.0f * scale);
    draw->AddText(font, scoreSize, ImVec2(x, y), accent, scoreText);

    // 勝数は中央のまま、設定値をその右隣へ配置する。
    const auto values = FormatHudFixedValues(false, false, 0, false, 0, delay, rollback, 0);
    char settings[32];
    std::snprintf(settings, sizeof(settings), "D %s  R %s", values.delay, values.rollback);
    const float settingsSize = 9.0f * scale;
    const ImVec2 settingsExtent = font->CalcTextSizeA(settingsSize, 100000.0f, 0.0f, settings);
    const float settingsX = x + extent.x + 16.0f * scale;
    const float settingsY = y + (extent.y - settingsExtent.y) * 0.5f;
    draw->AddRectFilled(ImVec2(settingsX - 5.0f * scale, y - padY),
                        ImVec2(settingsX + settingsExtent.x + 5.0f * scale, y + extent.y + padY),
                        IM_COL32(8, 16, 27, 218), 3.0f * scale);
    draw->AddText(font, settingsSize, ImVec2(settingsX, settingsY), accent, settings);
    const float leftWidth = std::max(scale, std::min(220.0f * scale, x - 34.0f * scale));
    const float rightWidth = std::max(scale, std::min(220.0f * scale,
        display.x - settingsX - settingsExtent.x - 36.0f * scale));
    drawName(names.p1.data(), 12.0f * scale, false, leftWidth);
    drawName(names.p2.data(), display.x - 12.0f * scale, true, rightWidth);
}

void DrawBattleMetrics(const HudFixedValues &value, bool qpcFallback) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = HudScale();
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
    const float x = (display.x - extent.x) * 0.5f;
    const float y = display.y - extent.y - 3.0f * scale;
    draw->AddRectFilled(ImVec2(x - 5.0f * scale, y - 1.0f * scale),
                        ImVec2(x + extent.x + 5.0f * scale, display.y),
                        IM_COL32(8, 16, 27, 192), 2.0f * scale);
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
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float scale = HudScale();
    auto *draw = ImGui::GetForegroundDrawList();
    auto *font = ImGui::GetFont();
    char prefix[128], complete[192];
    std::snprintf(prefix, sizeof(prefix), "%s  RTT SPIKE %.0fms > D+R %.0fms  |  ",
                  latency.severity == LatencyWarningSeverity::Heavy ? "SEVERE" : "WARNING",
                  latency.spikeRttMs, latency.coverageMs);
    std::snprintf(complete, sizeof(complete), "%sINCREASE D OR RB LIMIT", prefix);
    float size = 10.0f * scale;
    const float maxWidth = display.x - 24.0f * scale;
    ImVec2 extent = font->CalcTextSizeA(size, 100000.0f, 0.0f, complete);
    if (extent.x > maxWidth && extent.x > 0.0f) {
        size *= maxWidth / extent.x;
        extent = font->CalcTextSizeA(size, 100000.0f, 0.0f, complete);
    }
    const float x = (display.x - extent.x) * 0.5f;
    const float y = display.y - 29.0f * scale;
    draw->AddRectFilled(ImVec2(x - 6.0f * scale, y - 2.0f * scale),
                        ImVec2(x + extent.x + 6.0f * scale, y + extent.y + 2.0f * scale),
                        IM_COL32(20, 10, 13, 220), 2.0f * scale);
    const ImU32 severityColor = latency.severity == LatencyWarningSeverity::Heavy ? critical : warning;
    draw->AddText(font, size, ImVec2(x, y), severityColor, prefix);
    const float actionX = x + font->CalcTextSizeA(size, 100000.0f, 0.0f, prefix).x;
    draw->AddText(font, size, ImVec2(actionX, y), warning, "INCREASE D OR RB LIMIT");
}

// 描画のみ。ImGui window/focus/capture を作らず、各行を実際の幅へ収める。
class Panel {
  public:
    Panel(bool selection, int rows, bool bottom = false) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        scale_ = std::min(display.x / 640.0f, display.y / 480.0f);
        scale_ = std::max(0.1f, scale_);
        const float width = std::min(display.x - 16 * scale_, 624 * scale_);
        left_ = (display.x - width) * 0.5f;
        width_ = width;
        const float height = (rows * 16 + 12) * scale_;
        // キャラ選択・再戦では上端、対戦中は体力と下端ゲージの間へ置く。
        top_ = bottom ? std::max(0.0f, display.y - height - 8.0f * scale_)
                      : selection ? 6 * scale_ : std::max(0.0f, display.y - 64 * scale_ - height);
        draw_ = ImGui::GetForegroundDrawList();
        draw_->AddRectFilled(ImVec2(left_, top_), ImVec2(left_ + width_, top_ + height),
                             IM_COL32(10, 18, 29, 206), 4 * scale_);
        draw_->AddRect(ImVec2(left_, top_), ImVec2(left_ + width_, top_ + height), border,
                       4 * scale_, 0, std::max(1.0f, scale_));
        draw_->AddRectFilled(ImVec2(left_, top_ + 3 * scale_),
                             ImVec2(left_ + 3 * scale_, top_ + height - 3 * scale_), accent);
        draw_->PushClipRect(ImVec2(left_, top_), ImVec2(left_ + width_, top_ + height), true);
    }
    ~Panel() { draw_->PopClipRect(); }
    void Row(const char *value, ImU32 color = normal) {
        auto *font = ImGui::GetFont();
        float size = 12.0f * scale_;
        const float measured = font->CalcTextSizeA(size, 100000.0f, 0.0f, value).x;
        const float available = width_ - 20 * scale_;
        if (measured > available && measured > 0) size *= available / measured;
        draw_->AddText(font, size, ImVec2(left_ + 10 * scale_, top_ + (6 + row_ * 16) * scale_),
                       color, value);
        ++row_;
    }
    void MetricRow(const HudFixedValues &value, bool qpcFallback = false) {
        auto *font = ImGui::GetFont();
        char complete[256];
        std::snprintf(complete, sizeof(complete),
                      "RTT %7s   JITTER %8s   D %2s   RB LIMIT %2s   1F %9s%s",
                      value.ping, value.jitter, value.delay, value.rollback, value.frame,
                      qpcFallback ? "   CLOCK QPC" : "");
        float size = 12.0f * scale_;
        const float measured = font->CalcTextSizeA(size, 100000.0f, 0.0f, complete).x;
        const float available = width_ - 20 * scale_;
        if (measured > available && measured > 0) size *= available / measured;
        float x = left_ + 10 * scale_;
        const float y = top_ + (6 + row_ * 16) * scale_;
        const auto part = [&](const char *text, ImU32 color) {
            draw_->AddText(font, size, ImVec2(x, y), color, text);
            x += font->CalcTextSizeA(size, 100000.0f, 0.0f, text).x;
        };
        char ping[8], jitter[9], delay[3], rollback[3], frame[10];
        std::snprintf(ping, sizeof(ping), "%7s", value.ping);
        std::snprintf(jitter, sizeof(jitter), "%8s", value.jitter);
        std::snprintf(delay, sizeof(delay), "%2s", value.delay);
        std::snprintf(rollback, sizeof(rollback), "%2s", value.rollback);
        std::snprintf(frame, sizeof(frame), "%9s", value.frame);
        part("RTT ", normal); part(ping, accent);
        part("   JITTER ", normal); part(jitter, accent);
        part("   D ", normal); part(delay, accent);
        part("   RB LIMIT ", normal); part(rollback, accent);
        part("   1F ", normal); part(frame, accent);
        if (qpcFallback) part("   CLOCK QPC", warning);
        ++row_;
    }
    void CompactSettingsRow(const HudFixedValues &value, bool qpcFallback = false) {
        auto *font = ImGui::GetFont();
        char complete[256];
        std::snprintf(complete, sizeof(complete),
                      "RTT %s  JIT %s  D %s  R %s  1F %s  |  Ctrl+0-8: D  Alt+0-8: R%s",
                      value.ping, value.jitter, value.delay, value.rollback, value.frame,
                      qpcFallback ? "  |  CLOCK QPC" : "");
        float size = 12.0f * scale_;
        const float measured = font->CalcTextSizeA(size, 100000.0f, 0.0f, complete).x;
        const float available = width_ - 20 * scale_;
        if (measured > available && measured > 0.0f) size *= available / measured;
        float x = left_ + 10 * scale_;
        const float y = top_ + (6 + row_ * 16) * scale_;
        const auto part = [&](const char *text, ImU32 color) {
            draw_->AddText(font, size, ImVec2(x, y), color, text);
            x += font->CalcTextSizeA(size, 100000.0f, 0.0f, text).x;
        };
        part("RTT ", normal); part(value.ping, accent);
        part("  JIT ", normal); part(value.jitter, accent);
        part("  D ", normal); part(value.delay, accent);
        part("  R ", normal); part(value.rollback, accent);
        part("  1F ", normal); part(value.frame, accent);
        part("  |  Ctrl+0-8: D  Alt+0-8: R", normal);
        if (qpcFallback) part("  |  CLOCK QPC", warning);
        ++row_;
    }
    void LatencyWarningRow(const LatencyWarningSnapshot &latency, bool qpcFallback = false) {
        auto *font = ImGui::GetFont();
        char prefix[160], complete[256];
        std::snprintf(prefix, sizeof(prefix), "%s  RTT SPIKE %.0fms > D+R %.0fms  |  ",
                      latency.severity == LatencyWarningSeverity::Heavy ? "SEVERE" : "WARNING",
                      latency.spikeRttMs, latency.coverageMs);
        std::snprintf(complete, sizeof(complete), "%sINCREASE D OR RB LIMIT%s", prefix,
                      qpcFallback ? "  |  CLOCK QPC" : "");
        float size = 12.0f * scale_;
        const float measured = font->CalcTextSizeA(size, 100000.0f, 0.0f, complete).x;
        const float available = width_ - 20 * scale_;
        if (measured > available && measured > 0.0f) size *= available / measured;
        float x = left_ + 10 * scale_;
        const float y = top_ + (6 + row_ * 16) * scale_;
        const auto part = [&](const char *text, ImU32 color) {
            draw_->AddText(font, size, ImVec2(x, y), color, text);
            x += font->CalcTextSizeA(size, 100000.0f, 0.0f, text).x;
        };
        part(prefix, latency.severity == LatencyWarningSeverity::Heavy ? critical : warning);
        part("INCREASE D OR RB LIMIT", warning);
        if (qpcFallback) part("  |  CLOCK QPC", warning);
        ++row_;
    }
  private:
    ImDrawList *draw_;
    float scale_, left_, top_, width_;
    int row_ = 0;
};

void DrawHud(bool selection) {
    using Settings = cccaster::core::sync::SettingsCommands;
    const auto mode = HudDisplay::Get();
    const bool fallback = cccaster::core::timer::WasapiClock::GetInstance().IsFallback();
    const auto appMode = cccaster::domain::session::SceneRunner::AppMode();
    if (appMode == 2) {
        const auto s = cccaster::domain::session::SceneRunner::SpectatorStatus();
        if (selection || !FrameBarDisplay::Visible(appMode)) DrawBattleIdentity(s.delay, s.rollback);
        return;
    }
    if (appMode != 0) {
        if (appMode != 1 && appMode != 2) return;
        if (selection) {
            DrawBattleIdentity(StateUiLogic::GetDelay(), StateUiLogic::GetRollback());
            Panel panel(true, 2, true);
            panel.Row(ControllerSetupGuidance(StateUiLogic::IsControllerConfirmationActive()),
                      StateUiLogic::IsControllerConfirmationActive() ? accent : warning);
            panel.Row(appMode == 1 ? "OFFLINE TRAINING  |  F4: CONTROLLER  |  Ctrl+F3: DISPLAY"
                                   : "SPECTATOR  |  Ctrl+F3: DISPLAY",
                      fallback ? warning : normal);
          } else if (appMode == 1) {
              using Runner = cccaster::domain::session::SceneRunner;
              using Event = cccaster::domain::session::TrainingStateEvent;
              const auto event = Runner::TrainingStateNotice();
              if (mode == HudDisplayMode::Hidden && event == Event::None && !fallback) return;
              const char *text = event == Event::None
                  ? (Runner::HasTrainingState() ? "FN1: SAVE  |  FN2: RESET + LOAD" : "FN1: SAVE  |  FN2: RESET")
                  : cccaster::domain::session::TrainingState::Text(event);
              char line[160];
              std::snprintf(line, sizeof(line), "%s%s", text, fallback ? "  |  CLOCK: QPC FALLBACK" : "");
              const auto display = ImGui::GetIO().DisplaySize;
              const auto scale = HudScale();
              auto *font = ImGui::GetFont();
              const float size = 10.0f * scale;
              const auto extent = font->CalcTextSizeA(size, 10000, 0, line);
              const ImVec2 pos((display.x - extent.x) * .5f, display.y - extent.y - 3 * scale);
              auto *draw = ImGui::GetForegroundDrawList();
              draw->AddRectFilled(ImVec2(pos.x - 5 * scale, pos.y - 2 * scale),
                  ImVec2(pos.x + extent.x + 5 * scale, display.y), IM_COL32(8, 16, 27, 205), 2 * scale);
              draw->AddText(font, size, pos,
                  fallback || event == Event::Empty || event == Event::SaveFailed || event == Event::LoadFailed
                    ? warning : accent, line);
          } else if (fallback) {
            Panel panel(selection, 1);
            panel.Row("CLOCK: QPC FALLBACK (WASAPI unavailable)", warning);
        }
        return;
    }
    const int d = selection ? Settings::delay.load() : StateUiLogic::GetDelay();
    const int r = selection ? Settings::rollback.load() : StateUiLogic::GetRollback();
    const auto latency = ReadLatencyWarning(d, r);
    const char *notice = nullptr;
    if (selection) {
        if (Settings::pending.load()) notice = "SYNCING SETTINGS...";
        else if (Settings::notice.load() == 1) notice = "SETTINGS: D + R MUST BE <= 8";
        else if (Settings::notice.load() == 3) notice = "SETTINGS: NOT APPLIED";
        else if (d != StateUiLogic::GetDelay() || r != StateUiLogic::GetRollback())
            notice = "SETTINGS: APPLY TO NEXT MATCH";
    }
    // 非表示でも反映中/不採用の重要通知は消さない。
    if (mode == HudDisplayMode::Hidden && !selection) {
        if (notice || fallback) {
            Panel panel(selection, (notice ? 1 : 0) + (fallback ? 1 : 0));
            if (notice) panel.Row(notice, warning);
            if (fallback) panel.Row("CLOCK: QPC FALLBACK (WASAPI unavailable)", warning);
        }
        DrawBattleLatencyWarning(latency);
        return;
    }
    const auto net = StateUiLogic::GetNetworkMetrics();
    const auto &timing = cccaster::core::timer::FrameTiming::Get();
    if (selection) {
        // Panelのクリップ範囲に上端の名前・勝数・D/Rを巻き込まない。
        DrawBattleIdentity(d, r);
        Panel panel(true, 2, true);
        const bool confirmed = StateUiLogic::IsControllerConfirmationActive();
        panel.Row(ControllerSetupGuidance(confirmed),
                  confirmed ? accent : warning);
        if (notice)
            panel.Row(notice, warning);
        else if (latency.severity != LatencyWarningSeverity::None)
            panel.LatencyWarningRow(latency, fallback);
        else
            panel.Row(fallback ? "Ctrl+0-8: DELAY  |  Alt+0-8: ROLLBACK  |  CLOCK QPC"
                               : "Ctrl+0-8: DELAY  |  Alt+0-8: ROLLBACK", fallback ? warning : normal);
        return;
    }
    DrawBattleIdentity(d, r);
    DrawBattleLatencyWarning(latency);
    DrawBattleMetrics(FormatHudFixedValues(net.available, net.stale, net.latestRttMs,
                                            net.jitterAvailable, net.jitterMs, d, r,
                                            timing.last), fallback);
}
} // namespace
void StateUiView::DrawCharaSelectBar() { DrawHud(true); }
void StateUiView::DrawInGameBar() { DrawHud(false); }
void StateUiView::DrawRematchBar() {
    if (cccaster::domain::session::SceneRunner::AppMode() == 2) {
        DrawHud(true);
        return;
    }
    const auto mode = HudDisplay::Get();
    const bool fallback = cccaster::core::timer::WasapiClock::GetInstance().IsFallback();
    if (mode == HudDisplayMode::Hidden && !fallback) return;
    // 再戦の選択肢は中央にあるため、通常HUDと同じ1行だけを上端へ表示する。
    Panel panel(true, 1);
    if (mode != HudDisplayMode::Hidden) {
        const auto net = StateUiLogic::GetNetworkMetrics();
        const auto &timing = cccaster::core::timer::FrameTiming::Get();
        panel.MetricRow(FormatHudFixedValues(net.available, net.stale, net.latestRttMs,
                                             net.jitterAvailable, net.jitterMs,
                                             StateUiLogic::GetDelay(), StateUiLogic::GetRollback(),
                                             timing.last), fallback);
    } else if (fallback)
        panel.Row("CLOCK: QPC FALLBACK (WASAPI unavailable)", warning);
}
void StateUiView::DrawDelayPopup() { DrawHud(true); }
void StateUiView::DrawRollbackPopup() { DrawHud(true); }
} // namespace cccaster::domain::ui
