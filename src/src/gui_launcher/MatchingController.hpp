#pragma once
#include "Session.hpp"
#include "shared_contracts/NativePath.hpp"
namespace cccaster::gui {
struct MatchingController {
    std::unique_ptr<cccaster::matching::Client> client;
    cccaster::matching::Snapshot view;
    uint64_t notifications = 0;
    std::string activeMatch;
    bool hostReady = false, playing = false, otherMode = false;
    int port = 7500;
    char comment[161]{};

    static const char* Status(const std::string& value) {
        if(value=="idle") return Text("Not started", "未開始");
        if(value=="registering") return Text("Registering...", "登録中...");
        if(value=="waiting") return Text("Accepting requests", "申し込み待受中");
        if(value=="confirming") return Text("Confirming this pairing...", "対戦成立を確認中...");
        if(value=="connecting") return Text("Connecting...", "接続中...");
        if(value=="playing"||value=="busy") return Text("In a match / connecting", "対戦中・接続中");
        if(value=="ending") return Text("Finishing the current session...", "現在の接続の終了待ち...");
        if(value=="pending") return Text("Waiting for approval", "相手の承諾待ち");
        if(value=="paused") return Text("Not accepting requests", "受付休止中");
        if(value=="declined") return Text("Request declined", "申し込みを断られました");
        if(value=="closed") return Text("Matching ended", "マッチング終了済み");
        if(value=="listing_expired") return Text("Public listing expired after 6 hours. You can list again with the same code.", "公開掲載から6時間以上経過したため一覧から除外されました。同じコードで公開待機を再開できます");
        if(value=="cancelled") return Text("Request cancelled", "申し込みを取り消しました");
        if(value=="finished") return Text("Session ended; ready for another opponent", "対戦終了。同じコードで次の相手を待てます");
        if(value=="no_response") return Text("No response. You can try another opponent.", "返答がありません。別の相手へ申し込めます");
        if(value=="unavailable") return Text("Could not find this matching registration. Check the code and the opponent's standby state.", "マッチングの登録を確認できません。コードと相手の待機状態を確認してください");
        if(value=="cleanup_unavailable") return Text("Could not prepare automatic listing cancellation. Restart the launcher and try again.", "終了時の募集取消を準備できませんでした。ランチャーを再起動してお試しください");
        if(value=="cleanup_pending") return Text("Could not cancel an earlier listing. Check the connection, then retry below.", "以前の募集を取り消せませんでした。接続を確認し、待機欄から再試行してください");
        if(value=="cleanup_finished") return Text("Earlier listings were cancelled.", "残っていた自分の募集を取り消しました");
        if(value=="invalid_code") return Text("Check the six-character opponent code", "相手の6文字コードを確認してください");
        if(value=="rate_limited") return Text("Service posting limit reached. Wait before retrying.", "通知サービスの投稿制限です。時間を置いて再試行してください");
        if(value=="connection_failed") return Text("Connection failed. Your registration is kept.", "接続できませんでした。登録とコードはそのまま使えます");
        if(value=="invalid_profile") return Text("Check your player name in Profile & settings.", "プロフィール・設定でプレイヤー名を確認してください");
        if(value=="online") return Text("Listing service connected", "一覧に接続済み");
        if(value=="reconnecting") return Text("Reconnecting to service...", "通知サービスへ再接続中...");
        if(value.empty()) return Text("Listed / status not checked", "掲載中・状態未確認");
        return Text("Could not complete the operation. Please retry.", "処理を完了できませんでした。再試行してください");
    }
    void Ensure() {
        if (client) return;
        cccaster::matching::Options options;
        options.server=ConfigManager::GetString("Connection","NtfyServer","https://ntfy.sh");
        if (const char* server=std::getenv("CCCASTER_NTFY_SERVER")) options.server=server;
        options.cleanupDirectory=exePath.parent_path()/L"matching-cleanup";
        client=std::make_unique<cccaster::matching::Client>(options);
        port=std::clamp(ConfigManager::GetInt("Matching","Port",7500),1,65535);
    }
    void Start(bool published) {
        auto name=ConfigManager::GetString("Player","Name","");
        if(name.empty())name="PLAYER";
        client->Command({{"type","start"},{"name",name},{"comment",comment},{"public",published},
            {"spectators",ConfigManager::GetInt("Connection","AllowSpectators",1)!=0}});
    }
    void Invite(const std::string& code) {
        if(!view.registered) Start(false);
        client->Command({{"type","invite"},{"code",code}});
    }
    void Poll(Session& session) {
        if (!client) return;
        if(!activeMatch.empty()) {
            if(!session.Running()) {
                client->Command({{"type","finished"},{"match",activeMatch}});
                activeMatch.clear(); hostReady=false; playing=false;
            } else {
                if(!hostReady && !session.code.empty()) {
                    client->Command({{"type","host_ready"},{"match",activeMatch},{"code",session.code}}); hostReady=true;
                }
                if(!playing && session.p2pStage=="connected") {
                    client->Command({{"type","playing"},{"match",activeMatch}}); playing=true;
                }
            }
        }
        const bool busy=session.Running() && activeMatch.empty();
        if(otherMode!=busy) { otherMode=busy; client->Command({{"type","activity"},{"busy",busy}}); }
        view=client->View();
        if(view.notifications!=notifications) {
            notifications=view.notifications;
            if(!view.incoming.empty()) {
                session.incomingNotice=true;
                if(ConfigManager::GetInt("Notifications","Sound",1))cccaster::notification::PlayIncomingSound();
                if(ConfigManager::GetInt("Notifications","FlashTaskbar",1))cccaster::notification::FlashIncomingWindow(guiWindow,true);
                if(ConfigManager::GetInt("Notifications","DesktopPopup",1))cccaster::notification::ShowIncomingToast(guiWindow,
                    japanese?L"対戦の申し込みが届きました\nマッチング画面で承諾／断るを選べます":L"New match request\nAccept or decline in Matching");
            }
        }
        for(const auto& event:client->Events()) {
            if(event.type=="launch") {
                if(session.Running()) { client->Command({{"type","finished"},{"match",event.match}}); continue; }
                session.Start(event.host,port,event.code.c_str(),false,false,
                    std::clamp(ConfigManager::GetInt("GUI","ConnectionPreference",0),0,2),false,&event);
                if(session.Running()) { activeMatch=event.match; hostReady=false; playing=false; }
                else client->Command({{"type","finished"},{"match",event.match}});
            } else if(event.type=="abort" && activeMatch==event.match && session.Running() && !playing) {
                SetEvent(session.cancel); session.cancelling=true;
            }
        }
    }
};
}
