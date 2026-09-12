// ============================================================================
// ControllerMapper.cpp — コントローラーマッピングUI 実装
// ============================================================================
//
// 【概要】
//   F4キーで開閉するコントローラー設定画面。以下の3つのサブ処理で構成:
//   1. デバイス選択入力処理: ジョイスティック方向/キーボード矢印でデバイスをP1/P2に割当
//   2. バインド入力処理: 13種のゲーム入力に対して順次キー/ボタンをバインド
//   3. UI描画: メインテーブル（デバイス3列）+ P1/P2個別バインドリスト
//
// 【関数構成（リファクタリング後）】
//   Draw()                    : メインエントリ。各サブ処理を順に呼び出す。
//   ProcessDeviceSelectionInput() : デバイス選択フェーズの入力処理
//   ProcessBindingInput()     : バインドフェーズの入力処理
//   SaveBinds()               : バインド結果をINIに保存
//   DrawDeviceSelectionTable(): デバイス選択テーブルを描画
//   DrawP1BindingWindow()     : P1バインドウィンドウを描画
//   DrawP2BindingWindow()     : P2バインドウィンドウを描画
//   DrawBindList()            : 1人分のバインドリストを描画するユーティリティ
// ============================================================================

#include "core_dll/ui/ControllerMapper.hpp"
#include "core_dll/ui/OverlayRenderer.hpp"
#include "core_dll/hook/DirectInputHook.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include <imgui.h>
#include <string>
#include <cmath>

using namespace cccaster::domain::ui;
using cccaster::overlay::OverlayRenderer;

// ============================================================================
// ファイルスコープ状態変数
// ============================================================================

/// P1に割り当てられたジョイスティックID (-2=キーボード, -1=未割当, 0+=デバイスindex)
static int g_p1JoyId = -1;
/// P2に割り当てられたジョイスティックID
static int g_p2JoyId = -1;

/// P1/P2のバインドステップ位置 (0=未開始, 1-13=入力中, 14=完了確認)
static int g_p1OverlayPosition = 0;
static int g_p2OverlayPosition = 0;

/// P1/P2の各ゲーム入力に割り当てられたキー/ボタン名文字列
static std::string g_p1Binds[ControllerMapper::NUM_GAME_INPUTS];
static std::string g_p2Binds[ControllerMapper::NUM_GAME_INPUTS];

/// P1/P2バインド開始時刻 [秒]。ImGui::GetTime()基準。
static double p1BindStartTime = 0.0;
static double p2BindStartTime = 0.0;

/// Edge入力キャッシュ（#8 修正: Draw() で1度だけ取得し ProcessBindingInput でも参照）
static std::string g_p1CachedEdge;
static std::string g_p2CachedEdge;

/// ゲーム入力名の表示ラベル配列
static const char *const gameInputNames[] = {"Up", "Down", "Left",  "Right", "A (confirm)", "B (cancel)", "C",
                                             "D",  "E",    "Start", "FN1",   "FN2",         "A+B"};

// ============================================================================
// 共通ヘルパー: デバイス名サニタイズ
// ============================================================================

static std::string SanitizeDeviceName(const std::string &name) {
    std::string sanitized = name;
    for (char &c : sanitized) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' ||
            c == '|')
            c = '_';
    }
    return sanitized;
}

// ============================================================================
// 共通ヘルパー: デバイスIDから名前を取得
// ============================================================================

static std::string GetDeviceNameById(int joyId) {
    if (joyId == -2)
        return "Keyboard";
    if (joyId >= 0) {
        auto devices = cccaster::game_interface::DirectInputHook::GetConnectedDevices();
        for (const auto &dev : devices) {
            if (dev.id == joyId)
                return dev.name;
        }
    }
    return "";
}

// ============================================================================
// デバイス割当保存
// ============================================================================

void ControllerMapper::SaveDeviceAllocations() {
    auto saveAssignedDev = [](int joyId, const std::string &prefix) {
        std::string deviceName = GetDeviceNameById(joyId);
        if (!deviceName.empty()) {
            cccaster::main_app::ConfigManager::SetString("Settings", prefix + "Device",
                                                         SanitizeDeviceName(deviceName));
        } else {
            cccaster::main_app::ConfigManager::SetString("Settings", prefix + "Device", "");
        }
    };
    saveAssignedDev(g_p1JoyId, "P1");
    saveAssignedDev(g_p2JoyId, "P2");
    cccaster::main_app::ConfigManager::Save("cccaster\\cccaster_v10.ini");
    cccaster::game_interface::DirectInputHook::ReloadConfigs();
}

// ============================================================================
// バインド保存（プライベートヘルパー）
// ============================================================================

void ControllerMapper::SaveBinds(int joyId, const std::string &prefix, std::string *binds) {
    std::string deviceName = GetDeviceNameById(joyId);
    if (deviceName.empty())
        deviceName = "UnknownDevice_" + std::to_string(joyId);

    std::string sanitizedName = SanitizeDeviceName(deviceName);
    std::string filename = "cccaster\\" + sanitizedName + ".ini";
    cccaster::main_app::Config deviceConfig;
    deviceConfig.Load(filename);

    deviceConfig.SetString("Mapping", "Up", binds[0]);
    deviceConfig.SetString("Mapping", "Down", binds[1]);
    deviceConfig.SetString("Mapping", "Left", binds[2]);
    deviceConfig.SetString("Mapping", "Right", binds[3]);
    deviceConfig.SetString("Mapping", "A", binds[4]);
    deviceConfig.SetString("Mapping", "B", binds[5]);
    deviceConfig.SetString("Mapping", "C", binds[6]);
    deviceConfig.SetString("Mapping", "D", binds[7]);
    deviceConfig.SetString("Mapping", "E", binds[8]);
    deviceConfig.SetString("Mapping", "Start", binds[9]);
    deviceConfig.SetString("Mapping", "FN1", binds[10]);
    deviceConfig.SetString("Mapping", "FN2", binds[11]);
    deviceConfig.SetString("Mapping", "A+B", binds[12]);

    deviceConfig.Save(filename);
    cccaster::main_app::ConfigManager::SetString("Settings", prefix + "Device", sanitizedName);
}

// ============================================================================
// デバイス選択入力処理（プライベートヘルパー）
// ============================================================================

void ControllerMapper::ProcessDeviceSelectionInput() {
    int activeJoyId = -1;
    int dir = cccaster::game_interface::DirectInputHook::GetActiveDeviceDirection(activeJoyId);

    // ジョイスティックの左右傾きでP1/P2に割当
    if (g_p1OverlayPosition == 0 && dir == -1 && activeJoyId >= 0) {
        if (g_p2JoyId == activeJoyId) {
            if (g_p2OverlayPosition == 0)
                g_p2JoyId = -1;
        } else if (g_p1JoyId != activeJoyId)
            g_p1JoyId = activeJoyId;
    } else if (g_p2OverlayPosition == 0 && dir == 1 && activeJoyId >= 0) {
        if (g_p1JoyId == activeJoyId) {
            if (g_p1OverlayPosition == 0)
                g_p1JoyId = -1;
        } else if (g_p2JoyId != activeJoyId)
            g_p2JoyId = activeJoyId;
    }

    // キーボード矢印キーでキーボードをP1/P2に割当
    if (g_p1OverlayPosition == 0 && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
        if (g_p2JoyId == -2) {
            if (g_p2OverlayPosition == 0)
                g_p2JoyId = -1;
        } else if (g_p1JoyId != -2)
            g_p1JoyId = -2;
    }
    if (g_p2OverlayPosition == 0 && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        if (g_p1JoyId == -2) {
            if (g_p1OverlayPosition == 0)
                g_p1JoyId = -1;
        } else if (g_p2JoyId != -2)
            g_p2JoyId = -2;
    }
}

// ============================================================================
// バインド入力処理（プライベートヘルパー）
// ============================================================================

void ControllerMapper::ProcessBindingInput(int joyId, int playerIndex, int &pos, std::string *binds) {
    if (pos == 0)
        return;

    // F4 はバインド対象キーから除外（ウィンドウ閉じは OnMappingInput() が一元管理）
    // → ProcessBindingInput では何もしない

    // バインド開始直後のデッドタイム: 意図しない入力を無視する
    double bindStartTime = (playerIndex == 0) ? p1BindStartTime : p2BindStartTime;
    if (ImGui::GetTime() - bindStartTime < MAPPING_START_DEAD_TIME)
        return;

    int maxPos = NUM_GAME_INPUTS + 1;
    int bindIndex = pos - 1;

    bool deleteBind = ImGui::IsKeyPressed(ImGuiKey_Delete);
    std::string newBind = "";

    if (joyId == -2) {
        // キーボードからの入力を取得
        if (!deleteBind && !ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key) {
                if (ImGui::IsKeyPressed((ImGuiKey)key)) {
                    if (pos == maxPos && (key == ImGuiKey_Enter || key == ImGuiKey_Space)) {
                        SaveBinds(joyId, playerIndex == 0 ? "P1" : "P2", binds);
                        cccaster::main_app::ConfigManager::Save("cccaster\\cccaster_v10.ini");
                        pos = 0;
                        return;
                    }
                    if (key != ImGuiKey_Enter && key != ImGuiKey_F4 && key != ImGuiKey_Escape) {
                        std::string kName = ImGui::GetKeyName((ImGuiKey)key);
                        if (kName.find("Gamepad") == std::string::npos &&
                            kName.find("Mouse") == std::string::npos) {
                            newBind = kName;
                            break;
                        }
                    }
                }
            }
        }
    } else if (joyId >= 0) {
        // ジョイスティックからの入力を取得（キャッシュ済み）
        const std::string &edge = (playerIndex == 0) ? g_p1CachedEdge : g_p2CachedEdge;
        if (!edge.empty()) {
            if (pos == maxPos && edge.find("H") == std::string::npos && edge.find("A") == std::string::npos) {
                SaveBinds(joyId, playerIndex == 0 ? "P1" : "P2", binds);
                cccaster::main_app::ConfigManager::Save("cccaster\\cccaster_v10.ini");
                pos = 0;
                return;
            }
            if (pos < maxPos)
                newBind = edge;
        }
    }

    if (deleteBind && pos < maxPos)
        binds[bindIndex] = "";
    if (!newBind.empty() && pos < maxPos) {
        binds[bindIndex] = newBind;
        pos++;
    }
}

// ============================================================================
// バインドリスト描画ユーティリティ（プライベートヘルパー）
// ============================================================================

void ControllerMapper::DrawBindList(int pos, std::string *binds, ImVec4 hiliteColor, bool rightAlign) {
    for (int i = 0; i < NUM_GAME_INPUTS; ++i) {
        int currentListIndex = i + 1;
        bool isSelected = (currentListIndex == pos);
        std::string val = binds[i].empty() ? "..." : binds[i];
        ImVec4 normalCol = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
        ImVec4 textCol = isSelected ? hiliteColor : normalCol;

        if (isSelected)
            ImGui::SetWindowFontScale(1.1f);

        if (!rightAlign) {
            std::string prefix = isSelected ? "> " : "  ";
            ImGui::TextColored(textCol, "%s%-12s : %s", prefix.c_str(), gameInputNames[i], val.c_str());
        } else {
            std::string suffix = isSelected ? " <" : "  ";
            std::string rowText = val + " : " + gameInputNames[i] + suffix;
            float tw = ImGui::CalcTextSize(rowText.c_str()).x;
            ImGui::SetCursorPosX(ImGui::GetColumnWidth() - tw);
            ImGui::TextColored(textCol, "%s", rowText.c_str());
        }

        if (isSelected)
            ImGui::SetWindowFontScale(1.0f);
        else
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 1.0f);
    }

    // 「Finish and Save」行
    bool doneSelected = (pos == NUM_GAME_INPUTS + 1);
    ImVec4 doneColor = doneSelected ? hiliteColor : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
    if (doneSelected)
        ImGui::SetWindowFontScale(1.1f);

    if (!rightAlign) {
        std::string prefix = doneSelected ? "> " : "  ";
        ImGui::TextColored(doneColor, "%sFinish and Save", prefix.c_str());
    } else {
        std::string suffix = doneSelected ? " <" : "  ";
        std::string rowText = std::string("Finish and Save") + suffix;
        float tw = ImGui::CalcTextSize(rowText.c_str()).x;
        ImGui::SetCursorPosX(ImGui::GetColumnWidth() - tw);
        ImGui::TextColored(doneColor, "%s", rowText.c_str());
    }

    if (doneSelected)
        ImGui::SetWindowFontScale(1.0f);
}

// ============================================================================
// デバイス選択テーブル描画（プライベートヘルパー）
// ============================================================================

void ControllerMapper::DrawDeviceSelectionTable() {
    auto devices = cccaster::game_interface::DirectInputHook::GetConnectedDevices();

    static ImGuiTableFlags table_flags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;

    if (ImGui::BeginTable("DeviceSelectionTable", 3, table_flags)) {
        ImGui::TableSetupColumn("P1", ImGuiTableColumnFlags_WidthStretch, 0.35f);
        ImGui::TableSetupColumn("List", ImGuiTableColumnFlags_WidthStretch, 0.3f);
        ImGui::TableSetupColumn("P2", ImGuiTableColumnFlags_WidthStretch, 0.35f);

        // ヘッダ行
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "PLAYER 1 CONTROLLER");

        ImGui::TableSetColumnIndex(1);
        const char *titleMid = "AVAILABLE DEVICES";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (ImGui::GetColumnWidth() - ImGui::CalcTextSize(titleMid).x) * 0.5f);
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", titleMid);

        ImGui::TableSetColumnIndex(2);
        const char *titleP2 = "PLAYER 2 CONTROLLER";
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() -
                             ImGui::CalcTextSize(titleP2).x - 10.0f);
        ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "%s", titleP2);

        // 区切り線
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::Separator();
        ImGui::TableSetColumnIndex(1);
        ImGui::Separator();
        ImGui::TableSetColumnIndex(2);
        ImGui::Separator();

        // コンテンツ行
        ImGui::TableNextRow();

        // IDで名前を逆引きするヘルパー（ID≠vectorインデックスなので添え字アクセスは禁止）
        auto findDevName = [&](int id) -> const char * {
            for (const auto &d : devices) {
                if (d.id == id)
                    return d.name;
            }
            return nullptr;
        };

        // P1列
        ImGui::TableSetColumnIndex(0);
        ImGui::Spacing();
        const char *p1Name = findDevName(g_p1JoyId);
        if (p1Name) {
            OverlayRenderer::DrawFittedText(p1Name, ImVec4(1, 1, 1, 1), false);
            if (g_p1OverlayPosition > 0)
                OverlayRenderer::DrawFittedText("(Binding...)", ImVec4(1.0f, 0.8f, 0.2f, 1.0f), false);
        } else if (g_p1JoyId == -2) {
            OverlayRenderer::DrawFittedText("ASCII Keyboard", ImVec4(1, 1, 1, 1), false);
            if (g_p1OverlayPosition > 0)
                OverlayRenderer::DrawFittedText("(Binding...)", ImVec4(1.0f, 0.8f, 0.2f, 1.0f), false);
        } else {
            float alpha = 0.5f + 0.5f * sinf(ImGui::GetTime() * 5.0f);
            OverlayRenderer::DrawFittedText("< PRESS LEFT TO ASSIGN", ImVec4(1.0f, 0.4f, 0.4f, alpha), false);
        }

        // 中央列（利用可能デバイス一覧）
        ImGui::TableSetColumnIndex(1);
        ImGui::Spacing();
        if (g_p1JoyId != -2 && g_p2JoyId != -2)
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  Keyboard");
        for (const auto &dev : devices) {
            if (dev.id != g_p1JoyId && dev.id != g_p2JoyId)
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "  %s", dev.name);
        }

        // P2列
        ImGui::TableSetColumnIndex(2);
        ImGui::Spacing();
        const char *p2Name = findDevName(g_p2JoyId);
        if (p2Name) {
            OverlayRenderer::DrawFittedText(p2Name, ImVec4(1, 1, 1, 1), true);
            if (g_p2OverlayPosition > 0)
                OverlayRenderer::DrawFittedText("(Binding...)", ImVec4(0.2f, 0.9f, 1.0f, 1.0f), true);
        } else if (g_p2JoyId == -2) {
            OverlayRenderer::DrawFittedText("ASCII Keyboard", ImVec4(1, 1, 1, 1), true);
            if (g_p2OverlayPosition > 0)
                OverlayRenderer::DrawFittedText("(Binding...)", ImVec4(0.2f, 0.9f, 1.0f, 1.0f), true);
        } else {
            float alpha = 0.5f + 0.5f * sinf(ImGui::GetTime() * 5.0f);
            OverlayRenderer::DrawFittedText("PRESS RIGHT TO ASSIGN >", ImVec4(0.4f, 0.6f, 1.0f, alpha), true);
        }
        ImGui::EndTable();
    }
}

// ============================================================================
// P1バインドウィンドウ描画（プライベートヘルパー）
// ============================================================================

void ControllerMapper::DrawP1BindingWindow(float screenWidth) {
    if (g_p1OverlayPosition == 0)
        return;

    ImGui::SetNextWindowSize(ImVec2(MAPPING_WINDOW_WIDTH * 0.46f, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2((screenWidth - MAPPING_WINDOW_WIDTH) * 0.5f, -2.0f + 140.0f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGuiWindowFlags bindFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    OverlayRenderer::PushModernStyle();

    if (ImGui::Begin("P1 Binding", nullptr, bindFlags)) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "PLAYER 1 MAPPING");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTable("P1MappingTable", 1, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("P1", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawBindList(g_p1OverlayPosition, g_p1Binds, ImVec4(1.0f, 0.8f, 0.2f, 1.0f), false);
            ImGui::EndTable();
        }
    }
    ImGui::End();
    OverlayRenderer::PopModernStyle();
}

// ============================================================================
// P2バインドウィンドウ描画（プライベートヘルパー）
// ============================================================================

void ControllerMapper::DrawP2BindingWindow(float screenWidth) {
    if (g_p2OverlayPosition == 0)
        return;

    ImGui::SetNextWindowSize(ImVec2(MAPPING_WINDOW_WIDTH * 0.46f, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(
        ImVec2((screenWidth - MAPPING_WINDOW_WIDTH) * 0.5f + (MAPPING_WINDOW_WIDTH * 0.54f), -2.0f + 140.0f),
        ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGuiWindowFlags bindFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
    OverlayRenderer::PushModernStyle();

    if (ImGui::Begin("P2 Binding", nullptr, bindFlags)) {
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::CalcTextSize("PLAYER 2 MAPPING").x - 20.0f);
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 1.0f, 1.0f), "PLAYER 2 MAPPING");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTable("P2MappingTable", 1, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("P2", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            DrawBindList(g_p2OverlayPosition, g_p2Binds, ImVec4(0.2f, 0.9f, 1.0f, 1.0f), true);
            ImGui::EndTable();
        }
    }
    ImGui::End();
    OverlayRenderer::PopModernStyle();
}

// ============================================================================
// メイン描画関数
// ============================================================================

void ControllerMapper::Draw() {
    float screenWidth = ImGui::GetIO().DisplaySize.x;
    float screenHeight = ImGui::GetIO().DisplaySize.y;
    (void)screenHeight; // 将来的に使用予定

    // ---- メインウィンドウ配置 ----
    ImGui::SetNextWindowSize(ImVec2(MAPPING_WINDOW_WIDTH, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2((screenWidth - MAPPING_WINDOW_WIDTH) * 0.5f, -2.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav;

    OverlayRenderer::PushModernStyle();

    if (ImGui::Begin("Controller Mapping", nullptr, flags)) {

        // ---- タイトルヘッダー ----
        ImGui::SetWindowFontScale(1.3f);
        float titleWidth = ImGui::CalcTextSize("CONTROLLER CONFIGURATION").x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - titleWidth) * 0.5f);
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.9f, 1.0f), "CONTROLLER CONFIGURATION");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();

        // ---- 操作ガイド ----
        const char *helperText = "[F4] Close Menu  |  [Enter/Button] Start Mapping";
        float helperWidth = ImGui::CalcTextSize(helperText).x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - helperWidth) * 0.5f);
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", helperText);

        ImGui::Separator();
        ImGui::Spacing();

        // ---- デバイス選択入力処理 ----
        ProcessDeviceSelectionInput();

        // ---- バインド開始共通ヘルパー (#6) ----
        auto startBinding = [](int &pos, double &startTime, std::string *binds) {
            if (pos == 0) {
                pos = 1;
                startTime = ImGui::GetTime();
                for (int i = 0; i < ControllerMapper::NUM_GAME_INPUTS; ++i)
                    binds[i] = "";
            }
        };

        bool kbdStart = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space);
        if (kbdStart) {
            if (g_p1JoyId == -2)
                startBinding(g_p1OverlayPosition, p1BindStartTime, g_p1Binds);
            if (g_p2JoyId == -2)
                startBinding(g_p2OverlayPosition, p2BindStartTime, g_p2Binds);
        }

        // ジョイスティック Edge 取得 + バインド開始 + 切断検知
        // (#8 修正: Edge はここで1度だけ取得し、キャッシュに保存)
        auto devices = cccaster::game_interface::DirectInputHook::GetConnectedDevices();
        g_p1CachedEdge.clear();
        g_p2CachedEdge.clear();
        bool p1DeviceStillExists = (g_p1JoyId == -2);
        bool p2DeviceStillExists = (g_p2JoyId == -2);
        for (const auto &dev : devices) {
            if (dev.id == g_p1JoyId)
                p1DeviceStillExists = true;
            if (dev.id == g_p2JoyId)
                p2DeviceStillExists = true;

            std::string edge = cccaster::game_interface::DirectInputHook::GetAnyInputEdge(dev.id);
            if (!edge.empty()) {
                // P1/P2 の Edge をキャッシュ
                if (dev.id == g_p1JoyId)
                    g_p1CachedEdge = edge;
                if (dev.id == g_p2JoyId)
                    g_p2CachedEdge = edge;

                // バインド開始（HAT/Axis は除外）
                if (edge.find("H") == std::string::npos && edge.find("A") == std::string::npos) {
                    if (g_p1JoyId == dev.id)
                        startBinding(g_p1OverlayPosition, p1BindStartTime, g_p1Binds);
                    if (g_p2JoyId == dev.id)
                        startBinding(g_p2OverlayPosition, p2BindStartTime, g_p2Binds);
                }
            }
        }
        if (!p1DeviceStillExists && g_p1OverlayPosition > 0) {
            g_p1OverlayPosition = 0;
            g_p1JoyId = -1;
        }
        if (!p2DeviceStillExists && g_p2OverlayPosition > 0) {
            g_p2OverlayPosition = 0;
            g_p2JoyId = -1;
        }

        // ---- バインド入力処理 ----
        ProcessBindingInput(g_p1JoyId, 0, g_p1OverlayPosition, g_p1Binds);
        ProcessBindingInput(g_p2JoyId, 1, g_p2OverlayPosition, g_p2Binds);

        // ---- デバイス選択テーブル描画 ----
        DrawDeviceSelectionTable();
    }
    ImGui::End();
    OverlayRenderer::PopModernStyle();

    // ---- バインドウィンドウ描画 ----
    DrawP1BindingWindow(screenWidth);
    DrawP2BindingWindow(screenWidth);
}

// ============================================================================
// バインド状態リセット — F4 で閉じた際にバインド途中の状態を安全にクリアする
// ============================================================================
void ControllerMapper::ResetBindingState() {
    if (g_p1OverlayPosition > 0) {
        g_p1OverlayPosition = 0;
        for (int i = 0; i < NUM_GAME_INPUTS; ++i)
            g_p1Binds[i] = "";
    }
    if (g_p2OverlayPosition > 0) {
        g_p2OverlayPosition = 0;
        for (int i = 0; i < NUM_GAME_INPUTS; ++i)
            g_p2Binds[i] = "";
    }
}
