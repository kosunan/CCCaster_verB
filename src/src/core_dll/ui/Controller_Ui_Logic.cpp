#include "shared_contracts/NativePath.hpp"
#include "core_dll/ui/Controller_Ui_Logic.hpp"
#include "core_dll/common/DataPaths.hpp"
#include "core_dll/common/InputDiagnostic.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include <imgui.h>
#include <fstream>
#include <vector>
#include <algorithm>

namespace cccaster::domain::ui {
namespace {
using L=ControllerUiLogic;
using namespace cccaster::input;
using Hook=cccaster::game_interface::DirectInputHook;
using cccaster::main_app::Config;
using cccaster::main_app::ConfigManager;
struct Player {
    DeviceIdentity device;
    Bindings binds{};
    int row=L::OverviewRow, capture=-1;
    std::string pending, releaseInput;
};
std::array<Player,2> players;
bool active=false, closeRequested=false;
std::string error;
std::string ProfilePath(const DeviceIdentity& device,bool read) {
    const auto path=cccaster::core::paths::Resolve(ProfileFilename(device));
    if(read && !std::ifstream(cccaster::Utf8Path(path)).good())
        return cccaster::core::paths::Resolve(ProfileFilename(device,true));
    return path;
}
void Error(const std::string& text) {
    error=text;
    if(cccaster::diagnostics::input::Enabled())
        cccaster::domain::session::DebugLog("[InputSetup] %s",text.c_str());
}
void LoadBindings(Player& player) {
    Config config;
    if(!player.device.Empty()) config.Load(ProfilePath(player.device,true));
    const auto defaults=DefaultBindings(player.device.name=="Keyboard");
    for(int i=0;i<BindingCount;++i)
        player.binds[i]=config.GetString("Mapping",BindingKeys[i],defaults[i]);
}
bool SaveAllocation(int p,const DeviceIdentity& device) {
    const auto path=cccaster::core::paths::Resolve("cccaster.ini");
    Config main; main.Load(path);
    const std::string prefix=p ? "P2" : "P1";
    if(main.GetString("Settings",prefix+"Device")==device.name &&
       main.GetString("Settings",prefix+"DeviceGuid")==device.guid) return true;
    main.SetString("Settings",prefix+"Device",device.name);
    main.SetString("Settings",prefix+"DeviceGuid",device.guid);
    if(!main.SaveChecked(path)) { Error("Cannot save player device. Check file permissions and try again."); return false; }
    ConfigManager::Load(path);
    return true;
}
bool SaveBindings(int p,const Bindings& binds) {
    auto& player=players[p];
    Config config; config.Load(ProfilePath(player.device,true));
    // 補助方向・TrainingSave/Load・その他の既存設定は変更しない。
    for(int i=0;i<SaveStateBinding;++i) config.SetString("Mapping",BindingKeys[i],binds[i]);
    if(!config.SaveChecked(ProfilePath(player.device,false))) {
        Error("Key not saved. Check file permissions and assign it again."); return false;
    }
    player.binds=binds;
    error.clear();
    SaveAllocation(p,player.device); // 旧名前形式の設定は操作時にGUIDへ固定する。
    Hook::ReloadConfigs();
    return true;
}
bool Held(int id,const std::string& bind) {
    if(bind.empty()) return false;
    if(id==-2) {
        for(int key=ImGuiKey_NamedKey_BEGIN;key<=ImGuiKey_AppForward;++key)
            if(bind==ImGui::GetKeyName(static_cast<ImGuiKey>(key)))
                return ImGui::IsKeyDown(static_cast<ImGuiKey>(key));
    }
    return Hook::IsBindingPressed(id,bind);
}
std::string KeyboardEdge() {
    for(int key=ImGuiKey_NamedKey_BEGIN;key<=ImGuiKey_AppForward;++key) {
        if(key==ImGuiKey_F4 || key==ImGuiKey_Escape) continue;
        const bool repeat=key==ImGuiKey_UpArrow || key==ImGuiKey_DownArrow;
        if(ImGui::IsKeyPressed(static_cast<ImGuiKey>(key),repeat))
            return ImGui::GetKeyName(static_cast<ImGuiKey>(key));
    }
    return {};
}
int Direction(int id,const std::string& edge,const Player* player=nullptr) {
    const char* keys[]{"UpArrow","DownArrow","LeftArrow","RightArrow"};
    const char* hats[]{"H0_8","H0_2","H0_4","H0_6"};
    const char* axes[]{"A1+","A1-","A0-","A0+"};
    for(int i=0;i<4;++i)
        if((id==-2 && edge==keys[i]) || (id>=0 && (edge==hats[i] || edge==axes[i] ||
           (player && !edge.empty() && edge==player->binds[i])))) return i;
    return -1;
}
void SelectRow(int p,int row,bool continueDirections=false) {
    L::CancelCapture(p);
    auto& player=players[p]; player.row=row;
    if(row>=0 && row<L::DoneRow && (row>=4 || continueDirections)) player.capture=row;
}
void Commit(int p,const std::string& input) {
    auto& player=players[p];
    const int binding=player.capture;
    auto binds=player.binds;
    // 旧版の物理入力→動作の対応に合わせ、同じ入力の以前の割当を外す。
    for(int i=0;i<SaveStateBinding;++i) if(binds[i]==input) binds[i].clear();
    binds[binding]=input;
    if(!SaveBindings(p,binds)) return;
    // 最後のA+Bでは止まる。Doneへは下入力で移動する。
    SelectRow(p,(std::min)(binding+1,SaveStateBinding-1),true);
    player.releaseInput=input;
}
}

void ControllerUiLogic::BeginUiSession() {
    if(active) return;
    active=true; closeRequested=false; error.clear();
    const auto devices=Hook::GetConnectedDevices();
    for(int p=0;p<2;++p) {
        auto& player=players[p]; player={};
        const std::string prefix=p ? "P2" : "P1";
        player.device={ConfigManager::GetString("Settings",prefix+"Device",""),
                       ConfigManager::GetString("Settings",prefix+"DeviceGuid","")};
        const int id=ResolveDevice(player.device,devices);
        if(id>=0) player.device=IdentifyDevice(id,devices);
        LoadBindings(player);
    }
}
int ControllerUiLogic::DeviceId(int p) { return ResolveDevice(players[p].device,Hook::GetConnectedDevices()); }
const DeviceIdentity& ControllerUiLogic::Device(int p) { return players[p].device; }
const Bindings& ControllerUiLogic::Binds(int p) { return players[p].binds; }
int ControllerUiLogic::SelectedRow(int p) { return players[p].row; }
int ControllerUiLogic::CaptureBinding(int p) { return players[p].capture; }
const std::string& ControllerUiLogic::Status() { return error; }
bool ControllerUiLogic::StatusError() { return !error.empty(); }
bool ControllerUiLogic::SelectDevice(int p,int joyId) {
    const auto device=IdentifyDevice(joyId,Hook::GetConnectedDevices());
    if(device==players[p].device) return true;
    if(!device.Empty() && device==players[1-p].device) return false;
    if(!SaveAllocation(p,device)) return false;
    auto& player=players[p]; player={}; player.device=device;
    LoadBindings(player);
    error.clear(); Hook::ReloadConfigs();
    return true;
}
void ControllerUiLogic::CancelCapture(int p) {
    if(p<0) { CancelCapture(0); CancelCapture(1); return; }
    auto& player=players[p]; player.capture=-1; player.pending.clear(); player.releaseInput.clear();
}
void ControllerUiLogic::StartBinding(int p,int binding) {
    if(DeviceId(p)==-1 || binding<0 || binding>=SaveStateBinding) return;
    if(DeviceId(p)>=0 && binding<4) return; // パッド方向は既存の十字／スティック割当を使う。
    SelectRow(p,binding,true);
}
bool ControllerUiLogic::ClearBinding(int p,int binding) {
    if(DeviceId(p)==-1 || binding<0 || binding>=SaveStateBinding) return false;
    if(DeviceId(p)>=0 && binding<4) return false;
    auto binds=players[p].binds; binds[binding].clear();
    if(!SaveBindings(p,binds)) return false;
    CancelCapture(p); // 旧版同様、削除後は別の行を選んで登録を再開する。
    return true;
}
void ControllerUiLogic::Navigate(int p,int delta) {
    if(DeviceId(p)==-1) return;
    const int first=DeviceId(p)==-2 ? 0 : 4;
    const int count=DoneRow-first+2; // 機器名、設定項目、Done
    const int current=players[p].row==OverviewRow ? 0 : players[p].row-first+1;
    const int index=(current+delta%count+count)%count;
    SelectRow(p,index==0 ? OverviewRow : first+index-1);
}
void ControllerUiLogic::Done(int p) {
    SelectRow(p,OverviewRow);
    closeRequested=true; // 両側の入力処理後に終了を判定する。
}
bool ControllerUiLogic::TakeCloseRequest() {
    const bool close=closeRequested; closeRequested=false;
    for(const auto& player:players)
        if(!player.device.Empty() && player.row!=OverviewRow) return false;
    return close;
}
void ControllerUiLogic::Update() {
    struct Event { int id; std::string edge; };
    std::vector<Event> events{{-2,KeyboardEdge()}};
    for(const auto& device:Hook::GetConnectedDevices())
        events.push_back({device.id,Hook::GetAnyInputEdge(device.id)});
    // 左右は機器の参加・解除。キーボードの方向登録中だけ通常のキー入力として扱う。
    for(const auto& event:events) {
        if(event.edge.empty()) continue;
        int owner=-1;
        for(int p=0;p<2;++p) if(DeviceId(p)==event.id) owner=p;
        if(owner>=0 && event.id==-2 && players[owner].capture>=0 && players[owner].capture<4) continue;
        const int direction=Direction(event.id,event.edge,owner<0 ? nullptr : &players[owner]);
        if(direction<2) continue;
        if(owner>=0) {
            if(direction==(owner==0 ? 3 : 2)) SelectDevice(owner,-1);
        } else {
            const int p=direction==2 ? 0 : 1;
            if(players[p].device.Empty()) SelectDevice(p,event.id);
        }
    }
    const bool escape=ImGui::IsKeyPressed(ImGuiKey_Escape,false);
    for(int p=0;p<2;++p) {
        auto& player=players[p]; const int id=DeviceId(p);
        if(id==-1) { CancelCapture(p); player.row=OverviewRow; continue; }
        std::string edge;
        for(const auto& event:events) if(event.id==id) edge=event.edge;
        if(escape) { CancelCapture(p); continue; }
        if(!player.releaseInput.empty()) {
            if(Held(id,player.releaseInput)) continue;
            player.releaseInput.clear();
        }
        if(!player.pending.empty()) {
            if(Held(id,player.pending)) continue;
            const auto input=player.pending; player.pending.clear();
            Commit(p,input); continue;
        }
        // キーボード方向はEnterの解放後に入力待ちへ入り、実キーの押下で登録。
        if(id==-2 && player.capture>=0 && player.capture<4) {
            if(!edge.empty()) Commit(p,edge);
            continue;
        }
        const int direction=Direction(id,edge,&player);
        if(direction==0 || direction==1) { Navigate(p,direction==0 ? -1 : 1); continue; }
        if(direction==(p==0 ? 2 : 3) && player.row>=0 && player.row<DoneRow) {
            ClearBinding(p,player.row); continue;
        }
        if(id==-2 && player.row>=0 && player.row<4) {
            if(edge=="Enter") { StartBinding(p,player.row); player.releaseInput=edge; }
            else if(edge=="Delete") ClearBinding(p,player.row);
            continue;
        }
        const bool button=!edge.empty() && (id==-2 ? edge=="Enter" : edge[0]=='B');
        if(player.row==DoneRow && button) { Done(p); continue; }
        if(player.capture<0 || edge.empty() || direction>=0) continue;
        if(id==-2) Commit(p,edge);
        else player.pending=edge;
    }
}
bool ControllerUiLogic::EndUiSession() {
    CancelCapture(); active=false; closeRequested=false;
    return true;
}
void ControllerUiLogic::Suspend() { EndUiSession(); }
} // namespace cccaster::domain::ui
