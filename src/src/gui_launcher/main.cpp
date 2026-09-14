#include "shared_contracts/ConfigPath.hpp"
#include "ProductVersion.hpp"
#include "shared_contracts/PlayerName.hpp"
#include "imgui.h"
#include "HeaderImage.hpp"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "cli_launcher/ConfigManager.hpp"
#include "cli_launcher/GuiSession.hpp"
#include "cli_launcher/controller/MainController.hpp"
#include "cli_launcher/network_wrapper/ConnectionHash.hpp"
#include "shared_contracts/NetplaySettings.hpp"
#include <windows.h>
#include <shellapi.h>
#include <d3d9.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstdio>
#include <string>

namespace {
using namespace cccaster::main_app;
LPDIRECT3D9 d3d = nullptr;
LPDIRECT3DDEVICE9 device = nullptr;
D3DPRESENT_PARAMETERS present{};
UINT resizeWidth = 0, resizeHeight = 0;
ImFont* titleFont = nullptr;
ImFont* headingFont = nullptr;
ImFont* logoFont = nullptr;
HeaderImage headerImage;
// 参考画像の紅色・夜の紫・青白い光を、文字と操作の役割に分ける。
const ImVec4 accent(0.79f, 0.66f, 1.0f, 1);
const ImVec4 paper(0.96f, 0.94f, 1.0f, 1);
const ImVec4 crimson(0.61f, 0.10f, 0.25f, 1);
const ImVec4 muted(0.78f, 0.75f, 0.84f, 1);
const ImVec4 warning(1.0f, 0.76f, 0.42f, 1);
std::filesystem::path exePath;
bool japanese = false;
const char* Text(const char* english, const char* translated) {
    return japanese ? translated : english;
}
struct Message {
    std::string english, translated;
    const char* c_str() const { return japanese ? translated.c_str() : english.c_str(); }
};

Message IpResultText(const std::string& state) {
    if(state=="waiting") return {"Waiting for peer (not tested)","相手待ち（未確認）"};
    if(state=="checking_direct") return {"Testing direct connection...","直接接続を確認中..."};
    if(state=="checking_punch") return {"Direct failed; trying hole punching...","直接接続不成立・ホールパンチング中..."};
    if(state=="exchanging") return {"Direct failed; exchanging punch addresses...","直接接続不成立・パンチ用アドレス交換中..."};
    if(state=="ok_direct") return {"Direct connection available","直接接続できます"};
    if(state=="ok_punch") return {"Connected by hole punching","ホールパンチングで接続できます"};
    if(state=="selected_direct") return {"Selected: direct connection","採用：直接接続"};
    if(state=="selected_punch") return {"Selected: hole punching","採用：ホールパンチング"};
    if(state=="legacy_connected") return {"Connected (peer without route comparison)","接続成功（相手は品質比較非対応）"};
    if(state=="direct_failed") return {"No direct response","直接接続の応答なし"};
    if(state=="peer_direct_failed") return {"Peer reports no direct response","相手側の直接確認：応答なし"};
    if(state=="peer_punch_failed") return {"Peer reports no punch response","相手側のパンチ確認：応答なし"};
    if(state=="punch_failed") return {"No response after hole punching","ホールパンチング後も応答なし"};
    if(state=="punch_unavailable") return {"Punch address exchange unavailable (not tested)","パンチ用アドレス交換不可（未確認）"};
    if(state=="no_candidate") return {"No connection candidate (not tested)","接続候補なし（未確認）"};
    if(state=="bind_failed") return {"Could not open local socket","待受ソケットを作成できません"};
    if(state=="cancelled") return {"Cancelled","確認をキャンセルしました"};
    return {"Not tested","未確認"};
}

struct Session {
    HANDLE process = nullptr, cancel = nullptr;
    std::filesystem::path logPath;
    std::string log, code, connectionStage;
    std::string ipResults[2];
    Message status = {"Ready when you are.", "開始できます。"};
    bool booting = false, cancelling = false, training = false, spectating = false, replay = false;
    bool revealCloseLog = false;
    bool failed = false;
    ~Session() {
        if (cancel) { SetEvent(cancel); CloseHandle(cancel); }
        if (process) CloseHandle(process); // 実行中のゲームは終了しない。
    }
    bool Running() const { return process != nullptr; }
    bool ShowCloseStatus() {
        if (log.find("[ PEER CLOSED ]") != std::string::npos) {
            if (!peerNoticeSeen) { revealCloseLog = true; peerNoticeSeen = true; }
            if (log.find("[ PEER CLOSED ] reason=1") != std::string::npos)
                status = {"Your opponent pressed the game's close button.", "相手がゲームの閉じるボタンを押しました。"};
            else if (log.find("[ PEER CLOSED ] reason=2") != std::string::npos)
                status = {"Your opponent pressed ESC in the game.", "相手がゲームでESCキーを押しました。"};
            else if (log.find("[ PEER CLOSED ] reason=3") != std::string::npos)
                status = {"Your opponent pressed F12 in the game.", "相手がゲームでF12キーを押しました。"};
            else status = {"Your opponent's game exited. The cause is unknown.", "相手のゲームが終了しました。操作理由は不明です。"};
            return true;
        }
        if (log.find("[ LOCAL CLOSED ]") != std::string::npos) {
            if (!localNoticeSeen) { revealCloseLog = true; localNoticeSeen = true; }
            status = {"Your game ended. See the session log for the exit reason and delivery result.",
                      "ゲームを終了しました。終了理由と通知の受領結果は詳細ログに表示しています。"};
            return true;
        }
        return false;
    }
    bool peerNoticeSeen = false, localNoticeSeen = false;
    void Poll() {
        if (!process) return;
        // 終了確認後にログを読む。読み取り直後のworker終了で末尾通知を取りこぼさない。
        const bool ended = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        std::ifstream input(logPath, std::ios::binary);
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        input.seekg(length > 65536 ? length - std::streamoff(65536) : std::streampos(0));
        std::string raw((std::istreambuf_iterator<char>(input)), {});
        // ANSI制御列は画面に持ち込まない。ログファイル自体は原文を維持する。
        log.clear();
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\x1b' && i + 1 < raw.size() && raw[i + 1] == '[') {
                i += 2;
                while (i < raw.size() && !(raw[i] >= '@' && raw[i] <= '~')) ++i;
            } else if (raw[i] == '\r') log += '\n';
            else log += raw[i];
        }
        auto start = log.find("[HEADLESS HOST] Hash: ");
        if (start != std::string::npos) {
            start += std::strlen("[HEADLESS HOST] Hash: ");
            auto end = log.find('\n', start);
            if (end != std::string::npos) code = log.substr(start, end - start);
        }
        for(int family=0;family<2;++family) {
            const std::string prefix=family?"[IP_RESULT] IPv6 ":"[IP_RESULT] IPv4 ";
            const auto pos=log.rfind(prefix);
            if(pos!=std::string::npos) {
                const auto end=log.find('\n',pos+prefix.size());
                if(end!=std::string::npos) ipResults[family]=log.substr(pos+prefix.size(),end-pos-prefix.size());
            }
        }
        booting = booting || log.find("Booting game") != std::string::npos;
        if (replay) status = log.find("[ REPLAY READY ]") != std::string::npos
            ? Message{"Replay viewer is running. Press F4 in the game for controller settings.", "リプレイ観戦を起動しました。ゲーム内のF4でコントローラ設定を開けます。"}
            : Message{"Launching replay viewer...", "リプレイ観戦を起動しています..."};
        else if (training && log.find("[ TRAINING READY ]") != std::string::npos)
            status = {"Training is running. Switch to the game window.", "トレーニングを起動しました。ゲーム画面に切り替えてください。"};
        else if (training) status = {"Launching training...", "トレーニングを起動しています..."};
        else if (log.find("[ IN GAME ]") != std::string::npos) status = spectating
            ? Message{"Spectator connected. Playback status is shown in the game.", "観戦接続が成立しました。再生状態はゲーム画面に表示します。"}
            : Message{"Match in progress. Switch to the game window.", "対戦中です。ゲーム画面に切り替えてください。"};
        else if (booting) status = {"Connected. Launching the game...", "接続が完了しました。ゲームを起動しています..."};
        else if (!code.empty()) status = {"Waiting for an opponent...", "対戦相手を待っています..."};
        auto stageStart = log.rfind("[CONNECT_STAGE] ");
        if (stageStart != std::string::npos) {
            stageStart += std::strlen("[CONNECT_STAGE] ");
            auto stageEnd = log.find('\n', stageStart);
            if (stageEnd != std::string::npos) connectionStage = log.substr(stageStart, stageEnd-stageStart);
        }
        if (!booting && !cancelling) {
            if (connectionStage == "relay_waiting") status = {"Relay service connected. Waiting for someone to join; no short waiting limit.", "接続支援サーバーへ接続しました。相手の参加を待っています。短い待機制限はありません。"};
            else if (connectionStage == "relay") status = {"Trying automatic hole punching through the legacy relay service...", "旧版の接続支援サーバーを使って自動接続を試しています..."};
            else if (connectionStage == "match" || connectionStage == "punch") status = {"Opponent found. Checking the direct UDP connection...", "参加者が見つかりました。双方のUDP接続を確認しています..."};
            else if (connectionStage == "relay_unavailable") status = {"Relay service unavailable. Direct connection is still available; check the relay list or network.", "接続支援サーバーへ到達できません。直接接続は継続します。サーバー設定・回線を確認してください。"};
            else if (connectionStage == "attempt_expired" || connectionStage == "handshake_timeout") status = {"That connection attempt did not complete. Hosting continues for the next opponent.", "その参加者との接続が成立しませんでした。次の参加者の募集を続けます。"};
        }
        if (cancelling && !booting) status = {"Cancelling connection...", "接続をキャンセルしています..."};
        if (ended) {
            DWORD result = 0;
            GetExitCodeProcess(process, &result);
            if (cancelling && !booting) status = {"Connection cancelled.", "接続をキャンセルしました。"};
            else if (ShowCloseStatus()) {}
            else if (result != 0 || log.find("ERROR") != std::string::npos ||
                     log.find("TIMEOUT") != std::string::npos || log.find("failed") != std::string::npos)
                { failed = true; status = {"Check the opponent's code and whether they are hosting. Automatic hole punching also failed or launch could not complete. Check the session log for launch errors.", "相手のコードと募集状態を確認してください。接続の自動試行または起動に失敗しました。起動エラーは詳細ログに表示します。"}; }
            else status = {"Session ended. Ready to start again.", "終了しました。もう一度開始できます。"};
            if (!cancelling && (connectionStage == "timeout" || connectionStage == "handshake_timeout")) {
                failed = true;
                status = {"Connection attempt ended. Verify the host is still waiting, the code is current, and CCCaster is allowed through the firewall. Repeating unchanged conditions may not help.", "接続試行を終了しました。相手の募集状態・コードの期限・ファイアウォールの許可を確認してください。同じ条件の繰り返しで改善するとは限りません。"};
            }
            CloseHandle(process); process = nullptr;
            CloseHandle(cancel); cancel = nullptr;
            cancelling = false;
        }
        ShowCloseStatus();
        if (log.size() > 24000) log.erase(0, log.size() - 24000);
    }
    void Start(bool host, int port, const char* hash, bool offline = false, bool watch = false, int preference = 0, bool replayMode = false) {
        if (Running()) return;
        failed = true; // 準備段階の失敗もメイン画面で強調する。
        if (!offline && !cccaster::public_api::NetplaySettings::IsValid(
                ConfigManager::GetInt("Netplay", "DefaultDelay", 2),
                ConfigManager::GetInt("Netplay", "MaxRollback", 4))) {
            status = {"Invalid settings: delay and rollback must be non-negative, with a total of 8 or less.", "設定を確認してください。入力遅延とロールバック上限は0以上、合計8以下です。"};
            return;
        }
        auto dir = exePath.parent_path();
        if (!std::filesystem::exists(dir / ".." / "MBAA.exe") ||
            !std::filesystem::exists(dir / "libcccaster_hook.dll")) {
            status = {"Game files not found. Place this launcher and libcccaster_hook.dll in the game's cccaster_B folder.", "ゲームファイルが見つかりません。ゲーム内の cccaster_B フォルダーにランチャーと libcccaster_hook.dll を配置してください。"};
            return;
        }
        auto eventName = L"Local\\CCCasterGuiCancel_" + std::to_wstring(GetCurrentProcessId());
        cancel = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (!cancel) { status = {"Could not prepare the connection.", "接続の準備に失敗しました。"}; return; }
        wchar_t temp[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temp)) { CloseHandle(cancel); cancel = nullptr; return; }
        logPath = std::filesystem::path(temp) / (L"CCCaster_B_GUI_" + std::to_wstring(GetCurrentProcessId()) + L".log");
        std::ofstream freshLog(logPath, std::ios::binary | std::ios::trunc);
        if (!freshLog) {
            status = {"Could not create the session log.", "詳細ログのファイルを作成できません。"};
            CloseHandle(cancel); cancel = nullptr; return;
        }
        freshLog.close();
        std::wstring args = L"\"" + exePath.wstring() + L"\" --worker \"" + eventName + L"\" \"" + logPath.wstring() + L"\" ";
        args += replayMode ? L"replay 0" : offline ? L"training 0" : watch ? L"spectate " + std::wstring(hash, hash + std::strlen(hash))
            : host ? L"host " + std::to_wstring(port) : L"join " + std::wstring(hash, hash + std::strlen(hash));
        if(host&&!offline) args += L" " + std::to_wstring(preference);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(exePath.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, dir.c_str(), &startup, &info)) {
            const auto error = std::to_string(GetLastError());
            status = {"Launch failed (Windows error: " + error + ")", "起動に失敗しました（Windowsエラー: " + error + "）"};
            CloseHandle(cancel); cancel = nullptr; return;
        }
        CloseHandle(info.hThread); process = info.hProcess;
        failed = false;
        log.clear(); code.clear(); connectionStage.clear(); training = offline && !replayMode; spectating = watch; replay = replayMode; booting = offline || watch; cancelling = false;
        ipResults[0].clear(); ipResults[1].clear();
        peerNoticeSeen = localNoticeSeen = revealCloseLog = false;
        status = replayMode ? Message{"Launching replay viewer...", "リプレイ観戦を起動しています..."}
                         : offline ? Message{"Launching training...", "トレーニングを起動しています..."}
                         : host ? Message{"Creating your connection code...", "接続コードを作成しています..."}
                      : Message{"Connecting to your opponent...", "対戦相手に接続しています..."};
    }
};

void Heading(const char* text) {
    ImGui::PushFont(headingFont); ImGui::TextUnformatted(text); ImGui::PopFont();
}

bool ModeButton(const char* id, const char* number, const char* label, const char* detail,
                bool selected, float& reveal) {
    const auto pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
    const bool pressed = ImGui::Button(id, ImVec2(width, 64));
    ImGui::PopStyleColor(3);
    const bool hovered = ImGui::IsItemHovered();
    reveal += ((selected ? 1.f : 0.f) - reveal) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);
    auto* draw = ImGui::GetWindowDrawList();
    const float inset = 7.f * (1.f - reveal);
    const ImVec2 shape[] = {{pos.x + inset, pos.y}, {pos.x + width, pos.y},
        {pos.x + width - 14, pos.y + 64}, {pos.x + inset, pos.y + 64}};
    draw->AddConvexPolyFilled(shape, 4, selected ? IM_COL32(136,24,65,255)
        : hovered ? IM_COL32(51,28,73,255) : IM_COL32(26,19,40,255));
    if (selected) draw->AddRectFilled(ImVec2(pos.x, pos.y), ImVec2(pos.x + 5, pos.y + 64), IM_COL32(199,166,255,255));
    if (ImGui::IsItemFocused()) draw->AddRect(pos, ImVec2(pos.x + width, pos.y + 64), IM_COL32(220,206,255,255), 0, 0, 2);
    const auto color = ImGui::GetColorU32(paper);
    draw->AddText(ImVec2(pos.x + 12, pos.y + 11), color, number);
    draw->AddText(headingFont, 23.f, ImVec2(pos.x + 42 + 4 * reveal, pos.y + 8), color, label);
    draw->AddText(ImVec2(pos.x + 12, pos.y + 39), selected ? color : ImGui::GetColorU32(muted), detail);
    return pressed;
}

bool PrimaryButton(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Button, crimson);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.73f, .14f, .34f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.46f, .10f, .31f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, paper);
    ImGui::PushFont(headingFont);
    const bool pressed = ImGui::Button(label, ImVec2(-1, 46));
    ImGui::PopFont(); ImGui::PopStyleColor(4);
    return pressed;
}

// 高さを文字の折返しから決め、長い失敗理由も切り落とさない。
void Notice(const char* id, const char* title, const char* detail, ImVec4 color) {
    const auto pos = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float textWidth = std::max(1.f, width - 30);
    ImGui::PushFont(headingFont);
    const float titleHeight = ImGui::CalcTextSize(title, nullptr, false, textWidth).y;
    ImGui::PopFont();
    const float detailHeight = *detail ? ImGui::CalcTextSize(detail, nullptr, false, textWidth).y + 6 : 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 10));
    ImGui::BeginChild(id, ImVec2(0, titleHeight + detailHeight + 22), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushFont(headingFont); ImGui::TextWrapped("%s", title); ImGui::PopFont();
    ImGui::PopStyleColor();
    if (*detail) ImGui::TextWrapped("%s", detail);
    ImGui::EndChild(); ImGui::PopStyleVar();
    ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + 3, pos.y + titleHeight + detailHeight + 22), ImGui::GetColorU32(color));
}

bool QuietDetails(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(.09f,.06f,.13f,1));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(.18f,.12f,.24f,1));
    const bool open = ImGui::CollapsingHeader(label);
    ImGui::PopStyleColor(2); return open;
}

void Draw(Session& session) {
    static char playerName[cccaster::public_api::PlayerNameSize]{};
    static bool nameLoaded = false, nameSaveFailed = false;
    if (!nameLoaded) {
        const auto configured = ConfigManager::GetString("Player", "Name", "");
        cccaster::public_api::NormalizePlayerName(playerName, configured.c_str(), "");
        nameLoaded = true;
    }
    static int page = 0, mode = 0, port = 7500;
    static float reveal[5] = {1, 0, 0, 0, 0};
    static char hash[512]{};
    const auto size = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(size);
    ImGui::Begin("Launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    auto* backdrop = ImGui::GetWindowDrawList();
    backdrop->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(size.x, 88),
        IM_COL32(115,15,45,255), IM_COL32(62,14,47,255),
        IM_COL32(18,10,35,255), IM_COL32(49,15,62,255));
    if (headerImage.texture) {
        backdrop->AddRectFilled(ImVec2(0, 0), ImVec2(size.x, 88), IM_COL32(0,0,0,255));
        // ヘッダーを88pxへ圧縮し、右端の画像をクリップする。
        const float imageWidth = std::min(480.f, size.x * .42f);
        const float imageHeight = imageWidth * static_cast<float>(headerImage.height) / headerImage.width;
        backdrop->PushClipRect(ImVec2(0, 0), ImVec2(size.x, 88), true);
        backdrop->AddImage(reinterpret_cast<ImTextureID>(headerImage.texture),
            ImVec2(size.x - imageWidth, 0), ImVec2(size.x, imageHeight));
        backdrop->PopClipRect();
    }
    backdrop->AddRectFilledMultiColor(ImVec2(0,88), ImVec2(size.x,94),
        IM_COL32(204,38,81,255), IM_COL32(107,55,228,255),
        IM_COL32(107,55,228,255), IM_COL32(204,38,81,255));
    backdrop->AddText(logoFont, 48, ImVec2(29,17), IM_COL32(77,45,122,255), "CCCaster");
    backdrop->AddText(logoFont, 48, ImVec2(26,14), ImGui::GetColorU32(paper), "CCCaster");
    backdrop->AddText(ImVec2(28,62), IM_COL32(224,209,243,255), "MELTY BLOOD Actress Again Current Code");
    backdrop->AddText(ImVec2(size.x * .35f,28), IM_COL32(202,180,218,255), "V10  /  MBAACC Ver.1.07 Rev.1.4.0");
    ImGui::SetCursorPos(ImVec2(16, 106));
    ImGui::BeginChild("navigation", ImVec2(202, -28), false);
    if (ModeButton("##play", "01", Text("VERSUS", "対戦"), Text("Host / Join with code", "募集・コードで参加"), page == 0, reveal[0])) page = 0;
    if (ModeButton("##spectate", "02", Text("SPECTATE", "観戦"), Text("Watch with connection code", "接続コードで観戦"), page == 3, reveal[1])) page = 3;
    if (ModeButton("##training", "03", Text("TRAINING", "トレーニング"), Text("Offline practice", "オフラインで練習"), page == 2, reveal[2])) page = 2;
    if (ModeButton("##replay", "04", Text("REPLAY", "リプレイ観戦"), Text("Watch saved matches", "保存した試合を再生"), page == 4, reveal[4])) page = 4;
    if (ModeButton("##guide", "05", Text("HOW TO PLAY", "操作ガイド"), Text("Set up & controls", "準備と操作方法"), page == 1, reveal[3])) page = 1;
    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::Button(Text("日本語###language", "English###language"), ImVec2(-1, 32))) japanese = !japanese;
    ImGui::TextColored(muted, Text("Display language", "表示言語"));
    ImGui::EndChild(); ImGui::SameLine();
    ImGui::BeginChild("content", ImVec2(0, -28), true);
    Heading(page == 4 ? Text("REPLAY", "リプレイ観戦") : page == 3 ? Text("SPECTATE", "観戦") : page == 2 ? Text("TRAINING", "トレーニング")
        : page == 1 ? Text("HOW TO PLAY", "操作ガイド") : Text("PLAY ONLINE", "ネット対戦"));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));
    if (page == 0) {
        ImGui::BeginDisabled(session.Running());
        ImGui::TextUnformatted(Text("Player name", "プレイヤー名"));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##playerName", playerName, sizeof(playerName), ImGuiInputTextFlags_CallbackCharFilter,
            [](ImGuiInputTextCallbackData* data) { return data->EventChar < 0x20 || data->EventChar > 0x7e ? 1 : 0; });
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char normalized[cccaster::public_api::PlayerNameSize]{};
            cccaster::public_api::NormalizePlayerName(normalized, playerName, "");
            const auto old = ConfigManager::GetString("Player", "Name", "");
            ConfigManager::SetString("Player", "Name", normalized);
            nameSaveFailed = !ConfigManager::SaveChecked(cccaster::ConfigPath(exePath.parent_path()).string());
            if (nameSaveFailed) ConfigManager::SetString("Player", "Name", old);
            else std::snprintf(playerName, sizeof(playerName), "%s", normalized);
        }
        ImGui::TextWrapped(Text("Up to 31 ASCII characters. Saved when you leave this field; applies to the next match. Empty uses the game's name.", "半角英数字・記号31文字まで。欄を離れると保存し、次の対戦から反映します。空欄はゲーム側の名前を使います。"));
        if (nameSaveFailed) ImGui::TextWrapped(Text("Name could not be saved. Edit the field to retry.", "名前を保存できませんでした。入力欄を編集して再試行してください。"));
        ImGui::EndDisabled();
        ImGui::Separator();
    }
    if (page == 3) {
        Heading(Text("A place for every spectator", "大会も、仲間の対戦も"));
        ImGui::TextWrapped(Text("The match players' names are shown while spectating.", "観戦中は対戦している2人の名前を表示します。"));
        ImGui::TextWrapped(Text("Paste the host's connection code as-is. No prefix is needed. Join a running match and catch up automatically.", "募集側の接続コードをそのまま貼り付けてください。接頭辞の追加は不要です。試合途中から自動で追いつきます。"));
        static char spectatorCode[256]{};
        ImGui::BeginDisabled(session.Running());
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##spectatorCode", Text("Connection code / case-sensitive", "接続コード / 大文字・小文字を区別"), spectatorCode, sizeof(spectatorCode));
        network_wrapper::ConnectionHash::DecodedAddress address;
        const bool valid = network_wrapper::ConnectionHash::DecodeSpectator(spectatorCode, address) &&
            std::all_of(spectatorCode, spectatorCode + std::strlen(spectatorCode), [](unsigned char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
        ImGui::BeginDisabled(!valid);
        if (PrimaryButton(Text("START SPECTATING###watch", "観戦を開始###watch"))) session.Start(false, 0, spectatorCode, false, true);
        ImGui::EndDisabled(); ImGui::EndDisabled();
        ImGui::TextWrapped("%s", session.status.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(Text("Direct connection uses the host's TCP port. If the current match history is unavailable, wait for the next match. Up to 8 viewers.", "直接接続には募集側のTCPポートへの到達が必要です。現在の試合履歴が残っていない場合は次の試合を待ちます。最大8人。"));
        if (QuietDetails(Text("Session log###watchLog", "詳細ログ###watchLog"))) ImGui::TextUnformatted(session.log.c_str());
    } else if (page == 1) {
        ImGui::TextWrapped(Text("Start here, then keep the controls close at hand.", "初めての起動から、対戦後のリプレイまで。"));
        ImGui::Spacing();
        if (ImGui::BeginTabBar("guideTopics")) {
            const auto section = [](const char* title, const char* body) {
                ImGui::Spacing(); Heading(title);
                ImGui::TextWrapped("%s", body);
                ImGui::Dummy(ImVec2(0, 8));
            };
            if (ImGui::BeginTabItem(Text("GET STARTED###guideStart", "はじめに###guideStart"))) {
                section(Text("01   Place the launcher", "01   ファイルを配置"),
                    Text("Place the cccaster_B folder beside MBAA.exe. Keep CCCaster_B_GUI.exe, libcccaster_hook.dll and cccaster.ini together inside it.",
                         "MBAA.exe と同じ場所に cccaster_B フォルダーを配置します。中に CCCaster_B_GUI.exe、libcccaster_hook.dll、cccaster.ini を揃えてください。"));
                section(Text("02   Try TRAINING first", "02   まずはトレーニングへ"),
                    Text("Choose TRAINING on the left and start the game. You can check your controls without an opponent or a connection code.",
                         "左の「トレーニング」から起動。対戦相手や接続コードなしで、操作を確認できます。"));
                ImGui::PushStyleColor(ImGuiCol_Text, accent);
                Heading(Text("F4   Set up your controller", "F4   コントローラを設定"));
                ImGui::PopStyleColor();
                ImGui::TextWrapped(Text("At character select, press F4. Choose your device for PLAYER 1, check the bindings, then press F4 to save and close. Configure the local player's device under PLAYER 1 even when joining a match.",
                    "キャラクター選択で F4 を押し、PLAYER 1 の機器とボタン割当を確認します。もう一度 F4 で保存して閉じます。対戦に参加する側も、自分の機器は PLAYER 1 に設定してください。"));
                ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
                ImGui::TextWrapped(Text("Ready to play? Open MODES for hosting, joining and watching matches.", "準備ができたら「各モード」で、対戦や観戦の始め方を確認できます。"));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Text("MODES###guideModes", "各モード###guideModes"))) {
                section(Text("VERSUS   Play online", "対戦   募集・コードで参加"),
                    Text("Set your player name. Host a match and share the generated code, or join with your opponent's code. Codes are case-sensitive. The game launches after connection. Leave connection priority on Automatic to start.",
                         "プレイヤー名を入力。募集する側は発行されたコードを相手へ共有し、参加する側はそのコードを貼り付けます。大文字・小文字を区別します。接続後にゲームが起動。接続方式はまず「自動」で利用できます。"));
                section(Text("SPECTATE   Watch a live match", "観戦   進行中の試合を見る"),
                    Text("Paste the host's connection code as-is and start spectating. No prefix is needed. Playback catches up automatically; if the current match is unavailable, wait for the next one.",
                         "募集側の接続コードをそのまま貼り付けて開始。接頭辞は不要です。途中参加は自動で追いつきます。現在の試合を取得できない場合は、次の試合を待ちます。"));
                section(Text("TRAINING   Practice offline", "トレーニング   オフラインで練習"),
                    Text("Start without a connection code. Controller settings are available with F4 at character select and during practice. See CONTROLS for FN1 / FN2 and the frame bar.",
                         "接続コードなしで起動。キャラ選択中と練習中に F4 で設定できます。FN1・FN2やフレームバーは「操作」を確認してください。"));
                section(Text("REPLAY   Watch a saved match", "リプレイ観戦   保存した試合を見る"),
                    Text("Open the standard replay list. It reads ReplayVS beside MBAA.exe. Press F4 to set up your controller, then use directions and A to choose a replay; B goes back.",
                         "MBAA.exe と同じ場所の ReplayVS を一覧表示します。F4 で機器を設定し、方向入力と A で再生。B で戻ります。"));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Text("CONTROLS###guideControls", "操作###guideControls"))) {
                ImGui::Spacing();
                ImGui::TextWrapped(Text("A / B / FN1 / FN2 refer to your configured buttons, not fixed keyboard keys.", "A・B・FN1・FN2は、設定したボタンの名前です。固定のキーボードキーではありません。"));
                ImGui::Spacing();
                if (ImGui::BeginTable("guideKeys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, 132.f);
                    ImGui::TableSetupColumn("action");
                    const auto row = [](const char* key, const char* detail) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn();
                        ImGui::TextUnformatted(key); ImGui::TableNextColumn(); ImGui::TextWrapped("%s", detail);
                    };
                    row("F4", Text("Controller settings: character select, training battle or replay list. Press again to save and close.", "コントローラ設定。キャラ選択・練習中・リプレイ一覧で使用。再度押すと保存して閉じます。"));
                    row("A / B", Text("Confirm / back. At character select, B is blocked before choosing a character.", "決定／戻る。キャラ未選択時の B は、選択画面から抜けないよう無効になります。"));
                    row("Ctrl + 0-8", Text("Input delay D, in frames. Available during online character select.", "入力ディレイ D（フレーム）。ネット対戦のキャラ選択中に変更。"));
                    row("Alt + 0-8", Text("Rollback limit R, in frames. D + R must be 8 or less. Default: D2 / R4.", "ロールバック上限 R（フレーム）。D＋Rは8以下。既定値は D2／R4。"));
                    row("F1", Text("Toggle the frame bar in training, live spectating and replay battles.", "トレーニング・ネット観戦・リプレイ再生中のフレームバー表示を切り替えます。"));
                    row("FN1", Text("Training: save the current state once. Hold to freeze both characters; release to resume.", "練習中：現在の状態を1回保存。押している間は両キャラを停止し、離すと再開。"));
                    row("FN2", Text("Training: reset, then restore the saved state. With no saved state, perform the normal reset.", "練習中：通常リセット後に保存状態を復元。保存がなければ通常リセット。"));
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                ImGui::TextWrapped(Text("Training has one temporary save slot. Leaving battle for character select clears it.", "練習用の保存は一時的な1枠です。キャラクター選択などで戦闘を離れると消去されます。"));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(Text("AFTER A MATCH###guideAfter", "対戦後###guideAfter"))) {
                section(Text("Play again", "同じ相手と続ける"),
                    Text("Both players choose ONCE AGAIN for a rematch. If either chooses CHARACTER SELECT, both return to character select.",
                         "双方が ONCE AGAIN を選ぶと再戦。片側が CHARACTER SELECT を選ぶと、双方がキャラクター選択に戻ります。"));
                section(Text("Your replay is saved automatically", "リプレイは自動保存"),
                    Text("When the online match result is confirmed, each player saves one standard .rep file in ReplayVS. You do not need to select SAVE REPLAY.",
                         "ネット対戦の結果確定時に、各プレイヤーの ReplayVS へ標準の .rep ファイルを1回保存します。SAVE REPLAY の選択は不要です。"));
                section(Text("Find the match you want", "日時・名前・勝者で探す"),
                    Text("Filenames contain the date, time, PLAYER 1 name and PLAYER 2 name. [WIN] follows the winner's name. [UNDECIDED] means the winner could not be determined.",
                         "ファイル名には日時、1P名、2P名が入ります。勝者の名前の後ろに [WIN]、勝者を確定できなかった場合は末尾に [UNDECIDED] が付きます。"));
                ImGui::Separator(); ImGui::Spacing();
                ImGui::TextWrapped(Text("Close the game, then choose REPLAY in this launcher to watch a saved match, or select another mode.",
                    "ゲームを閉じ、GUIの「リプレイ観戦」で保存した試合を再生できます。別のモードを始める場合も、先にゲームを終了してください。"));
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    } else if (page == 4) {
        Heading(Text("Watch saved matches", "保存した試合を観戦する"));
        ImGui::TextWrapped(Text("Open the game's replay list and select a match to watch. No connection code is needed.", "ゲームのリプレイ一覧を開き、観戦する試合を選びます。接続コードは不要です。"));
        ImGui::Spacing();
        ImGui::BeginDisabled(session.Running());
        if (PrimaryButton(Text("OPEN REPLAY VIEWER###startReplay", "リプレイ観戦を開始###startReplay")))
            session.Start(true, 0, "", true, false, 0, true);
        ImGui::EndDisabled();
        ImGui::TextWrapped("%s", session.status.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(Text("Press F4 in the replay list to configure your controller. Use your configured directional controls and A to select a replay; B goes back.", "リプレイ一覧で F4 を押すとコントローラ設定ができます。設定した方向入力と A でリプレイを選び、B で戻ります。"));
        ImGui::TextWrapped(Text("Replays are in the ReplayVS folder beside MBAA.exe. Auto-saved filenames contain the date, time and both player names; [WIN] marks the winner.", "MBAA.exe と同じ場所の ReplayVS フォルダーを読み込みます。自動保存ファイルは日時・両プレイヤー名で識別でき、[WIN] が勝者です。"));
        ImGui::TextWrapped(Text("Close the game to start another mode.", "ゲームを終了すると、別のモードを開始できます。"));
        if (QuietDetails(Text("Session log###replayLog", "詳細ログ###replayLog"))) ImGui::TextUnformatted(session.log.c_str());
    } else if (page == 2) {
        Heading(Text("Practice at your own pace", "自分のペースで練習する"));
        ImGui::TextWrapped(Text("Launch offline training. No opponent or connection code is needed.", "対戦相手や接続コードなしで、オフラインのトレーニングを開始します。"));
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::BeginChild("trainingStatus", ImVec2(0, 60), true);
        ImGui::TextColored(accent, session.Running() ? Text("RUNNING", "実行中") : Text("STANDBY", "待機中"));
        ImGui::TextWrapped("%s", session.status.c_str());
        ImGui::EndChild();
        ImGui::BeginDisabled(session.Running());
        if (PrimaryButton(Text("START TRAINING###startTraining", "トレーニングを開始###startTraining")))
            session.Start(true, 0, "", true);
        ImGui::EndDisabled();
        ImGui::Spacing();
        ImGui::TextWrapped(Text("Choose your characters in the game. Press F4 to configure your controller.", "ゲーム画面でキャラクターを選んでください。コントローラ設定は F4 で開けます。"));
        ImGui::TextWrapped(Text("Close the game to return and choose another mode.", "ゲームを終了すると、別のモードを開始できます。"));
        if (QuietDetails(Text("Session log###trainingLog", "詳細ログ###trainingLog"))) {
            ImGui::BeginChild("trainingLog", ImVec2(0, 150), true);
            ImGui::TextUnformatted(session.log.c_str()); ImGui::EndChild();
        }
    } else {
        ImGui::BeginDisabled(session.Running());
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2;
        ImGui::PushFont(headingFont);
        for (int choice = 0; choice < 2; ++choice) {
            if (choice) ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, mode == choice ? ImVec4(.34f,.18f,.47f,1) : ImVec4(.13f,.09f,.19f,1));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, mode == choice ? 2.f : 1.f);
            ImGui::PushStyleColor(ImGuiCol_Border, mode == choice ? accent : ImVec4(.36f,.29f,.43f,1));
            if (ImGui::Button(choice == 0 ? Text("HOST A MATCH###host", "対戦を募集###host") : Text("JOIN WITH CODE###join", "コードで参加###join"), ImVec2(half, 46))) mode = choice;
            ImGui::PopStyleColor(2); ImGui::PopStyleVar();
        }
        ImGui::PopFont();
        ImGui::Spacing();
        ImGui::EndDisabled();
        if(!session.ipResults[0].empty()||!session.ipResults[1].empty()) {
            // 接続コードや説明で結果が画面下へ押し出されない位置に固定する。
            for(int family=0;family<2;++family) {
                const auto result=IpResultText(session.ipResults[family]);
                ImGui::TextWrapped("%s: %s",family?"IPv6":"IPv4",result.c_str());
            }
            ImGui::Spacing();
        }
        if (!session.Running()) {
            bool valid = true;
            if (mode == 0) {
                ImGui::TextWrapped(Text("Create a code and share it with your opponent.", "募集を開始して、コードを相手に共有します。"));
                ImGui::SetNextItemWidth(170); ImGui::InputInt(Text("Port###port", "ポート番号###port"), &port, 0);
                int preference=std::clamp(ConfigManager::GetInt("GUI","ConnectionPreference",0),0,2);
                static bool preferenceSaveFailed=false;
                ImGui::SetNextItemWidth(230);
                if(ImGui::Combo(Text("Connection priority###priority","接続方式の優先###priority"), &preference,
                    japanese ? "自動（接続品質で選択）\0IPv4優先\0IPv6優先\0" : "Automatic (connection quality)\0Prefer IPv4\0Prefer IPv6\0")) {
                    const int old=ConfigManager::GetInt("GUI","ConnectionPreference",0);
                    ConfigManager::SetInt("GUI","ConnectionPreference",preference);
                    preferenceSaveFailed=!ConfigManager::SaveChecked(cccaster::ConfigPath(exePath.parent_path()).string());
                    if(preferenceSaveFailed) ConfigManager::SetInt("GUI","ConnectionPreference",old);
                }
                if(preferenceSaveFailed) ImGui::TextColored(warning,Text("Could not save priority.","優先設定を保存できませんでした。"));
                ImGui::TextWrapped(Text("If the preferred protocol cannot connect, the other is used.","優先した方式で接続できない場合は、もう一方を使用します。"));
                valid = port > 0 && port <= 65535;
                if (!valid) ImGui::TextColored(ImVec4(1,.48f,.64f,1), Text("Enter a port between 1 and 65535.", "1〜65535 のポート番号を入力してください。"));
            } else {
                ImGui::TextWrapped(Text("Paste the code exactly as shared (case-sensitive), then join.", "大文字・小文字を変えずに相手のコードを貼り付けて、参加します。"));
                ImGui::SetNextItemWidth(-1);
                // 貼り付け先を見失わないよう、入力欄だけを細い紅色の線で常時囲む。
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
                ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(.90f, .18f, .30f, 1.f));
                ImGui::InputTextWithHint("##hash", Text("Connection code / Ctrl+V to paste", "接続コードを入力 / Ctrl+V で貼り付け"), hash, sizeof(hash));
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
                network_wrapper::ConnectionHash::DecodedAddress address;
                const bool asciiCode = std::all_of(hash, hash + std::strlen(hash), [](unsigned char c) {
                    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-';
                });
                valid = asciiCode && network_wrapper::ConnectionHash::Decode(hash, address);
                if (*hash && !valid) ImGui::TextColored(ImVec4(1,.48f,.64f,1),
                    address.isExpired ? Text("This code has expired. Ask for a new one.", "期限切れです。新しいコードを受け取ってください。") : Text("Check the connection code and try again.", "接続コードを確認してください。"));
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(!valid);
            if (PrimaryButton(mode == 0 ? Text("START HOSTING  >###start", "募集を開始する  >###start") : Text("JOIN MATCH  >###start", "対戦に参加する  >###start")))
                { session.Start(mode == 0, port, hash, false, false,
                    std::clamp(ConfigManager::GetInt("GUI","ConnectionPreference",0),0,2)); }
            ImGui::EndDisabled();
        }
        if (!session.code.empty() && session.Running() && !session.booting) {
            ImGui::TextWrapped(Text("Post this code exactly as shown (case-sensitive). It expires in 6 hours.", "大文字・小文字を変えずにコードを共有してください。有効期限は作成から6時間です。"));
            ImGui::Spacing(); ImGui::TextUnformatted(Text("Share this connection code with your opponent", "この接続コードを相手に共有してください"));
            ImGui::SetNextItemWidth(-130);
            ImGui::InputText("##share", session.code.data(), session.code.size()+1, ImGuiInputTextFlags_ReadOnly);
            ImGui::SameLine();
            if (ImGui::Button(Text("COPY###copy", "コピー###copy"), ImVec2(110, 0))) ImGui::SetClipboardText(session.code.c_str());
        }
        if (!session.code.empty() && session.Running()) {
            ImGui::Spacing(); ImGui::TextWrapped(Text("Spectators use this same connection code. Select SPECTATE and paste it as-is. Available once the match connects.", "観戦者も同じ接続コードを使います。「観戦」を選び、そのまま貼り付けてください。対戦接続後に利用できます。"));
            const auto watchCode = session.code;
            if (ImGui::Button(Text("COPY CONNECTION CODE###copyWatch", "接続コードをコピー###copyWatch"))) ImGui::SetClipboardText(watchCode.c_str());
        }
        if (session.Running() && !session.booting) {
            ImGui::BeginDisabled(session.cancelling);
            if (ImGui::Button(Text("Cancel connection###cancel", "接続をキャンセル###cancel"))) { SetEvent(session.cancel); session.cancelling = true; }
            ImGui::EndDisabled();
        }
        ImGui::Spacing();
        const char* statusTitle = session.failed ? Text("Connection / launch failed", "接続・起動に失敗しました")
            : session.cancelling && !session.booting ? Text("Cancelling...", "キャンセル中...")
            : session.ShowCloseStatus() ? Text("Session ended", "セッション終了")
            : session.Running() && session.booting ? Text("Connected / game running", "接続完了・ゲーム実行中")
            : session.Running() && (session.connectionStage == "match" || session.connectionStage == "punch") ? Text("Connecting to your opponent", "参加者との接続を確認中")
            : session.Running() && !session.code.empty() ? Text("Waiting for an opponent", "相手の参加を待っています")
            : session.Running() ? Text("Preparing the connection...", "接続を準備しています...")
            : Text("Ready to start", "開始できます");
        const char* statusDetail = session.log.empty() && !session.Running() && !session.failed
            ? (mode == 0 ? Text("Start hosting to create your invitation code.", "「募集を開始する」で招待コードを作成します。")
                         : Text("Use your opponent's code to join their match.", "相手のコードで募集済みの対戦に参加します。"))
            : session.status.c_str();
        Notice("status", statusTitle, statusDetail, session.failed ? warning : accent);
        ImGui::TextWrapped(Text("Direct connection first; automatic hole punching when needed.", "直接接続を優先し、必要なら自動でホールパンチングします。"));

        ImGui::PushFont(headingFont);
        ImGui::TextWrapped(Text("F4  |  Controller settings in game", "F4  |  ゲーム内でコントローラ設定"));
        ImGui::PopFont();
        const int delay = ConfigManager::GetInt("Netplay", "DefaultDelay", 2);
        const int rollback = ConfigManager::GetInt("Netplay", "MaxRollback", 4);
        ImGui::TextColored(muted, Text("Input delay  %d F    /    Rollback limit  %d F", "入力遅延  %d F    /    ロールバック上限  %d F"), delay, rollback);
        if (QuietDetails(Text("Session log###details", "詳細ログ###details"))) {
            ImGui::BeginChild("log", ImVec2(0, 150), true);
            ImGui::TextUnformatted(session.log.c_str());
            if (session.revealCloseLog) { ImGui::SetScrollHereY(1.0f); session.revealCloseLog = false; }
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();
    ImGui::SetCursorPos(ImVec2(18, size.y - 26));
    ImGui::TextColored(muted, Text("TAB / ARROWS: SELECT    ENTER / SPACE: CONFIRM", "TAB / 矢印: 選択    ENTER / SPACE: 決定"));
    ImGui::End();
}
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
LRESULT WINAPI WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(window, message, w, l)) return true;
    if (message == WM_SIZE && w != SIZE_MINIMIZED) { resizeWidth = LOWORD(l); resizeHeight = HIWORD(l); return 0; }
    if (message == WM_GETMINMAXINFO) {
        auto info = reinterpret_cast<MINMAXINFO*>(l); info->ptMinTrackSize = {870, 640}; return 0;
    }
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (message == WM_SYSCOMMAND && (w & 0xfff0) == SC_KEYMENU) return 0;
    return DefWindowProcW(window, message, w, l);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, 32768); exePath = path;
    const auto config = cccaster::ConfigPath(exePath.parent_path());
    if (std::filesystem::exists(config)) ConfigManager::Load(config.string());
    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && (argc == 6 || argc == 7) && wcscmp(argv[1], L"--worker") == 0) {
        gui::cancelEvent = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        if (!gui::cancelEvent) { LocalFree(argv); return 1; }
        if (!_wfreopen(argv[3], L"wb", stdout)) { CloseHandle(gui::cancelEvent); LocalFree(argv); return 1; }
        std::cout << std::unitbuf;
        const bool replay = wcscmp(argv[4], L"replay") == 0;
        const bool training = wcscmp(argv[4], L"training") == 0;
        const bool spectator = wcscmp(argv[4], L"spectate") == 0;
        const bool host = training || replay || wcscmp(argv[4], L"host") == 0;
        const std::wstring value = argv[5];
        if(argc==7 && host && !training && !replay) {
            if(wcscmp(argv[6],L"1")==0) gui::hostPreference=1;
            else if(wcscmp(argv[6],L"2")==0) gui::hostPreference=2;
        }
        LocalFree(argv);
        int result = 0;
        try {
            controller::MainController app(true, false, host, "", host ? static_cast<uint16_t>(std::stoi(value)) : 0,
                                           host ? "" : std::string(value.begin(), value.end()), true,
                                           replay ? cccaster::public_api::IpcGameMode::Replay
                                           : spectator ? cccaster::public_api::IpcGameMode::Spectator
                                           : training ? cccaster::public_api::IpcGameMode::Training
                                                    : cccaster::public_api::IpcGameMode::Versus);
            app.Run();
        } catch (const std::exception& error) { std::cout << "ERROR: " << error.what(); result = 1; }
        CloseHandle(gui::cancelEvent); return result;
    }
    if (argv) LocalFree(argv);
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.style = CS_CLASSDC; wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.lpszClassName = L"CCCasterV10Gui";
    RegisterClassExW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, CCCASTER_PRODUCT_TITLE_W, WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 870, 640, nullptr, nullptr, instance, nullptr);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    present.Windowed = TRUE; present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.BackBufferFormat = D3DFMT_UNKNOWN; present.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (!window || !d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
        D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present, &device))) {
        MessageBoxW(nullptr, L"Could not initialize Direct3D 9.", L"CCCaster", MB_ICONERROR);
        if (d3d) d3d->Release();
        if (window) DestroyWindow(window);
        return 1;
    }
    headerImage.Load(device);
    ImGui::CreateContext();
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    wchar_t windowsDir[MAX_PATH]{}; GetWindowsDirectoryW(windowsDir, MAX_PATH);
    auto font = std::filesystem::path(windowsDir) / "Fonts" / "meiryo.ttc";
    if (!std::filesystem::exists(font)) font = std::filesystem::path(windowsDir) / "Fonts" / "msgothic.ttc";
    if (!std::filesystem::exists(font)) {
        MessageBoxW(window, L"A Japanese font is required for the help guide (Meiryo or MS Gothic).", L"CCCaster", MB_ICONERROR);
        ImGui::DestroyContext(); headerImage.Release(); device->Release(); d3d->Release(); DestroyWindow(window); return 1;
    }
    const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    io.Fonts->AddFontFromFileTTF(font.string().c_str(), 19 * scale, nullptr, io.Fonts->GetGlyphRangesJapanese());
    auto bold = std::filesystem::path(windowsDir) / "Fonts" / "meiryob.ttc";
    if (!std::filesystem::exists(bold)) bold = font;
    headingFont = io.Fonts->AddFontFromFileTTF(bold.string().c_str(), 23 * scale, nullptr, io.Fonts->GetGlyphRangesJapanese());
    titleFont = io.Fonts->AddFontFromFileTTF(bold.string().c_str(), 34 * scale, nullptr, io.Fonts->GetGlyphRangesJapanese());
    auto logo = std::filesystem::path(windowsDir) / "Fonts" / "impact.ttf";
    if (!std::filesystem::exists(logo)) logo = bold;
    logoFont = io.Fonts->AddFontFromFileTTF(logo.string().c_str(), 48 * scale);
    io.FontGlobalScale = 1 / scale;
    auto& style = ImGui::GetStyle(); ImGui::StyleColorsDark();
    style.WindowPadding = ImVec2(14, 10); style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 6); style.ChildRounding = 0; style.FrameRounding = 0;
    style.Colors[ImGuiCol_Text] = paper;
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(.51f,.47f,.59f,1);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(.028f,.018f,.055f,1);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(.055f,.035f,.086f,1);
    style.Colors[ImGuiCol_Border] = ImVec4(.24f,.16f,.34f,1);
    style.Colors[ImGuiCol_Separator] = ImVec4(.49f,.23f,.76f,1);
    style.Colors[ImGuiCol_Header] = ImVec4(.29f,.13f,.43f,1);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(.39f,.19f,.55f,1);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(.47f,.21f,.59f,1);
    style.Colors[ImGuiCol_Button] = ImVec4(.18f,.10f,.28f,1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.30f,.16f,.44f,1);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(.39f,.19f,.50f,1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(.11f,.07f,.17f,1);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.22f,.13f,.33f,1);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(.28f,.16f,.41f,1);
    style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(.47f,.26f,.70f,.70f);
    style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(.04f,.025f,.065f,1);
    style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(.28f,.20f,.38f,1);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(.39f,.27f,.54f,1);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(.49f,.32f,.67f,1);
    style.Colors[ImGuiCol_NavHighlight] = accent;
    ImGui_ImplWin32_Init(window); ImGui_ImplDX9_Init(device);
    ShowWindow(window, SW_SHOWDEFAULT); UpdateWindow(window);
    Session session; bool done = false;
    while (!done) {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessage(&message); if (message.message == WM_QUIT) done = true;
        }
        if (done) break;
        session.Poll();
        if (IsIconic(window)) { Sleep(100); continue; }
        if (resizeWidth && resizeHeight) {
            present.BackBufferWidth = resizeWidth; present.BackBufferHeight = resizeHeight;
            resizeWidth = resizeHeight = 0;
            ImGui_ImplDX9_InvalidateDeviceObjects(); device->Reset(&present);
        }
        auto ready = device->TestCooperativeLevel();
        if (ready == D3DERR_DEVICELOST) { Sleep(100); continue; }
        if (ready == D3DERR_DEVICENOTRESET) { ImGui_ImplDX9_InvalidateDeviceObjects(); device->Reset(&present); continue; }
        ImGui_ImplDX9_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        Draw(session); ImGui::Render();
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(7,5,14), 1, 0);
        if (SUCCEEDED(device->BeginScene())) { ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData()); device->EndScene(); }
        device->Present(nullptr, nullptr, nullptr, nullptr);
        Sleep(session.booting ? 100 : 16);
    }
    ImGui_ImplDX9_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    headerImage.Release(); device->Release(); d3d->Release(); DestroyWindow(window); UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
