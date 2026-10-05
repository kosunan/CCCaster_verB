#include "LauncherModel.hpp"
#include "EmblemPresets.hpp"
#include "ProductVersion.hpp"
#include "shared_contracts/PlayerName.hpp"
#include "shared_contracts/NativePath.hpp"
#include "p2p/Ntfy.hpp"
#include <commdlg.h>
#include <stdexcept>

namespace cccaster::gui {
using main_app::ConfigManager;
LauncherModel::LauncherModel() {
    const auto language = ConfigManager::GetString("GUI", "Language", "ja");
    japanese = language != "en";
    const auto path = exePath.parent_path() / emblem::FileName;
    std::error_code ignored;
    profileError_ = std::filesystem::exists(path, ignored) && !emblem::Load(path, emblem_);
    UpdateEmblem();
}
void LauncherModel::SaveString(const char* section, const char* key, const std::string& value) {
    const auto old = ConfigManager::GetString(section, key, "");
    ConfigManager::SetString(section, key, value);
    if (!ConfigManager::SaveChecked(cccaster::PathUtf8(cccaster::ConfigPath(exePath.parent_path())))) {
        ConfigManager::SetString(section, key, old);
        throw std::runtime_error(Text("Could not save settings.", "設定を保存できませんでした。"));
    }
}
void LauncherModel::SaveInt(const char* section, const char* key, int value) {
    SaveString(section, key, std::to_string(value));
}
void LauncherModel::UpdateEmblem() {
    emblemData_ = emblem_.id ? p2p::Base64(emblem_.pixels) : "";
}
void LauncherModel::ClearNotice() {
    session_.incomingNotice = false;
    notification::CloseIncomingToast();
    notification::FlashIncomingWindow(guiWindow, false);
}
bool LauncherModel::Occupied() const {
    return codeLookup_.valid() || session_.Running() || (matching_.view.state != "idle" && matching_.view.state != "waiting");
}
void LauncherModel::Poll() {
    session_.Poll();
    matching_.Poll(session_);
    if(codeLookup_.valid() && codeLookup_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {
            const bool registeredCode=codeLookup_.get();
            auto request=std::move(pendingConnection_);pendingConnection_=Json{};
            if(registeredCode) {
                if(!matching_.view.registered) {
                    matching_.Ensure();
                    matching_.port=Integer(request,"port",1,65535);
                    SaveInt("Matching","Port",matching_.port);
                }
                Command({{"type","matching_invite"},{"code",request["code"]}});
            } else Command(request);
        } catch(const std::exception&) {
            pendingConnection_=Json{};
            error_=Text("Could not check the connection code. Please retry.","接続コードの確認に失敗しました。再試行してください。");
        }
    }
}
void LauncherModel::Command(const Json& c) {
    error_.clear();
    try {
        if (!c.is_object() || c.size() > 8) throw std::invalid_argument("invalid message");
        const auto type = String(c, "type", 32);
        if (type == "language") {
            const auto lang = String(c, "value", 2);
            if (lang != "ja" && lang != "en") throw std::invalid_argument("language");
            SaveString("GUI", "Language", lang); japanese = lang == "ja";
        } else if (type == "settings") {
            const auto key = String(c, "key", 32);
            if (key == "Sound" || key == "FlashTaskbar" || key == "DesktopPopup") {
                const bool enabled = Boolean(c, "value"); SaveInt("Notifications", key.c_str(), enabled);
                if (!enabled && key == "FlashTaskbar") notification::FlashIncomingWindow(guiWindow, false);
                if (!enabled && key == "DesktopPopup") notification::CloseIncomingToast();
            } else if (key == "SoftwareRendering") {
                SaveInt("GUI", "SoftwareRendering", Boolean(c, "value"));
            } else if (key == "ConnectionPreference") {
                SaveInt("GUI", "ConnectionPreference", Integer(c, "value", 0, 2));
            } else if (key == "AllowSpectators") {
                if (Occupied() || !matching_.view.incoming.empty() || !matching_.view.outgoing.id.empty())
                    throw std::runtime_error(Text("Change this before requesting a match.", "申し込み前に変更してください。"));
                const bool allowed = Boolean(c, "value"); SaveInt("Connection", "AllowSpectators", allowed);
                if (matching_.client) matching_.client->Command({{"type","spectators"},{"allowed",allowed}});
            } else if (key == "NtfyServer") {
                if (Occupied() || matching_.view.registered) throw std::runtime_error(Text("End the session first.", "先に接続とマッチングを終了してください。"));
                const auto server = String(c, "value", 255);
                if (server.rfind("https://", 0) != 0 || server.find_first_of("\r\n\t ") != std::string::npos)
                    throw std::invalid_argument("https required");
                SaveString("Connection", "NtfyServer", server);
                matching_.client.reset(); matching_.view = {}; matching_.Ensure();
            } else throw std::invalid_argument("unknown setting");
        } else if (type == "profile") {
            if (session_.Running() || matching_.view.registered) throw std::runtime_error(Text("End matching before editing your profile.", "プロフィールの変更前にマッチングを終了してください。"));
            char name[public_api::PlayerNameSize]{};
            const auto value = String(c, "name", 124);
            public_api::NormalizePlayerName(name, value.c_str(), "");
            SaveString("Player", "Name", name);
        } else if (type == "emblem_choose" || type == "emblem_remove" || type == "emblem_preset") {
            if (session_.Running() || matching_.view.registered) throw std::runtime_error(Text("End matching before editing your profile.", "プロフィールの変更前にマッチングを終了してください。"));
            emblem::Image draft;
            if (type == "emblem_choose") {
                wchar_t path[32768]{};
                OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = guiWindow;
                dialog.lpstrFilter = L"24 x 12 / 24bit BMP\0*.bmp\0\0";
                dialog.lpstrFile = path; dialog.nMaxFile = 32768;
                dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
                if (!GetOpenFileNameW(&dialog)) {
                    if (CommDlgExtendedError()) throw std::runtime_error(Text("Could not open the image chooser.", "画像選択を開けませんでした。"));
                    return;
                }
                if (!emblem::Import(path, draft)) throw std::runtime_error(Text("Choose an uncompressed 24-bit BMP (.bmp), exactly 24 x 12 pixels.", "幅24×高さ12px・24bitの非圧縮BMP（.bmp）を選択してください。"));
            } else if (type == "emblem_preset") {
                if (!emblem::ImportPreset(String(c, "code", 2), draft))
                    throw std::runtime_error(Text("Could not load the emblem preset.", "エンブレムのプリセットを読み込めませんでした。"));
            }
            if (!emblem::Save(exePath.parent_path() / emblem::FileName, draft))
                throw std::runtime_error(Text("Could not save the emblem.", "エンブレムを保存できませんでした。"));
            emblem_ = draft; profileError_ = false; UpdateEmblem();
        } else if (type == "copy_code") {
            const auto source = String(c,"source",16);
            std::string value;
            if (source == "matching") value = matching_.view.code;
            else if (source == "session") value = session_.code;
            else if (source == "watch") value = session_.watchCode;
            else if (source == "manual") value = session_.manualCode;
            else throw std::invalid_argument("clipboard source");
            if (value.empty()) return;
            if (!OpenClipboard(guiWindow)) throw std::runtime_error(Text("Clipboard is busy.", "クリップボードを使用できません。"));
            const auto memory = GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));
            auto data = memory ? static_cast<wchar_t*>(GlobalLock(memory)) : nullptr;
            bool success = false;
            if (data) {
                std::copy(value.begin(),value.end(),data); data[value.size()]=0; GlobalUnlock(memory);
                if (EmptyClipboard()) success=SetClipboardData(CF_UNICODETEXT,memory)!=nullptr;
            }
            CloseClipboard(); if(!success && memory)GlobalFree(memory);
            if(!success)throw std::runtime_error(Text("Could not copy the code.", "コードをコピーできませんでした。"));
        } else if (type == "dismiss_notice") ClearNotice();
        else if (type == "test_notification") {
            if (ConfigManager::GetInt("Notifications","Sound",1) && !notification::PlayIncomingSound())
                error_ = Text("Could not play the notification sound.", "通知音を再生できませんでした。");
            if (ConfigManager::GetInt("Notifications","FlashTaskbar",1)) notification::FlashIncomingWindow(guiWindow,true);
            if (ConfigManager::GetInt("Notifications","DesktopPopup",1)) notification::ShowIncomingToast(guiWindow,
                japanese ? L"CCCaster 着信通知のテスト\n実際の申し込みではありません" : L"CCCaster notification test\nThis is not a real request");
        } else if (type == "launch") {
            if (session_.Running() || codeLookup_.valid()) throw std::runtime_error(Text("A connection is already in progress.", "既に接続処理を実行中です。"));
            auto mode = String(c, "mode", 16);
            if(mode=="auto") {
                const auto code=String(c,"code",511);
                const int port=Integer(c,"port",1,65535);
                const auto normalized=p2p::NormalizeCode(code);
                if(code.empty())mode="host";
                else if(normalized.empty())mode="join";
                else {
                    if(Occupied() || !matching_.view.outgoing.id.empty())throw std::invalid_argument("occupied");
                    auto server=ConfigManager::GetString("Connection","NtfyServer","https://ntfy.sh");
                    if(const char* overrideServer=std::getenv("CCCASTER_NTFY_SERVER"))server=overrideServer;
                    pendingConnection_={{"type","launch"},{"mode","join"},{"port",port},{"code",normalized}};
                    // 旧マッチングコードと直接接続コードの照会で描画スレッドを止めない。
                    codeLookup_=std::async(std::launch::async,[normalized,server] {
                        p2p::Keys keys(normalized);p2p::Ntfy ntfy(server);
                        const auto result=ntfy.Poll(keys.status);
                        for(const auto& body:result.messages) {
                            std::string plain;if(!keys.Open(keys.status,body,plain))continue;
                            const auto record=Json::parse(plain,nullptr,false);
                            if(matching::Registration(record) && record["code"]==normalized)return true;
                        }
                        // 登録なし・通知先に到達不能のときも、既存の直接接続／LAN探索を使える。
                        return false;
                    });
                    return;
                }
            }
            if (mode == "training" || mode == "replay") session_.Start(true,0,"",true,false,0,mode=="replay");
            else {
                const bool watch = mode == "spectate", host = mode == "host";
                if (!watch && mode != "host" && mode != "join") throw std::invalid_argument("mode");
                if (!watch && matching_.view.registered) throw std::runtime_error(Text("End matching before using direct connection.", "直接接続の前にマッチングを終了してください。"));
                const int port = watch ? 0 : Integer(c, "port", 1, 65535);
                const auto code = host ? std::string{} : String(c, "code", 511);
                if (!host) {
                    main_app::network_wrapper::ConnectionHash::DecodedAddress address;
                    std::string dc, ds; bool dh = false; std::vector<p2p::Candidate> candidates;
                    const bool valid = !p2p::NormalizeCode(code).empty() || (CodeAlphabet(code) &&
                        (watch ? main_app::network_wrapper::ConnectionHash::DecodeSpectator(code,address) :
                         main_app::network_wrapper::ConnectionHash::Decode(code,address) || (p2p::ReadDirectCode(code,dc,ds,dh,candidates) && dh)));
                    if (!valid) throw std::invalid_argument("code");
                }
                session_.Start(host,port,code.c_str(),false,watch,std::clamp(ConfigManager::GetInt("GUI","ConnectionPreference",0),0,2));
            }
        } else if (type == "cancel_session") {
            if (session_.Running() && !session_.booting) { SetEvent(session_.cancel); session_.cancelling = true; }
        } else if (type == "manual_reply") {
            if (!session_.Running() || session_.booting || session_.manualCode.empty()) throw std::invalid_argument("no manual session");
            const auto code = String(c, "code", 400);
            std::string dc,ds; bool dh=false; std::vector<p2p::Candidate> candidates;
            if (session_.code.empty() ? code != "start" : !p2p::ReadDirectCode(code,dc,ds,dh,candidates) || dh)
                throw std::invalid_argument("manual code");
            std::ofstream out(session_.peerCodePath,std::ios::trunc); out << code; out.flush();
            if (!out) throw std::runtime_error(Text("Could not save the reply.", "返信コードを保存できませんでした。"));
        } else if (type.rfind("matching_",0) == 0) {
            matching_.Ensure();
            if (type == "matching_open") return;
            if (type == "matching_start") {
                if (Occupied() || matching_.view.registered) throw std::invalid_argument("occupied");
                matching_.port = Integer(c,"port",1,65535);
                const auto comment = String(c,"comment",160);
                std::snprintf(matching_.comment,sizeof(matching_.comment),"%s",comment.c_str());
                const bool published = Boolean(c,"public");
                SaveInt("Matching","Public",published); SaveInt("Matching","Port",matching_.port);
                matching_.Start(published);
            } else if (type == "matching_invite") {
                if (Occupied() || matching_.view.paused || !matching_.view.outgoing.id.empty()) throw std::invalid_argument("occupied");
                const auto code = p2p::NormalizeCode(String(c,"code",32));
                if (code.empty()) throw std::invalid_argument("code");
                matching_.Invite(code);
            } else if (type == "matching_visibility") {
                const bool value = Boolean(c,"public"); SaveInt("Matching","Public",value);
                if (matching_.view.registered) matching_.client->Command({{"type","visibility"},{"public",value}});
            } else if (type == "matching_pause") matching_.client->Command({{"type","pause"},{"paused",Boolean(c,"paused")}});
            else if (type == "matching_stop") matching_.client->Command({{"type","stop"}});
            else if (type == "matching_cleanup") {
                if (Occupied() || !matching_.view.outgoing.id.empty()) throw std::invalid_argument("occupied");
                matching_.client->Command({{"type","cleanup"}});
            }
            else if (type == "matching_cancel") matching_.client->Command({{"type","cancel"}});
            else if (type == "matching_cancel_pairing") matching_.client->Command({{"type","cancel_match"}});
            else if (type == "matching_accept" || type == "matching_reject") {
                if (type == "matching_accept" && (Occupied() || matching_.view.paused)) throw std::invalid_argument("occupied");
                const auto id = String(c,"request",32);
                if (std::none_of(matching_.view.incoming.begin(),matching_.view.incoming.end(),[&](const auto& r){ return r.id == id; }))
                    throw std::invalid_argument("request expired");
                matching_.client->Command({{"type",type == "matching_accept" ? "accept" : "reject"},{"request",id}}); ClearNotice();
            } else throw std::invalid_argument("command");
        } else throw std::invalid_argument("command");
    } catch (const std::invalid_argument&) { error_ = Text("Check the input and current session state.", "入力内容と現在の接続状態を確認してください。"); }
      catch (const Json::exception&) { error_ = Text("Invalid operation data.", "操作データが不正です。"); }
      catch (const std::exception& error) { error_ = error.what(); }
}
Json LauncherModel::State(bool includeLog) const {
    auto person = [](const matching::Person& p) { return Json{{"id",p.id},{"name",p.name},{"code",p.code},{"comment",p.comment},{"listedAt",p.listedAt},{"result",MatchingController::Status(p.result)}}; };
    const auto& v = matching_.view;
    Json people = Json::array(), incoming = Json::array();
    for (const auto& p : v.people) {
        auto item=person(p);item["self"]=p.id==v.id;people.push_back(std::move(item));
    }
    for (const auto& r : v.incoming) incoming.push_back({{"id",r.id},{"peer",person(r.peer)},{"expires",r.expires}});
    Json settings;
    for (const char* key : {"Sound","FlashTaskbar","DesktopPopup"}) settings[key] = ConfigManager::GetInt("Notifications",key,1) != 0;
    settings["ConnectionPreference"] = std::clamp(ConfigManager::GetInt("GUI","ConnectionPreference",0),0,2);
    settings["AllowSpectators"] = ConfigManager::GetInt("Connection","AllowSpectators",1) != 0;
    settings["SoftwareRendering"] = ConfigManager::GetInt("GUI","SoftwareRendering",0) != 0;
    settings["NtfyServer"] = ConfigManager::GetString("Connection","NtfyServer","https://ntfy.sh");
    settings["public"] = ConfigManager::GetInt("Matching","Public",0) != 0;
    settings["port"] = std::clamp(ConfigManager::GetInt("Matching","Port",7500),1,65535);
    settings["delay"] = ConfigManager::GetInt("Netplay","DefaultDelay",2);
    settings["rollback"] = cccaster::public_api::NetplaySettings::DefaultRollback;
    Json routes = Json::array();
    for (const auto& route : session_.ipResults) routes.push_back(route.empty() ? "" : IpResultText(route).c_str());
    return {{"protocol",1},{"version",CCCASTER_VERSION},{"language",japanese?"ja":"en"},{"settings",settings},
        {"profile",{{"name",ConfigManager::GetString("Player","Name","")},{"emblemId",emblem_.id},{"pixels",emblemData_},{"error",profileError_}}},
        {"error",error_},{"connectionLookup",codeLookup_.valid()},{"session",{{"running",session_.Running()},{"game",GameRunning()},{"failed",session_.failed},{"cancelling",session_.cancelling},
          {"spectating",session_.spectating},{"status",session_.status.c_str()},{"code",session_.code},{"watchCode",session_.watchCode},
          {"manualCode",session_.manualCode},{"incomingNotice",session_.incomingNotice},{"routes",routes},{"log",includeLog?session_.log:""}}},
        {"matching",{{"registered",v.registered},{"public",v.publicVisible},{"paused",v.paused},{"code",v.code},{"state",v.state},{"port",matching_.port},
          {"status",MatchingController::Status((v.paused||matching_.otherMode)&&v.state=="waiting"?"paused":v.state)},
          {"notice",v.notice.empty()?"":MatchingController::Status(v.notice)},{"error",v.error.empty()?"":MatchingController::Status(v.error)},
          {"service",MatchingController::Status(v.service)},{"incomplete",v.incomplete},{"busy",Occupied()},{"otherMode",matching_.otherMode},
          {"cleanupPending",v.cleanupPending},{"people",people},{"incoming",incoming},{"outgoing",{{"id",v.outgoing.id},{"peer",person(v.outgoing.peer)}}}}}};
}
}
