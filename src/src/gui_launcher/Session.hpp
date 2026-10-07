#pragma once
#include "AppContext.hpp"
#include "SessionMessages.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include "cli_launcher/network_wrapper/ConnectionHash.hpp"
#include "p2p/Matching.hpp"
#include "shared_contracts/BootDiagnostics.hpp"
#include "shared_contracts/ConfigPath.hpp"
#include "shared_contracts/NetplaySettings.hpp"
#include "shared_contracts/IncomingRequest.hpp"
#include "shared_contracts/TrainingStandby.hpp"
#include "launcher/RequestNotification.hpp"
#include <fstream>
#include <algorithm>
#include <cstring>
namespace cccaster::gui {
using main_app::ConfigManager;
inline Message IpResultText(const std::string& state) {
    if(state=="waiting") return {"Waiting for peer (not tested)","相手待ち（未確認）"};
    if(state=="checking_direct") return {"Testing direct connection...","直接接続を確認中..."};
    if(state=="checking_punch") return {"Checking UDP connectivity / hole punching...","UDPの到達確認・ホールパンチング中..."};
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
    if(state=="unavailable") return {"No compatible local candidate","利用できる候補がありません"};
    if(state=="failed") return {"No authenticated response","応答を確認できませんでした"};
    return {"Not tested","未確認"};
}

struct Session {
    training_standby::Channel standby;
    bool standbyTraining = false;
    HANDLE process = nullptr, cancel = nullptr;
    std::filesystem::path logPath;
    std::string log, code, connectionStage;
    std::string p2pStage, p2pService, manualCode, watchCode, watchStage;
    std::filesystem::path peerCodePath;
    std::string ipResults[2];
    Message status = {"Ready when you are.", "開始できます。"};
    bool booting = false, cancelling = false, training = false, spectating = false, replay = false, localVersus = false;
    bool revealCloseLog = false;
    bool failed = false;
    bool hosting = false, incomingNotice = false;
    std::string matchId;
    cccaster::notification::IncomingRequests incomingRequests;
    ULONGLONG lastIncomingSound = 0;
    ~Session() {
        cccaster::notification::CloseIncomingToast();
        cccaster::notification::FlashIncomingWindow(guiWindow, false);
        if (cancel) { SetEvent(cancel); CloseHandle(cancel); }
        if (process) CloseHandle(process); // 実行中のゲームは終了しない。
    }
    bool Running() const { return process != nullptr; }
    bool peerNoticeSeen = false, localNoticeSeen = false;
    session_messages::State diagnostics;
    void Poll() {
        if (!process) return;
        // 終了確認後にログを読む。読み取り直後のworker終了で末尾通知を取りこぼさない。
        const bool ended = WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        DWORD workerExit = 0;
        if (ended) GetExitCodeProcess(process, &workerExit);
        std::ifstream input(logPath, std::ios::binary);
        input.seekg(0, std::ios::end);
        const auto length = input.tellg();
        input.seekg(length > 65536 ? length - std::streamoff(65536) : std::streampos(0));
        std::string raw((std::istreambuf_iterator<char>(input)), {});
        // 末尾64KiBの先頭が途中行なら捨てる。途中から現れた診断文字列を行頭扱いしない。
        if (length > 65536) {
            const auto newline = raw.find('\n');
            raw.erase(0, newline == raw.npos ? raw.size() : newline + 1);
        }
        // ANSI制御列は画面に持ち込まない。ログファイル自体は原文を維持する。
        log.clear();
        for (size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\x1b' && i + 1 < raw.size() && raw[i + 1] == '[') {
                i += 2;
                while (i < raw.size() && !(raw[i] >= '@' && raw[i] <= '~')) ++i;
            } else if (raw[i] == '\r') log += '\n';
            else log += raw[i];
        }
        if (hosting && !cancelling && incomingRequests.Observe(log)) {
            incomingNotice = true;
            const auto now = GetTickCount64();
            if (ConfigManager::GetInt("Notifications", "Sound", 1) &&
                (!lastIncomingSound || now - lastIncomingSound >= 1500)) {
                cccaster::notification::PlayIncomingSound();
                lastIncomingSound = now;
            }
            if (ConfigManager::GetInt("Notifications", "FlashTaskbar", 1))
                cccaster::notification::FlashIncomingWindow(guiWindow, true);
            if (ConfigManager::GetInt("Notifications", "DesktopPopup", 1))
                cccaster::notification::ShowIncomingToast(guiWindow, japanese
                    ? L"対戦の参加要求が届きました\nクリックしてCCCasterを確認"
                    : L"An opponent wants to connect\nClick to open CCCaster");
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
        auto readP2p = [&](const char* prefix, std::string& value) {
            const auto found = session_diagnostics::LastLine(log, prefix);
            if (found.data()) value = found;
        };
        readP2p("[P2P_STATUS] ",p2pStage);
        readP2p("[P2P_SERVICE] ",p2pService);
        readP2p("[P2P_MANUAL] ",manualCode);
        readP2p("[SPECTATOR CODE] ",watchCode);
        readP2p("[WATCH_STATUS] ",watchStage);
        diagnostics.Observe(log);
        booting = booting || diagnostics.gameStarting;
        if (replay) status = session_diagnostics::HasLine(log, "[ REPLAY READY ]")
            ? Message{"Replay viewer is running. Press F4 in the game for controller settings.", "リプレイ観戦を起動しました。ゲーム内のF4でコントローラ設定を開けます。"}
            : Message{"Launching replay viewer...", "リプレイ観戦を起動しています..."};
        else if (localVersus) status = session_diagnostics::HasLine(log, "[ OFFLINE READY ]")
            ? Message{"Offline versus is running. Both players use this game window.", "オフライン対戦を起動しました。同じゲーム画面で1P・2Pを操作してください。"}
            : Message{"Launching offline versus...", "オフライン対戦を起動しています..."};
        else if (training && session_diagnostics::HasLine(log, "[ TRAINING READY ]"))
            status = {"Training is running. Switch to the game window.", "トレーニングを起動しました。ゲーム画面に切り替えてください。"};
        else if (training) status = {"Launching training...", "トレーニングを起動しています..."};
        else if (session_diagnostics::HasLine(log, "[ IN GAME ]")) status = spectating
            ? Message{"Spectator connected. Playback status is shown in the game.", "観戦接続が成立しました。再生状態はゲーム画面に表示します。"}
            : Message{"Match in progress. Switch to the game window.", "対戦中です。ゲーム画面に切り替えてください。"};
        else if (booting) status = {"Connected. Launching the game...", "接続が完了しました。ゲームを起動しています..."};
        else if (!code.empty()) status = {"Waiting for an opponent...", "対戦相手を待っています..."};
        connectionStage = diagnostics.connection;
        if (!ended && !booting && !cancelling) {
            if (connectionStage == "relay_waiting") status = {"Relay service connected. Waiting for someone to join; no short waiting limit.", "接続支援サーバーへ接続しました。相手の参加を待っています。短い待機制限はありません。"};
            else if (connectionStage == "relay") status = {"Trying automatic hole punching through the legacy relay service...", "旧版の接続支援サーバーを使って自動接続を試しています..."};
            else if (connectionStage == "match" || connectionStage == "punch") status = {"Opponent found. Checking the direct UDP connection...", "参加者が見つかりました。双方のUDP接続を確認しています..."};
            else if (connectionStage == "relay_unavailable") status = {"Relay service unavailable. Direct connection is still available; check the relay list or network.", "接続支援サーバーへ到達できません。直接接続は継続します。サーバー設定・回線を確認してください。"};
            else if (connectionStage == "attempt_expired" || connectionStage == "handshake_timeout") status = {"That connection attempt did not complete. Hosting continues for the next opponent.", "その参加者との接続が成立しませんでした。次の参加者の募集を続けます。"};
        }
        if (cancelling && !booting) status = {"Cancelling connection...", "接続をキャンセルしています..."};
        if (!ended && !booting && !cancelling && !p2pStage.empty()) {
            if(p2pStage=="preparing")status={"Preparing connection candidates...","接続候補を準備しています..."};
            else if(p2pStage=="waiting")status={"Waiting for an opponent. The code stays valid while hosting.","募集しています。コードは募集を終了するまで有効です。"};
            else if(p2pStage=="punching")status={"Checking LAN, IPv6 and IPv4 paths...","LAN・IPv6・IPv4の到達を確認しています..."};
            else if(p2pStage=="manual_reply")status={"Send your manual reply code to the host.","手動返信コードを募集側へ送り、貼り付けてもらってください。"};
            else if(p2pStage=="rate_limited")status={"Notification service limit reached. Allow time before retrying; manual exchange is available.","通知サービスの利用上限です。時間を空けて再試行してください。手動交換も利用できます。"};
            else if(p2pStage=="reconnecting")status={"Reconnecting to the notification service...","通知サービスへ再接続しています..."};
            else if(p2pStage=="offline_fallback")status={"Notification service unavailable. Trying LAN discovery; manual exchange is available.","通知サービスが使えません。LAN探索を試します。手動交換も利用できます。"};
            else if(p2pStage=="busy")status={"The host is already connecting or playing.","募集側は接続処理中、または対戦中です。"};
            else if(p2pStage=="closed")status={"Hosting has ended. Ask for a new code.","募集は終了しています。新しいコードを受け取ってください。"};
            else if(p2pStage=="answer_timeout"||p2pStage=="ntfy_unavailable"||p2pStage=="invalid_code"||p2pStage=="code_collision") {
                const auto message=session_messages::P2pFailure(p2pStage);status={message.english,message.translated};
            }
            else if(p2pStage=="punch_timeout")status={"UDP connection failed. Check the firewall, IPv6 availability or port forwarding.","UDP接続が成立しません。ファイアウォール、IPv6の利用可否、ポート転送を確認してください。"};
            else if(p2pStage=="bind_failed") {const auto message=session_messages::P2pFailure(p2pStage);status={message.english,message.translated};}
            else if(p2pStage=="invalid_manual_code")status={"The manual reply does not match this host session.","手動返信コードが今回の募集と一致しません。"};
            if(p2pStage=="waiting"&&!p2pService.empty()&&p2pService!="online")
                status={"Waiting on LAN. For Internet play, exchange manual codes because the notification service is unavailable.","LANで募集しています。通知サービスが使えないため、インターネット対戦には手動コードを交換してください。"};
        }
        if (!ended && spectating && !booting) {
            if(cancelling) status={"Cancelling spectator standby...","観戦待機をキャンセルしています..."};
            else if(watchStage=="standby") status={"Standing by. Spectating starts automatically when the match connects.","観戦待機中です。対戦が接続されると自動で観戦を開始します。"};
            else if(watchStage=="checking") status={"Checking the host's six-character code...","6文字コードの募集状態を確認しています..."};
            else if(watchStage=="connecting") status={"The match is starting. Waiting for the spectator connection...","対戦を開始しています。観戦接続の準備を待っています..."};
            else if(watchStage=="closed") status={"Hosting ended. Ask for the new code.","募集が終了しました。新しいコードを受け取ってください。"};
            else if(watchStage=="disabled") status={"The host does not allow spectators.","募集側が観戦を許可していません。"};
            else if(watchStage=="expired") status={"Host status has expired. Check whether hosting continues.","募集状態の更新が途絶えました。募集が続いているか確認してください。"};
            else if(watchStage=="unreachable") {failed=true;status={"The spectator TCP connection could not be reached. Check the host's firewall or TCP port forwarding.","観戦TCPへ接続できませんでした。募集側のファイアウォール・TCPポート転送を確認してください。"};}
            else if(watchStage=="unavailable"||watchStage=="invalid_code") {failed=true;status={"Could not find current spectator status. Check the code, host version and notification service.","現在の観戦情報を取得できません。コード・募集側の更新版・通知サービスを確認してください。"};}
            else if(watchStage=="reconnecting") status={"Reconnecting to spectator status notifications...","観戦状態の通知へ再接続しています..."};
            else if(watchStage=="rate_limited") status={"Notification service limit reached. Waiting before reconnecting...","通知サービスの利用制限です。時間を空けて再接続します..."};
        }
        if ((diagnostics.peerReason >= 0 && !peerNoticeSeen) ||
            (diagnostics.localReason >= 0 && !localNoticeSeen)) revealCloseLog = true;
        peerNoticeSeen = peerNoticeSeen || diagnostics.peerReason >= 0;
        localNoticeSeen = localNoticeSeen || diagnostics.localReason >= 0;
        if (const auto message = diagnostics.Resolve({ended, cancelling, booting,
                training || replay || localVersus, spectating, workerExit})) {
            status = {message.english, message.translated};
            failed = message.failed;
        }
        if (ended) {
            CloseHandle(process); process = nullptr;
            CloseHandle(cancel); cancel = nullptr;
            cancelling = false;
        }
        if (log.size() > 24000) log.erase(0, log.size() - 24000);
    }
    void Start(bool host, int port, const char* hash, bool offline = false, bool watch = false, int preference = 0, bool replayMode = false,
               const cccaster::matching::Event* matched = nullptr, bool trainingStandby = false, bool localVersusMode = false) {
        if (Running()) return;
        standby.Close(); standbyTraining = false;
        failed = true; // 準備段階の失敗もメイン画面で強調する。
        if (!offline && !cccaster::public_api::NetplaySettings::IsValid(
                ConfigManager::GetInt("Netplay", "DefaultDelay", 2),
                cccaster::public_api::NetplaySettings::DefaultRollback)) {
            status = {"Delay must be 0-8. Rollback is managed internally at R7.", "入力ディレイは0〜8で指定してください。ロールバックは内部でR7に設定します。"};
            return;
        }
        auto dir = exePath.parent_path();
        for (const auto& name : {"../MBAA.exe", "libcccaster_hook.dll", "CCCaster_B.exe"}) {
            std::error_code error;
            const auto state = std::filesystem::status(dir / name, error);
            if (state.type() == std::filesystem::file_type::not_found && std::strcmp(name,"../MBAA.exe")==0) {
                status = {boot::Message(boot::Error::GameMissing,false),boot::Message(boot::Error::GameMissing,true)};
                return;
            }
            if (error || !std::filesystem::is_regular_file(state)) {
                status = {std::string("Cannot access ")+name+". Check its location and file permissions.",
                    std::string(name)+"を読み込めません。配置先とファイルのアクセス権を確認してください。"};
                return;
            }
        }
        auto eventName = L"Local\\CCCasterGuiCancel_" + std::to_wstring(GetCurrentProcessId());
        cancel = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (!cancel) { status = {"Could not prepare the launcher control event.", "ランチャーの制御用イベントを作成できません。"}; return; }
        wchar_t temp[MAX_PATH]{};
        const auto tempLength = GetTempPathW(MAX_PATH, temp);
        if (!tempLength || tempLength >= MAX_PATH) {
            status = {"Cannot access the temporary folder for the session log.","詳細ログの保存先となる一時フォルダーを取得できません。"};
            CloseHandle(cancel); cancel = nullptr; return;
        }
        logPath = std::filesystem::path(temp) / (L"CCCaster_B_GUI_" + std::to_wstring(GetCurrentProcessId()) + L".log");
        peerCodePath=logPath;peerCodePath+=L".peer";
        {std::ofstream clear(peerCodePath,std::ios::trunc);}
        std::ofstream freshLog(logPath, std::ios::binary | std::ios::trunc);
        if (!freshLog) {
            status = {"Could not create the session log.", "詳細ログのファイルを作成できません。"};
            CloseHandle(cancel); cancel = nullptr; return;
        }
        freshLog.close();
        const auto normalized=cccaster::p2p::NormalizeCode(hash);
        if(!normalized.empty())hash=normalized.c_str();
        const auto workerPath = dir / L"CCCaster_B.exe";
        std::wstring args = L"\"" + workerPath.wstring() + L"\" --gui-worker \"" + eventName + L"\" \"" + logPath.wstring() + L"\" ";
        args += localVersusMode ? L"offline 0" : replayMode ? L"replay 0" : offline ? L"training 0" : watch ? L"spectate " + std::wstring(hash, hash + std::strlen(hash))
            : host ? L"host " + std::to_wstring(port) : L"join " + std::wstring(hash, hash + std::strlen(hash));
        if(host&&!offline) args += L" " + std::to_wstring(preference);
        else if(!host&&!watch) args += L" " + std::to_wstring(port);
        if (matched) args += matched->allowSpectators ? L" matched 1" : L" matched 0";
        if (trainingStandby) {
            if (!offline || replayMode || localVersusMode || !standby.Create()) {
                status = {"Could not prepare training standby.", "トレーニング待受の準備に失敗しました。"};
                CloseHandle(cancel); cancel = nullptr; return;
            }
            args += L" \"" + standby.name + L"\"";
        }
        // 3窓目のゲーム初期化もCLIの検証済み経路に揃える。コンソールは表示しない。
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(workerPath.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                            nullptr, dir.c_str(), &startup, &info)) {
            const auto error = std::to_string(GetLastError());
            status = {"Launch failed (Windows error: " + error + ")", "起動に失敗しました（Windowsエラー: " + error + "）"};
            CloseHandle(cancel); cancel = nullptr; return;
        }
        CloseHandle(info.hThread); process = info.hProcess;
        standbyTraining = trainingStandby;
        hosting = host && !offline && !watch && !replayMode && !matched;
        matchId = matched ? matched->match : std::string{};
        incomingRequests.Reset(); incomingNotice = false; lastIncomingSound = 0;
        cccaster::notification::CloseIncomingToast();
        cccaster::notification::FlashIncomingWindow(guiWindow, false);
        failed = false;
        diagnostics = {};
        log.clear(); code.clear(); connectionStage.clear(); p2pStage.clear(); p2pService.clear(); manualCode.clear(); watchCode.clear(); watchStage.clear(); training = offline && !replayMode && !localVersusMode; spectating = watch; replay = replayMode; localVersus = localVersusMode; booting = offline; cancelling = false;
        ipResults[0].clear(); ipResults[1].clear();
        peerNoticeSeen = localNoticeSeen = revealCloseLog = false;
        status = localVersusMode ? Message{"Launching offline versus...", "オフライン対戦を起動しています..."}
                         : replayMode ? Message{"Launching replay viewer...", "リプレイ観戦を起動しています..."}
                         : offline ? Message{"Launching training...", "トレーニングを起動しています..."}
                         : watch ? Message{"Checking spectator standby...", "観戦待機の準備をしています..."}
                         : host ? Message{"Creating your connection code...", "接続コードを作成しています..."}
                      : Message{"Connecting to your opponent...", "対戦相手に接続しています..."};
    }
};


}
