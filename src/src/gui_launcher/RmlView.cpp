#include "RmlView.hpp"
#include "EmblemCatalog.hpp"
#include "MatchingPresentation.hpp"
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/StringUtilities.h>
#include <chrono>
#include <charconv>

namespace cccaster::gui {
namespace {
std::string Escape(const std::string& value) {
    std::string result;
    for(char c:value) switch(c) {
    case '&': result+="&amp;";break; case '<':result+="&lt;";break;
    case '>':result+="&gt;";break;case '"':result+="&quot;";break;
    case '\'':result+="&#39;";break;
    default:result+=c;
    }
    return result;
}
bool Disabled(Rml::Element* e) {
    for(;e;e=e->GetParentNode()) if(e->HasAttribute("disabled")) return true;
    return false;
}
void Walk(Rml::Element* e,const std::function<void(Rml::Element*)>& callback) {
    callback(e);
    for(int i=0;i<e->GetNumChildren();++i) Walk(e->GetChild(i),callback);
}
int Number(const std::string& value) {
    int result=0; const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
    return parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size()?result:0;
}
}
RmlView::RmlView(Rml::Context& context,std::function<void(Json)> send):context_(context),send_(std::move(send)) {
    document_=context_.LoadDocument("launcher.rml");
    if(!document_) throw std::runtime_error("RML document could not be loaded");
    std::string presets;
    for (const auto& preset : emblem::Presets) {
        const std::string code = preset.code;
        presets += "<button id=\"emblem-preset-" + code + "\" class=\"emblem-preset\" data-command=\"emblem-preset\" data-value=\"" + code +
            "\"><img src=\"preset:" + code + "\" width=\"48\" height=\"24\" /><span data-en=\"" + preset.english + "\">" + preset.japanese + "</span></button>";
    }
    Find("emblem-presets")->SetInnerRML(presets);
    const char* jaLabels[]{"上","下","左","右","A / 決定","B / 戻る","C","D","E","Start","FN1 / 練習状態の保存","FN2 / 練習状態の読込み","A+B"};
    const char* enLabels[]{"Up","Down","Left","Right","A / Confirm","B / Cancel","C","D","E","Start","FN1 / Save practice state","FN2 / Load practice state","A+B"};
    std::string bindings;
    for(int i=0;i<13;++i) {
        const auto row=std::to_string(i);
        bindings+="<div class='binding-row'><span class='binding-label' data-en='"+std::string(enLabels[i])+"'>"+jaLabels[i]+"</span><button id='controller-bind-"+row+"' class='binding-value' data-command='controller-capture' data-value='"+row+"'>--</button><button id='controller-clear-"+row+"' class='binding-clear quiet' data-command='controller-clear' data-value='"+row+"' data-en='Clear'>解除</button></div>";
    }
    Find("controller-bindings")->SetInnerRML(bindings);
    Walk(document_,[&](auto* e){
        if(e->HasAttribute("data-en")) translations_.emplace_back(e,e->GetInnerRML(),e->template GetAttribute<std::string>("data-en",""));
    });
    for(const char* event:{"click","change","blur","keydown"}) document_->AddEventListener(event,this,true);
    document_->Show(); MatchingTab("public");
}
RmlView::~RmlView() {
    for(const char* event:{"click","change","blur","keydown"}) document_->RemoveEventListener(event,this,true);
    document_->Close();
}
Rml::Element* RmlView::Find(const std::string& id) const { return document_->GetElementById(id); }
std::string RmlView::Value(const char* id) const {
    auto* e=dynamic_cast<Rml::ElementFormControl*>(Find(id)); return e?e->GetValue():"";
}
void RmlView::SetValue(const char* id,const std::string& value,bool force) {
    auto* e=dynamic_cast<Rml::ElementFormControl*>(Find(id));
    if(e && (force || context_.GetFocusElement()!=e) && e->GetValue()!=value) e->SetValue(value);
}
void RmlView::Text(const char* id,const std::string& value) {
    auto* e=Find(id); if(!e)return;
    auto* text=e->GetNumChildren()==1?dynamic_cast<Rml::ElementText*>(e->GetChild(0)):nullptr;
    if(text) { if(text->GetText()!=value)text->SetText(value); }
    else { e->SetInnerRML("");auto child=document_->CreateTextNode(value);e->AppendChild(std::move(child)); }
}
void RmlView::Show(const char* id,bool visible) { if(auto* e=Find(id))e->SetClass("hidden",!visible); }
void RmlView::Disable(const char* id,bool disabled) {
    if(auto* e=Find(id)) Walk(e,[&](auto* child){
        if(child==e || child->GetTagName()=="input" || child->GetTagName()=="select" || child->GetTagName()=="button") {
            if(disabled)child->SetAttribute("disabled","");else child->RemoveAttribute("disabled");
            child->SetPseudoClass("disabled",disabled);
            if(child->GetTagName()=="button")child->SetProperty("tab-index",disabled?"none":"auto");
        }
    });
}
void RmlView::Translate(const std::string& language) {
    language_=language;
    for(auto& [element,ja,en]:translations_) element->SetInnerRML(language_=="ja"?ja:Escape(en));
    Text("language",language_=="ja"?"English":"日本語"); peopleSignature_.clear();requestSignature_.clear();
}
void RmlView::Navigate(const std::string& page) {
    if(page!="matching"&&page!="spectate"&&page!="training"&&page!="replay"&&page!="settings"&&page!="guide"&&page!="controller")return;
    if(page_=="controller"&&page!="controller")send_({{"type","controller"},{"action","close"}});
    page_=page;
    for(const char* name:{"matching","spectate","training","replay","settings","guide","controller"}) {
        Show((std::string("page-")+name).c_str(),page_==name);
        if(auto* e=Find(std::string("nav-")+name))e->SetClass("active",page_==name);
    }
    Find("content")->SetScrollTop(0);
    if(page_=="matching")send_({{"type","matching_open"}});
    if(page_=="controller")send_({{"type","controller"},{"action","open"}});
}
void RmlView::MatchingTab(const std::string& tab) {
    if(tab!="public"&&tab!="direct")return;
    matchingTab_=tab;
    Show("direct-panel",tab=="direct");Show("matching-panel",tab!="direct");
    Show("public-directory",tab=="public");
    for(auto [id,name]:std::vector<std::pair<const char*,const char*>>{{"matching-tab","public"},{"direct-tab","direct"}}) {
        Find(id)->SetClass("active",tab==name);
        Find(id)->SetAttribute("aria-selected",tab==name?"true":"false");
    }
    // タブは閲覧だけを切り替える。公開範囲は開始／切替ボタンで明示的に変更する。
    Registration();
}
std::string TrimCode(const std::string& value) {
    const auto first=value.find_first_not_of(" \t\r\n");
    return first==std::string::npos?std::string{}:value.substr(first,value.find_last_not_of(" \t\r\n")-first+1);
}
void RmlView::Registration() {
    if(state_.empty())return;
    const auto& m=state_["matching"];
    const bool registered=m["registered"],ja=language_=="ja";
    auto t=[&](const char* jp,const char* en){return std::string(ja?jp:en);};
    Text("standby-title",registered?t("あなたの待機","Your standby"):t("公開で対戦を待つ","Open your listing"));
    Text("start-matching",t("公開マッチング待機開始","Start public standby"));
    const bool actualPublic=registered?m["public"].get<bool>():true;
    Text("visibility-help",actualPublic?t("名前とコードは公開されます。接続相手にIPアドレスが伝わります。","Your name and code are public. Opponents can see your IP address."):t("コードを相手に共有してください。","Share this code with your opponent."));
    Show("visibility-help",!registered||!actualPublic);
    Show("matching-status",registered||m["state"]!="idle");
    Text("registration-visibility",m["public"].get<bool>()?t("公開一覧に掲載中","Public listing"):t("公開一覧には未掲載","Not listed publicly"));
    Text("switch-visibility",t("このコードで公開待機","List this code publicly"));
    Show("switch-visibility",registered&&!m["public"].get<bool>());
    Disable("switch-visibility",m["busy"].get<bool>());
    auto name=state_["profile"]["name"].get<std::string>();Text("matching-name",name.empty()?"PLAYER":name);
}
void RmlView::ProcessEvent(Rml::Event& event) {
    if(updating_)return;
    auto* target=event.GetTargetElement();
    const auto type=event.GetType();
    if(type=="keydown" && (target->GetTagName()=="button" || target->GetTagName()=="summary")) {
        auto key=event.GetParameter<int>("key_identifier",0);
        if(key==Rml::Input::KI_RETURN || key==Rml::Input::KI_SPACE) {
            event.StopPropagation(); if(!Disabled(target))target->Click();
        }
        return;
    }
    if(type=="click") {
        while(target && target->GetTagName()!="button" && target->GetTagName()!="summary")target=target->GetParentNode();
        if(!target || Disabled(target))return;
        if(target->GetTagName()=="summary") {
            auto* details=target->GetParentNode(); details->SetClass("open",!details->IsClassSet("open"));
            if(details->GetId()=="log-details")send_({{"type","view"},{"log",details->IsClassSet("open")}});
            return;
        }
    } else if(type!="change" && type!="blur")return;
    if(Disabled(target))return;
    const auto id=target->GetId();
    const bool checked=target->HasAttribute("checked");
    auto command=[&](const char* t){send_({{"type",t}});};
    if(type=="click") {
        if(target->HasAttribute("data-page"))Navigate(target->GetAttribute<std::string>("data-page",""));
        else if(id=="matching-tab")MatchingTab("public");
        else if(id=="direct-tab")MatchingTab("direct");
        else if(id=="controller-p1"||id=="controller-p2")send_({{"type","controller"},{"action","player"},{"player",id=="controller-p1"?0:1}});
        else if(id=="controller-refresh"||id=="controller-cancel")send_({{"type","controller"},{"action",id=="controller-refresh"?"refresh":"cancel"}});
        else if(id=="language")send_({{"type","language"},{"value",language_=="ja"?"en":"ja"}});
        else if(id=="start-matching")send_({{"type","matching_start"},{"public",true},{"port",Number(Value("matching-port"))},{"comment",""}});
        else if(id=="switch-visibility")send_({{"type","matching_visibility"},{"public",true}});
        else if(id=="stop-matching")command("matching_stop");
        else if(id=="retry-cleanup")command("matching_cleanup");
        else if(id=="cancel-request")command("matching_cancel");
        else if(id=="cancel-pairing")command("matching_cancel_pairing");
        else if(id=="start-direct") {
            const auto code=TrimCode(Value("direct-code"));
            send_({{"type","launch"},{"mode","auto"},{"port",Number(Value("direct-port"))},{"code",code}});
        }
        else if(id=="invite-selected"||id=="watch-selected") {
            if(const auto* person=SelectedPerson();person && !person->value("self",false)) {
                if(id=="invite-selected")send_({{"type","matching_invite"},{"code",(*person)["code"]}});
                else send_({{"type","launch"},{"mode","spectate"},{"code",(*person)["code"]}});
            }
        }
        else if(id=="watch")send_({{"type","launch"},{"mode","spectate"},{"code",Value("spectator-code")}});
        else if(id=="training"||id=="replay")send_({{"type","launch"},{"mode",id}});
        else if(id=="cancel-session")command("cancel_session");
        else if(id=="use-reply"||id=="manual-start")send_({{"type","manual_reply"},{"code",id=="manual-start"?"start":Value("manual-peer")}});
        else if(id=="dismiss-notice")command("dismiss_notice");
        else if(id=="show-requests") {Navigate("matching");Find("requests-card")->ScrollIntoView();}
        else if(id=="emblem-choose")command("emblem_choose");
        else if(id=="emblem-remove")command("emblem_remove");
        else if(id=="test-notification")command("test_notification");
        else if(id=="save-server")send_({{"type","settings"},{"key","NtfyServer"},{"value",Value("ntfy-server")}});
        else if(id=="prev"||id=="next") {listPage_+=id=="prev"?-1:1;selectedPerson_.clear();peopleSignature_.clear();People();Find("people-window")->SetScrollTop(0);}
        else if(id=="copy-matching"||id=="copy-session"||id=="copy-watch"||id=="copy-manual")send_({{"type","copy_code"},{"source",id.substr(5)}});
        else if(target->HasAttribute("data-command")) {
            const auto operation=target->GetAttribute<std::string>("data-command","");
            const auto value=target->GetAttribute<std::string>("data-value","");
            if(operation=="select-person") {selectedPerson_=value;People();if(auto* row=Find(id))row->Focus();}
            else if(operation=="emblem-preset")send_({{"type","emblem_preset"},{"code",value}});
            else if(operation=="controller-capture"||operation=="controller-clear")send_({{"type","controller"},{"action",operation=="controller-capture"?"capture":"clear"},{"row",Number(value)}});
            else if(operation=="accept"||operation=="reject")send_({{"type","matching_"+operation},{"request",value}});
        }
    } else if(id=="player-name" && type=="blur") {
        if(!state_.empty() && Value("player-name")!=state_["profile"]["name"].get<std::string>())
            send_({{"type","profile"},{"name",Value("player-name")}});
    } else if(type=="change") {
        if(id=="pause-matching")send_({{"type","matching_pause"},{"paused",checked}});
        else if(id=="controller-device")send_({{"type","controller"},{"action","device"},{"device",Value("controller-device")}});
        else if(id=="training-standby")send_({{"type","settings"},{"key","TrainingStandby"},{"value",checked}});
        else if(id=="connection-preference")send_({{"type","settings"},{"key","ConnectionPreference"},{"value",Number(Value("connection-preference"))}});
        else {
            const std::map<std::string,std::string> keys={{"sound","Sound"},{"flash","FlashTaskbar"},{"popup","DesktopPopup"},{"allow-spectators","AllowSpectators"},{"software-rendering","SoftwareRendering"}};
            if(auto it=keys.find(id);it!=keys.end())send_({{"type","settings"},{"key",it->second},{"value",checked}});
        }
    }
}
const Json* RmlView::SelectedPerson() const {
    if(state_.empty()||selectedPerson_.empty())return nullptr;
    const auto& people=state_["matching"]["people"];
    // 並び替わりや掲載終了後にも、別の相手へ選択を移さない。
    for(size_t i=listPage_*20;i<people.size()&&i<size_t((listPage_+1)*20);++i)
        if(people[i]["id"]==selectedPerson_)return &people[i];
    return nullptr;
}
void RmlView::People() {
    if(state_.empty())return;
    const auto& m=state_["matching"];const auto& people=m["people"];
    const int pages=std::max(1,int((people.size()+19)/20));listPage_=std::clamp(listPage_,0,pages-1);
    Disable("prev",listPage_==0);Disable("next",listPage_+1>=pages);
    Text("page-count",std::to_string(listPage_+1)+" / "+std::to_string(pages));
    Text("people-count",std::to_string(people.size())+(language_=="ja"?"名":" players"));
    const bool locked=m["busy"].get<bool>()||m["paused"].get<bool>()||!m["outgoing"]["id"].get<std::string>().empty();
    const bool running=state_["session"]["running"];
    const auto* selected=SelectedPerson();if(!selected)selectedPerson_.clear();
    const bool ja=language_=="ja";
    const bool self=selected && selected->value("self",false);
    Text("selected-player",selected?selected->at("name").get<std::string>()+(self?(ja?"（自分の募集）":" (your listing)"):""):(ja?"プレイヤーを選択してください":"Select a player"));
    Disable("invite-selected",!selected||locked||self);Disable("watch-selected",!selected||running||self);
    const auto result=selected?selected->at("result").get<std::string>():std::string{};
    Text("selected-result",result);Show("selected-result",!result.empty()&&result!="掲載中・状態未確認"&&result!="Listed / status not checked");
    const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<std::string> ages;
    for(size_t i=listPage_*20;i<people.size()&&i<size_t((listPage_+1)*20);++i)
        ages.push_back(PublicListingAge(people[i].value("listedAt",int64_t(0)),now));
    const auto signature=Json{people,listPage_,language_,selectedPerson_,ages}.dump();if(signature==peopleSignature_)return;peopleSignature_=signature;
    std::string rml;
    if(people.empty())rml="<div class='empty'><strong>"+std::string(ja?"表示できる募集はありません":"No listings to display")+"</strong><p>"+(ja?"自分から公開待機を始められます。":"You can start your own public standby.")+"</p></div>";
    for(size_t i=listPage_*20;i<people.size()&&i<size_t((listPage_+1)*20);++i) {
        const auto& p=people[i];const bool active=p["id"]==selectedPerson_;
        const auto& name=p["name"].get_ref<const std::string&>();
        const auto nameEnd=Rml::StringUtilities::ConvertCharacterOffsetToByteOffset(name,10);
        const auto displayName=name.substr(0,nameEnd)+(size_t(nameEnd)<name.size()?"…":"");
        const auto index=std::to_string(i);
        const bool own=p.value("self",false);
        rml+="<button id='player-"+index+"' class='player-row"+(active?std::string(" selected"):std::string{})+(own?" own":"")+"' aria-pressed='"+(active?"true":"false")+"' data-command='select-person' data-value='"+Escape(p["id"])+"'><span id='player-name-"+index+"' class='player-name'>"+Escape(displayName)+"</span>"+(own?std::string("<span class='player-meta'><span class='player-self'>")+(ja?"自分":"You")+"</span>":std::string{})+"<span id='player-age-"+index+"' class='player-age'>"+ages[i-listPage_*20]+"</span>"+(own?"</span>":"")+"</button>";
    }
    Find("people")->SetInnerRML(rml);
}
void RmlView::Requests() {
    const auto& m=state_["matching"]; Show("requests-card",!m["incoming"].empty());
    const bool locked=!m["canAccept"].get<bool>(), ja=language_=="ja";
    const auto signature=Json{m["incoming"],locked,language_}.dump();
    if(signature!=requestSignature_) {
        requestSignature_=signature;std::string rml;int i=0;
        for(const auto& r:m["incoming"]) {
            const auto value=Escape(r["id"]),index=std::to_string(i++);
            rml+="<article class='request'><strong>"+Escape(r["peer"]["name"])+"</strong><p>"+Escape(r["peer"]["comment"])+"</p><p class='subtle' id='expires-"+index+"'></p><div class='request-actions'>";
            rml+="<button id='accept-"+index+"' data-command='accept' data-value='"+value+"'"+(locked?" disabled=''":"")+">"+(ja?"承諾する":"Accept")+"</button><button id='reject-"+index+"' data-command='reject' data-value='"+value+"'>"+(ja?"断る":"Decline")+"</button></div></article>";
        }
        Find("requests")->SetInnerRML(rml);
        for(int index=0;index<i;++index)Disable(("accept-"+std::to_string(index)).c_str(),locked);
    }
    const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    int i=0;for(const auto& r:m["incoming"]) {
        const auto seconds=std::max<int64_t>(0,r["expires"].get<int64_t>()-now);
        Text(("expires-"+std::to_string(i++)).c_str(),ja?"返答期限：あと"+std::to_string(seconds)+"秒":"Reply within "+std::to_string(seconds)+" seconds");
    }
}
void RmlView::State(const Json& value) {
    if(value.value("protocol",0)!=1)return;
    updating_=true;state_=value;
    SortPublicPlayers(state_["matching"]["people"]);
    if(language_!=value["language"].get<std::string>())Translate(value["language"]);
    auto t=[&](const char* ja,const char* en){return std::string(language_=="ja"?ja:en);};
    const auto& m=value["matching"];const auto& s=value["session"];const auto& c=value["settings"];const auto& p=value["profile"];
    const bool registered=m["registered"],busy=m["busy"],running=s["running"],game=s["game"],outgoing=!m["outgoing"]["id"].get<std::string>().empty();
    Show("loading",false);Text("version",value["version"]);Text("error",value["error"]);Show("error",!value["error"].get<std::string>().empty());Show("incoming-banner",s["incomingNotice"]);
    Text("matching-status",m["status"]);Text("matching-notice",m["notice"]);Text("matching-error",m["error"]);Text("service-status",m["service"]);
    Show("matching-notice",!m["notice"].get<std::string>().empty());Show("matching-error",!m["error"].get<std::string>().empty());
    Show("other-mode",m["otherMode"]);Show("registration-active",registered);Show("registration-start",!registered);Text("matching-code",m["code"]);
    Show("retry-cleanup",m.value("cleanupPending",0U)>0);Disable("retry-cleanup",busy||outgoing);
    Registration();
    auto check=[&](const char* id,bool enabled){auto* e=Find(id);if(enabled)e->SetAttribute("checked","");else e->RemoveAttribute("checked");};
    check("pause-matching",m["paused"]);Disable("matching-port",registered||busy);Disable("start-matching",busy);
    check("training-standby",c["TrainingStandby"]);Disable("training-standby",registered||busy);
    if(!initialized_){SetValue("matching-port",std::to_string(c["port"].get<int>()));SetValue("direct-port",std::to_string(c["port"].get<int>()));initialized_=true;send_({{"type","matching_open"}});}
    Text("outgoing-text",m["outgoing"]["peer"]["name"].get<std::string>()+t(" さんの承諾を待っています"," — waiting for acceptance"));Show("outgoing",outgoing);Show("cancel-pairing",m["state"]=="confirming");
    Show("incomplete",m["incomplete"]);People();Requests();
    Disable("direct-fields",busy||outgoing);Show("end-matching-hint",registered);
    Show("direct-lookup",value.value("connectionLookup",false));
    Disable("direct-port",busy||outgoing||registered);
    if(registered) {SetValue("direct-port",std::to_string(m["port"].get<int>()),true);SetValue("matching-port",std::to_string(m["port"].get<int>()),true);}
    for(const char* id:{"watch","training","replay"})Disable(id,running||value.value("connectionLookup",false));
    Disable("profile-fields",running||registered);SetValue("player-name",p["name"]);Show("profile-error",p["error"]);Disable("emblem-remove",running||registered||p["emblemId"].get<uint64_t>()==0);
    Show("profile-locked",running||registered);
    const auto imageId=p["emblemId"].get<uint64_t>();const auto source="emblem:"+std::to_string(imageId);
    for(const char* id:{"emblem","matching-emblem"}) {
        auto* emblem=Find(id);
        if(emblem->GetAttribute<std::string>("src","")!=source)emblem->SetAttribute("src",source);
    }
    Show("matching-emblem",imageId!=0);Show("matching-emblem-placeholder",imageId==0);
    for(auto [id,key]:std::vector<std::pair<const char*,const char*>>{{"sound","Sound"},{"flash","FlashTaskbar"},{"popup","DesktopPopup"},{"allow-spectators","AllowSpectators"},{"software-rendering","SoftwareRendering"}})check(id,c[key]);
    SetValue("connection-preference",std::to_string(c["ConnectionPreference"].get<int>()));SetValue("ntfy-server",c["NtfyServer"]);
    Disable("allow-spectators",busy||outgoing||!m["incoming"].empty());Disable("save-server",busy||registered);
    Text("renderer-status",value["display"]["software"].get<bool>()?t("CPU描画","CPU rendering"):t("GPU描画","GPU rendering"));
    Show("session-card",running||s["failed"].get<bool>()||!s["log"].get<std::string>().empty()||s["status"]!=t("開始できます。","Ready when you are."));
    Find("session-card")->SetClass("failed",s["failed"]);Text("session-status",s["status"]);
    Text("session-badge",running?(game?t("ゲーム実行中","Game running"):t("接続・待機中","Connecting / waiting")):t("待機","Idle"));
    std::string routes;int index=0;for(const auto& r:s["routes"]) {if(!r.get<std::string>().empty())routes+=(index?"IPv6: ":"IPv4: ")+r.get<std::string>()+"\n";++index;}Text("routes",routes);
    Show("share-code",running&&!s["code"].get<std::string>().empty()&&!game);Text("session-code",s["code"]);Show("copy-watch",running&&!s["watchCode"].get<std::string>().empty());
    Show("manual",running&&!game&&!s["manualCode"].get<std::string>().empty());SetValue("manual-code",s["manualCode"]);Show("manual-host",!s["code"].get<std::string>().empty());Show("manual-start",s["code"].get<std::string>().empty());
    Show("cancel-session",running&&!game);Disable("cancel-session",s["cancelling"]);
    if(Find("log-details")->IsClassSet("open"))Text("session-log",s["log"]);
    Controllers();
    updating_=false;
}
void RmlView::Controllers() {
    if(!state_.contains("controller"))return;
    const auto& c=state_["controller"];const bool ja=language_=="ja",locked=c["locked"],connected=c["connected"];
    const int capture=c["capture"],player=c["player"];
    const bool selectedExists=std::any_of(c["devices"].begin(),c["devices"].end(),[&](const auto& d){return d["id"]==c["selected"];});
    const bool missing=c["assigned"].get<bool>()&&!selectedExists;
    const auto signature=Json{c["devices"],missing,language_}.dump();
    if(signature!=controllerDevicesSignature_) {
        controllerDevicesSignature_=signature;
        auto* select=dynamic_cast<Rml::ElementFormControlSelect*>(Find("controller-device"));
        // RmlUiはoptionを内部selectboxへ移すため、SetInnerRMLだけでは旧候補が残る。
        // 候補専用APIで置換し、選択だけの変更では一覧を作り直さない。
        select->HideSelectBox();
        select->RemoveAll();
        select->Add(ja?"割当なし":"Unassigned","");
        for(const auto& d:c["devices"])select->Add(Escape(d["label"]),d["id"]);
        if(missing)select->Add(ja?"保存済みの機器（未接続／再検出が必要）":"Saved device (disconnected / refresh needed)","missing");
        select->SetValue(missing?"missing":c["selected"].get<std::string>());
    }
    // 保存失敗や重複割当の拒否でも、選択欄を保存済みの実状態へ戻す。
    SetValue("controller-device",missing?"missing":c["selected"].get<std::string>(),true);
    for(int p=0;p<2;++p) {
        auto* tab=Find(p==0?"controller-p1":"controller-p2");tab->SetClass("active",player==p);tab->SetAttribute("aria-selected",player==p?"true":"false");
    }
    Show("controller-locked",locked);Show("controller-disconnected",!locked&&c["assigned"].get<bool>()&&!connected);
    Disable("controller-fields",locked);Disable("controller-bindings",locked||!connected);
    Show("controller-cancel",capture>=0);Show("controller-capture-hint",capture>=0);
    Text("controller-status",c["notice"]);
    for(int i=0;i<13;++i) {
        const auto id="controller-bind-"+std::to_string(i);
        Text(id.c_str(),capture==i?(ja?"入力を待っています…":"Waiting for input…"):c["bindings"][i].get<std::string>());
        Find(id)->SetClass("capturing",capture==i);
    }
}
Json RmlView::Save() const {
    Json result={{"page",page_},{"matchingTab",matchingTab_},{"selectedPerson",selectedPerson_},{"listPage",listPage_},{"log",Find("log-details")->IsClassSet("open")},{"options",Find("matching-options")->IsClassSet("open")},{"values",Json::object()}};
    Walk(document_,[&](auto* e){if(auto* form=dynamic_cast<Rml::ElementFormControl*>(e);form&&!e->GetId().empty())result["values"][e->GetId()]=form->GetValue();});
    return result;
}
void RmlView::Restore(const Json& saved) {
    if(saved.empty())return;
    updating_=true;
    for(auto it=saved["values"].begin();it!=saved["values"].end();++it)SetValue(it.key().c_str(),it.value(),true);
    listPage_=saved.value("listPage",0);MatchingTab(saved.value("matchingTab","public"));Navigate(saved.value("page","matching"));
    selectedPerson_=saved.value("selectedPerson","");People();
    Find("matching-options")->SetClass("open",saved.value("options",false));
    Find("log-details")->SetClass("open",saved.value("log",false));updating_=false;
}
Json RmlView::Inspect() const {
    Json elements=Json::object();
    Json overflow=Json::array();
    auto* main=Find("content");
    const auto right=main->GetAbsoluteOffset(Rml::BoxArea::Content).x+main->GetBox().GetSize(Rml::BoxArea::Content).x;
    Walk(main,[&](auto* e){
        const auto pos=e->GetAbsoluteOffset(Rml::BoxArea::Border),size=e->GetBox().GetSize(Rml::BoxArea::Border);
        if(e!=main && e->IsVisible(true) && pos.x+size.x>right+1)
            overflow.push_back({{"tag",e->GetTagName()},{"id",e->GetId()},{"classes",e->GetClassNames()},{"rect",{pos.x,pos.y,size.x,size.y}}});
    });
    Walk(document_,[&](auto* e){if(e->GetId().empty())return;
        const auto pos=e->GetAbsoluteOffset(Rml::BoxArea::Border),size=e->GetBox().GetSize(Rml::BoxArea::Border);
        elements[e->GetId()]={{"visible",e->IsVisible(true)},{"disabled",Disabled(e)},{"checked",e->HasAttribute("checked")},{"value",Value(e->GetId().c_str())},{"text",e->GetInnerRML()},{"rect",{pos.x,pos.y,size.x,size.y}},
            {"clientWidth",e->GetClientWidth()},{"scrollWidth",e->GetScrollWidth()}};
        if(auto* select=dynamic_cast<Rml::ElementFormControlSelect*>(e)) {
            auto& options=elements[e->GetId()]["options"];options=Json::array();
            for(int i=0;i<select->GetNumOptions();++i) {
                auto* option=select->GetOption(i);
                options.push_back({{"value",option->GetAttribute<std::string>("value","")},{"label",option->GetInnerRML()}});
            }
        }
    });
    return {{"page",page_},{"elements",elements},{"overflow",overflow},{"state",state_}};
}
Json RmlView::Test(const Json& request) {
    const auto action=request.value("action","");
    if(action=="inspect")return Inspect();
    auto* e=Find(request.value("id",""));if(!e)throw std::runtime_error("unknown UI element");
    if(action=="click") {
        if(!e->IsVisible(true)||Disabled(e))throw std::runtime_error("UI element is hidden or disabled");
        e->ScrollIntoView();context_.Update();
        const auto pos=e->GetAbsoluteOffset(Rml::BoxArea::Border),size=e->GetBox().GetSize(Rml::BoxArea::Border);
        context_.ProcessMouseMove(int(pos.x+size.x/2),int(pos.y+size.y/2),0);
        context_.ProcessMouseButtonDown(0,0);context_.ProcessMouseButtonUp(0,0);
    } else if(action=="value") {
        SetValue(e->GetId().c_str(),request.at("value"),true);e->DispatchEvent("change",{});
    } else if(action=="focus")e->Focus();
    else if(action=="type") {
        e->Focus();context_.ProcessKeyDown(Rml::Input::KI_A,Rml::Input::KM_CTRL);context_.ProcessKeyUp(Rml::Input::KI_A,0);
        context_.ProcessKeyDown(Rml::Input::KI_BACK,0);context_.ProcessKeyUp(Rml::Input::KI_BACK,0);context_.ProcessTextInput(request.at("value").get<std::string>());
    } else if(action=="scroll")e->SetScrollTop(request.at("value").get<float>());
    else throw std::runtime_error("unknown UI test action");
    context_.Update();return Inspect();
}
}
