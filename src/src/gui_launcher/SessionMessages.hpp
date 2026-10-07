#pragma once
#include "shared_contracts/BootDiagnostics.hpp"
#include "shared_contracts/SessionDiagnostics.hpp"
#include <utility>

namespace cccaster::gui::session_messages {
namespace diagnostic = cccaster::session_diagnostics;
struct Message {
    std::string english, translated;
    bool failed=false;
    explicit operator bool() const { return !english.empty(); }
};
struct Context {
    bool ended=false, cancelling=false, booting=false, local=false, spectating=false;
    uint32_t workerExit=0;
};
inline Message P2pFailure(std::string_view stage) {
    if(stage=="bind_failed")return {"Could not open the listening port. Check the port and its permissions.","待受ポートを開けません。ポートの使用状況と使用許可を確認してください。",true};
    if(stage=="punch_timeout")return {"UDP connection failed. Check the firewall, IPv6 availability or port forwarding.","UDP接続が成立しません。ファイアウォール、IPv6の利用可否、ポート転送を確認してください。",true};
    if(stage=="busy")return {"The host is already connecting or playing. Try again after their session ends.","募集側は接続処理中、または対戦中です。終了後に再度申し込んでください。",true};
    if(stage=="closed")return {"Hosting has ended. Ask for a new code.","募集は終了しています。新しいコードを受け取ってください。",true};
    if(stage=="answer_timeout")return {"No answer from the host. Check their standby state and code, or exchange manual codes.","募集側から応答がありません。待機状態とコードを確認するか、手動コードを交換してください。",true};
    if(stage=="ntfy_unavailable"||stage=="offline_fallback")return {"The notification service is unavailable. Check your connection or exchange manual codes.","通知サービスを利用できません。回線を確認するか、手動コードを交換してください。",true};
    if(stage=="rate_limited")return {"The notification service limit was reached. Wait before retrying, or exchange manual codes.","通知サービスの利用上限に達しました。時間を空けるか、手動コードを交換してください。",true};
    if(stage=="invalid_code")return {"The connection code is invalid. Copy the host's current code again.","接続コードが不正です。募集側の現在のコードをもう一度コピーしてください。",true};
    if(stage=="code_collision")return {"Could not allocate an unused connection code. Try hosting again.","未使用の接続コードを確保できませんでした。募集を開始し直してください。",true};
    if(stage=="invalid_manual_code")return {"The manual reply does not match this host session.","手動返信コードが今回の募集と一致しません。",true};
    return {};
}
inline Message WatchResult(std::string_view stage) {
    if(stage=="cancelled")return {"Spectator standby cancelled.","観戦待機をキャンセルしました。"};
    if(stage=="closed")return {"Hosting ended. Ask for the new code.","募集が終了しました。新しいコードを受け取ってください。"};
    if(stage=="disabled")return {"The host does not allow spectators.","募集側が観戦を許可していません。"};
    if(stage=="expired")return {"Host status has expired. Check whether hosting continues.","募集状態の更新が途絶えました。募集が続いているか確認してください。",true};
    if(stage=="unreachable")return {"The spectator TCP connection could not be reached. Check the host's firewall or TCP port forwarding.","観戦TCPへ接続できませんでした。募集側のファイアウォール・TCPポート転送を確認してください。",true};
    if(stage=="unavailable")return {"Could not find current spectator status. Check the code, host version and notification service.","現在の観戦情報を取得できません。コード・募集側の更新版・通知サービスを確認してください。",true};
    if(stage=="invalid_code"||stage=="missing_endpoint")return {"A valid spectator code or address is required. Check the host's current code.","有効な観戦コードまたは接続先が必要です。募集側の現在のコードを確認してください。",true};
    return {};
}
inline Message Closed(bool peer, uint32_t reason) {
    if(peer) {
        if(reason==1)return {"Your opponent pressed the game's close button.","相手がゲームの閉じるボタンを押しました。"};
        if(reason==2)return {"Your opponent pressed ESC in the game.","相手がゲームでESCキーを押しました。"};
        if(reason==3)return {"Your opponent pressed F12 in the game.","相手がゲームでF12キーを押しました。"};
        return {"Your opponent's game ended. The cause is unknown.","相手のゲームが終了しました。操作理由は不明です。"};
    }
    if(reason==1)return {"You closed the game window.","ゲームの閉じるボタンで終了しました。"};
    if(reason==2)return {"You ended the game with ESC.","ESCキーでゲームを終了しました。"};
    if(reason==3)return {"You ended the game with F12.","F12キーでゲームを終了しました。"};
    return {"The game was ended by a local operation.","操作によりゲームを終了しました。"};
}
struct State {
    boot::Error bootError=boot::Error::None;
    diagnostic::Result result;
    std::string p2p, watch, connection;
    int peerReason=-1, localReason=-1;
    bool gameReady=false, gameStarting=false, hasLog=false;
    void Observe(std::string_view log) {
        hasLog=hasLog||!log.empty();
        if(auto error=boot::ParseError(log);error!=boot::Error::None)bootError=error;
        if(auto parsed=diagnostic::Parse(log);parsed.code!=diagnostic::Code::None)result=std::move(parsed);
        auto read=[&](std::string_view prefix,std::string& target) {
            auto value=diagnostic::LastLine(log,prefix);if(value.data())target=value;
        };
        read("[P2P_STATUS] ",p2p);read("[WATCH_STATUS] ",watch);read("[CONNECT_STAGE] ",connection);
        for(auto [prefix,target]:{std::pair{"[ PEER CLOSED ] ",&peerReason},std::pair{"[ LOCAL CLOSED ] ",&localReason}}) {
            auto line=diagnostic::LastLine(log,prefix);
            if(line.data())*target=int(diagnostic::Number(diagnostic::Field(line,"reason")));
        }
        gameReady=gameReady||diagnostic::HasLine(log,"[ IN GAME ]")||diagnostic::HasLine(log,"[ TRAINING READY ]")||
            diagnostic::HasLine(log,"[ OFFLINE READY ]")||diagnostic::HasLine(log,"[ REPLAY READY ]");
        gameStarting=gameStarting||gameReady||diagnostic::HasLine(log,"[BOOT_READY] ")||
            diagnostic::HasLine(log,"[ HEADLESS ] Connection established successfully. Booting game")||
            diagnostic::HasLine(log,"[ SPECTATE ] Booting game:");
        // 既存の詳細ログを開いた場合も、定義済み行だけを扱う。
        if(result.code==diagnostic::Code::None) {
            if(diagnostic::HasLine(log,"[ User Aborted ]"))result.code=diagnostic::Code::UserExit;
            else if(diagnostic::HasLine(log,"[ Peer Disconnected ]"))result.code=diagnostic::Code::Disconnected;
            else if(diagnostic::HasLine(log,"[ Sync Timeout ]")||diagnostic::HasLine(log,"[ State Failure ]"))result.code=diagnostic::Code::StateFailure;
            else if(diagnostic::HasLine(log,"[ INIT TIMEOUT ]")||diagnostic::HasLine(log,"[ SYNC TIMEOUT ]"))result.code=diagnostic::Code::InitTimeout;
            else if(diagnostic::HasLine(log,"[ FATAL ERROR ]"))result.code=diagnostic::Code::Exception;
            else if(diagnostic::HasLine(log,"[ HEADLESS ERROR ]"))result.code=diagnostic::Code::ConnectionFailure;
        }
    }
    Message Resolve(Context context) const {
        using diagnostic::Code;
        if(bootError!=boot::Error::None)return {boot::Message(bootError,false),boot::Message(bootError,true),true};
        switch(result.code) {
        case Code::Disconnected:return {"The connection to the other player was lost. Check the session log and your network.","相手との通信が切れました。詳細ログと回線の状態を確認してください。",true};
        case Code::StateFailure:return {"Game state processing or synchronization failed. The stage and reason are in the session log.","ゲーム状態の処理または同期に失敗しました。発生段階と原因は詳細ログに記録しています。",true};
        case Code::InitTimeout:return {"Game initialization or initial synchronization timed out. Check the last stage in the session log.","ゲーム初期化または開始時の同期が時間切れになりました。詳細ログで停止した段階を確認してください。",true};
        case Code::UnexpectedExit:return {"The game exited unexpectedly. Check its exit code and the last stage in the session log.","ゲームが予期せず終了しました。詳細ログの終了コードと停止した段階を確認してください。",true};
        case Code::IpcFailure:return {"Could not read the game's session result. Check the detailed session log.","ゲームの終了結果を取得できませんでした。詳細ログを確認してください。",true};
        case Code::Exception:return {"The launcher encountered an unexpected error. Check the detailed session log.","ランチャーの処理中に予期しないエラーが発生しました。詳細ログを確認してください。",true};
        default:break;
        }
        if((context.cancelling&&!context.booting&&context.ended) || result.code==Code::Cancelled)
            return context.spectating?WatchResult("cancelled"):Message{"Connection cancelled.","接続をキャンセルしました。"};
        if(context.ended&&context.workerExit!=0) {
            const auto code=std::to_string(context.workerExit);
            return {"The launcher exited unexpectedly (exit code: "+code+"). "+(hasLog?"Check the detailed session log.":"No session log was produced."),
                "ランチャーが異常終了しました（終了コード: "+code+"）。"+(hasLog?"詳細ログを確認してください。":"ログ出力前に終了しました。"),true};
        }
        if(result.code==Code::UserExit)return Closed(false,result.reason);
        if(result.code==Code::PeerExit)return Closed(true,result.reason);
        if(result.code==Code::Completed)return {"Session ended. Ready to start again.","終了しました。もう一度開始できます。"};
        if(!context.booting) {
            if(context.spectating) {if(auto message=WatchResult(watch))return message;}
            else if(context.ended||result.code==Code::ConnectionFailure) {if(auto message=P2pFailure(p2p))return message;}
            if((context.ended||result.code==Code::ConnectionFailure)&&(connection=="timeout"||connection=="handshake_timeout"))
                return {"Connection timed out. Check the host's standby state, code and firewall permissions.","接続が時間切れになりました。募集側の待機状態・コード・ファイアウォールの許可を確認してください。",true};
        }
        if(result.code==Code::ConnectionFailure)return {"Connection could not complete. Check the detailed session log.","接続が完了しませんでした。詳細ログを確認してください。",true};
        if(peerReason>=0)return Closed(true,uint32_t(peerReason));
        if(localReason>0)return Closed(false,uint32_t(localReason));
        if(context.ended) {
            if(gameReady||localReason==0)return {"Session ended. Ready to start again.","終了しました。もう一度開始できます。"};
            if(context.spectating)return {"Spectator standby ended before a connection was established. Check the detailed session log.","観戦接続が成立する前に待機処理が終了しました。詳細ログを確認してください。",true};
            return {"The session ended before startup or connection completed. Check the detailed session log.","起動または接続が完了する前に処理が終了しました。詳細ログを確認してください。",true};
        }
        return {};
    }
};
}
