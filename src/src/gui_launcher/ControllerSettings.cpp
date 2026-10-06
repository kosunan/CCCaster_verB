#define DIRECTINPUT_VERSION 0x0800
#include "ControllerSettings.hpp"
#include "AppContext.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include "core_dll/hook/ControllerProfile.hpp"
#include "core_dll/common/InputDiagnostic.hpp"
#include "core_dll/ui/ControllerMappingValidation.hpp"
#include "shared_contracts/NativePath.hpp"
#include "shared_contracts/ConfigPath.hpp"
#include <dinput.h>
#include <set>
#include <cstdio>

namespace cccaster::gui {
namespace {
using namespace cccaster::input;
using main_app::Config;
using main_app::ConfigManager;
std::string Token(const DeviceIdentity& d) { return d.Empty()?"":d.name=="Keyboard"?"keyboard":d.guid; }
std::string Guid(const GUID& g) {
    char s[40]{};
    std::snprintf(s,sizeof(s),"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        static_cast<unsigned long>(g.Data1),g.Data2,g.Data3,g.Data4[0],g.Data4[1],g.Data4[2],g.Data4[3],g.Data4[4],g.Data4[5],g.Data4[6],g.Data4[7]);
    return s;
}
std::string DisplayName(const std::string& name) {
    const int n=MultiByteToWideChar(CP_ACP,0,name.data(),int(name.size()),nullptr,0);
    std::wstring wide(n,L'\0');MultiByteToWideChar(CP_ACP,0,name.data(),int(name.size()),wide.data(),n);
    return PathUtf8(std::filesystem::path(wide));
}
std::filesystem::path Profile(const DeviceIdentity& d,bool read) {
    auto path=exePath.parent_path()/Utf8Path(ProfileFilename(d));
    if(read && !std::filesystem::exists(path))path=exePath.parent_path()/Utf8Path(ProfileFilename(d,true));
    return path;
}
std::string KeyName(WPARAM vk,LPARAM flags) {
    if(vk>='A'&&vk<='Z')return std::string(1,char(vk));
    if(vk>='0'&&vk<='9')return std::string(1,char(vk));
    if(vk>=VK_NUMPAD0&&vk<=VK_NUMPAD9)return "Keypad"+std::to_string(vk-VK_NUMPAD0);
    if(vk>=VK_F1&&vk<=VK_F24) {
        if(vk==VK_F1||vk==VK_F3||vk==VK_F4||vk==VK_F8)return {};
        return "F"+std::to_string(vk-VK_F1+1);
    }
    if(vk==VK_SHIFT)return MapVirtualKeyW((flags>>16)&255,MAPVK_VSC_TO_VK_EX)==VK_RSHIFT?"RightShift":"LeftShift";
    if(vk==VK_CONTROL)return flags&(1<<24)?"RightCtrl":"LeftCtrl";
    if(vk==VK_MENU)return flags&(1<<24)?"RightAlt":"LeftAlt";
    const std::pair<int,const char*> keys[]{
        {VK_TAB,"Tab"},{VK_LEFT,"LeftArrow"},{VK_RIGHT,"RightArrow"},{VK_UP,"UpArrow"},{VK_DOWN,"DownArrow"},
        {VK_PRIOR,"PageUp"},{VK_NEXT,"PageDown"},{VK_HOME,"Home"},{VK_END,"End"},{VK_INSERT,"Insert"},
        {VK_DELETE,"Delete"},{VK_BACK,"Backspace"},{VK_SPACE,"Space"},{VK_RETURN,"Enter"},
        {VK_LSHIFT,"LeftShift"},{VK_RSHIFT,"RightShift"},{VK_LCONTROL,"LeftCtrl"},{VK_RCONTROL,"RightCtrl"},
        {VK_LMENU,"LeftAlt"},{VK_RMENU,"RightAlt"},{VK_APPS,"Menu"},{VK_OEM_7,"Apostrophe"},
        {VK_OEM_COMMA,"Comma"},{VK_OEM_MINUS,"Minus"},{VK_OEM_PERIOD,"Period"},{VK_OEM_2,"Slash"},
        {VK_OEM_1,"Semicolon"},{VK_OEM_PLUS,"Equal"},{VK_OEM_4,"LeftBracket"},{VK_OEM_5,"Backslash"},
        {VK_OEM_6,"RightBracket"},{VK_OEM_3,"GraveAccent"},{VK_CAPITAL,"CapsLock"},{VK_SCROLL,"ScrollLock"},
        {VK_NUMLOCK,"NumLock"},{VK_SNAPSHOT,"PrintScreen"},{VK_PAUSE,"Pause"},{VK_DECIMAL,"KeypadDecimal"},
        {VK_DIVIDE,"KeypadDivide"},{VK_MULTIPLY,"KeypadMultiply"},{VK_SUBTRACT,"KeypadSubtract"},{VK_ADD,"KeypadAdd"}};
    for(auto [code,name]:keys)if(vk==WPARAM(code))return name;
    return {};
}
}
struct ControllerSettings::Impl {
    struct Device { DeviceIdentity identity; std::string label; IDirectInputDevice8A* input=nullptr; bool connected=true; };
    IDirectInput8A* input=nullptr;
    std::vector<Device> devices;
    DeviceIdentity players[2];
    Bindings binds{};
    int player=0,capture=-1;
    WPARAM swallowed=0;
    bool opened=false,blocked=false,armed=false;
    std::string pending;
    std::set<std::string> initialHeld;
    std::set<std::string> observed;
    Message notice;
    ~Impl() { Release();if(input)input->Release(); }
    void Release() { for(auto& d:devices)if(d.input){d.input->Unacquire();d.input->Release();}devices.clear(); }
    void Cancel() { capture=-1;pending.clear();initialHeld.clear();armed=false; }
    Device* Selected() {
        for(auto& d:devices)if(d.identity==players[player])return &d;
        return nullptr;
    }
    void Load() {
        Config config;if(!players[player].Empty())config.Load(PathUtf8(Profile(players[player],true)));
        const auto defaults=DefaultBindings(players[player].name=="Keyboard");
        for(int i=0;i<BindingCount;++i)binds[i]=config.GetString("Mapping",BindingKeys[i],defaults[i]);
    }
    static BOOL CALLBACK Axis(const DIDEVICEOBJECTINSTANCEA* object,void* context) {
        auto device=static_cast<IDirectInputDevice8A*>(context);
        DIPROPRANGE range{};range.diph={sizeof(range),sizeof(range.diph),object->dwType,DIPH_BYID};
        range.lMin=-32768;range.lMax=32767;device->SetProperty(DIPROP_RANGE,&range.diph);
        DIPROPDWORD dead{};dead.diph={sizeof(dead),sizeof(dead.diph),object->dwType,DIPH_BYID};
        device->SetProperty(DIPROP_DEADZONE,&dead.diph);return DIENUM_CONTINUE;
    }
    static BOOL CALLBACK Enumerate(const DIDEVICEINSTANCEA* instance,void* context) {
        auto& self=*static_cast<Impl*>(context);IDirectInputDevice8A* device=nullptr;
        if(FAILED(self.input->CreateDevice(instance->guidInstance,&device,nullptr)))return DIENUM_CONTINUE;
        if(FAILED(device->SetDataFormat(&c_dfDIJoystick2)) ||
           FAILED(device->SetCooperativeLevel(guiWindow,DISCL_NONEXCLUSIVE|DISCL_BACKGROUND))) {
            device->Release();return DIENUM_CONTINUE;
        }
        device->EnumObjects(Axis,device,DIDFT_AXIS);
        self.devices.push_back({{SafeDeviceName(instance->tszInstanceName),Guid(instance->guidInstance)},DisplayName(instance->tszInstanceName),device});
        return DIENUM_CONTINUE;
    }
    void Refresh() {
        Cancel();Release();notice={};
        devices.push_back({{"Keyboard",""},"Keyboard",nullptr});
        if(!input && FAILED(DirectInput8Create(GetModuleHandleW(nullptr),DIRECTINPUT_VERSION,IID_IDirectInput8A,reinterpret_cast<void**>(&input),nullptr)))
            notice={"Could not detect controllers. Keyboard is available.","コントローラを検出できませんでした。キーボードは設定できます。"};
        if(input && FAILED(input->EnumDevices(DI8DEVCLASS_GAMECTRL,Enumerate,this,DIEDFL_ATTACHEDONLY)))
            notice={"Detection failed. Try Refresh.","機器の検出に失敗しました。再検出をお試しください。"};
        Config config;config.Load(PathUtf8(ConfigPath(exePath.parent_path())));
        ConfigManager::Load(PathUtf8(ConfigPath(exePath.parent_path())));
        for(int p=0;p<2;++p) {
            const std::string prefix=p?"P2":"P1";
            players[p]={config.GetString("Settings",prefix+"Device"),config.GetString("Settings",prefix+"DeviceGuid")};
            // 旧名形式は同名が1台のときだけ個体を解決する。閲覧では書き換えない。
            if(!players[p].Empty()&&players[p].name!="Keyboard"&&players[p].guid.empty()) {
                Device* found=nullptr;int count=0;
                for(auto& d:devices)if(d.identity.name==players[p].name){found=&d;++count;}
                if(count==1)players[p]=found->identity;
            }
        }
        Load();
    }
    bool Allocate(const DeviceIdentity& device) {
        if(!device.Empty()&&device==players[1-player]) {
            notice={"This device is assigned to the other player. Unassign it there first.","この機器はもう一方のプレイヤーに割当済みです。先にそちらの割当を解除してください。"};return false;
        }
        const auto path=PathUtf8(ConfigPath(exePath.parent_path()));
        Config config;config.Load(path);const std::string prefix=player?"P2":"P1";
        config.SetString("Settings",prefix+"Device",device.name);config.SetString("Settings",prefix+"DeviceGuid",device.guid);
        if(!config.SaveChecked(path)) {SaveError();return false;}
        ConfigManager::Load(path);players[player]=device;return true;
    }
    void SaveError() {notice={"Could not save. Check folder permissions and try again.","保存できませんでした。フォルダーの書込み権限を確認して再試行してください。"};}
    bool Save(Bindings values) {
        Config config;config.Load(PathUtf8(Profile(players[player],true)));
        // 補助方向・TrainingSave/Loadと未知の既存項目はそのまま保つ。
        for(int i=0;i<SaveStateBinding;++i)config.SetString("Mapping",BindingKeys[i],values[i]);
        if(!config.SaveChecked(PathUtf8(Profile(players[player],false)))) {SaveError();return false;}
        binds=std::move(values);notice={"Saved.","保存しました。"};return true;
    }
    void Commit(const std::string& value) {
        auto next=binds;
        for(int i=0;i<SaveStateBinding;++i)if(next[i]==value)next[i].clear();
        next[capture]=value;Save(std::move(next));Cancel();
    }
    std::set<std::string> Inputs(Device& d) {
        std::set<std::string> result;DIJOYSTATE2 state{};
        d.input->Poll();auto hr=d.input->GetDeviceState(sizeof(state),&state);
        if(hr==DIERR_INPUTLOST||hr==DIERR_NOTACQUIRED) {d.input->Acquire();d.input->Poll();hr=d.input->GetDeviceState(sizeof(state),&state);}
        d.connected=SUCCEEDED(hr);if(!d.connected)return result;
        for(int i=0;i<128;++i)if(state.rgbButtons[i]&0x80)result.insert("B"+std::to_string(i));
        for(int i=0;i<4;++i) {
            const auto value=state.rgdwPOV[i];if(LOWORD(value)==0xFFFF)continue;
            const int dirs[]{8,9,6,3,2,1,4,7};const int dir=dirs[(value%36000)/4500];
            // 斜めを複数の方向へ同時登録しない。十字の正方向だけを採用。
            if(dir==8||dir==2||dir==4||dir==6)result.insert("H"+std::to_string(i)+"_"+std::to_string(dir));
            else result.insert("diagonal");
        }
        const LONG axes[]{state.lX,-state.lY,state.lZ,state.lRx,-state.lRy,state.lRz,state.rglSlider[0],state.rglSlider[1]};
        for(int i=0;i<8;++i)if(axes[i]>16383||axes[i]<-16383)result.insert("A"+std::to_string(i)+(axes[i]>0?"+":"-"));
        return result;
    }
};
ControllerSettings::ControllerSettings():impl_(std::make_unique<Impl>()) {}
ControllerSettings::~ControllerSettings()=default;
void ControllerSettings::Command(const Json& c,bool locked) {
    auto& s=*impl_;const auto action=String(c,"action",16);
    if(action=="close") {s.opened=false;s.Cancel();s.Release();return;}
    if(action=="open") {s.opened=true;s.blocked=locked;if(!locked)s.Refresh();return;}
    if(!s.opened)return;
    if(action=="cancel") {s.Cancel();s.notice={};return;}
    if(locked) {s.Cancel();return;}
    if(action=="refresh") {s.Refresh();return;}
    if(action=="player") {s.Cancel();s.player=Integer(c,"player",0,1);s.Load();s.notice={};return;}
    if(action=="device") {
        s.Cancel();const auto token=String(c,"device",80);DeviceIdentity device;
        if(!token.empty()) {
            auto it=std::find_if(s.devices.begin(),s.devices.end(),[&](const auto& d){return Token(d.identity)==token;});
            if(it==s.devices.end())throw std::invalid_argument("unknown controller");
            device=it->identity;
        }
        if(s.Allocate(device)){s.Load();s.notice={"Saved.","保存しました。"};}return;
    }
    auto* device=s.Selected();if(!device||!device->connected)return;
    const int row=Integer(c,"row",0,SaveStateBinding-1);
    s.Cancel();s.notice={};
    if(action=="capture") {s.capture=row;return;}
    if(action=="clear") {auto values=s.binds;values[row].clear();s.Save(values);return;}
    throw std::invalid_argument("controller action");
}
void ControllerSettings::Poll(bool locked) {
    auto& s=*impl_;if(!s.opened)return;
    if(GetForegroundWindow()!=guiWindow)s.swallowed=0;
    if(locked) {s.Cancel();if(!s.blocked)s.Release();s.blocked=true;return;}
    if(s.blocked) {s.blocked=false;s.Refresh();}
    if(s.capture<0)return;
    // 別ウィンドウの操作は記録しない。戻った際もパッドの解放を待ってから再開する。
    if(GetForegroundWindow()!=guiWindow) {s.armed=false;s.pending.clear();return;}
    auto* device=s.Selected();if(!device){s.Cancel();return;}
    if(!device->input)return;
    auto held=s.Inputs(*device);
    if(diagnostics::input::Enabled())s.observed=held;
    if(!device->connected) {s.Cancel();s.notice={"Device disconnected. Reconnect and refresh.","機器が切断されました。接続し直して再検出してください。"};return;}
    // 片側トリガーの静止値が軸の負端になる機器でも、ボタンを登録できる。
    // 開始時点の入力は、一度解放されるまで候補から外す。
    if(!s.armed) {s.initialHeld=held;s.armed=true;return;}
    std::erase_if(s.initialHeld,[&](const auto& value){return !held.contains(value);});
    for(const auto& value:s.initialHeld)held.erase(value);
    if(!s.pending.empty()) {if(!held.contains(s.pending))s.Commit(s.pending);return;}
    if(held.size()==1&&!held.contains("diagonal"))s.pending=*held.begin();
}
bool ControllerSettings::KeyMessage(UINT message,WPARAM key,LPARAM flags) {
    auto& s=*impl_;
    if(message<WM_KEYFIRST||message>WM_KEYLAST)return false;
    if(s.swallowed) {
        if((message==WM_KEYUP||message==WM_SYSKEYUP)&&key==s.swallowed)s.swallowed=0;
        return true;
    }
    if(s.capture<0)return false;
    if(message==WM_KEYDOWN||message==WM_SYSKEYDOWN) {
        if(flags&(1<<30))return true;
        s.swallowed=key;
        if(key==VK_ESCAPE){s.Cancel();s.notice={};}
        else if(s.players[s.player].name=="Keyboard") {
            const auto name=KeyName(key,flags);
            if(!name.empty())s.Commit(name);
            else s.notice={"This key is reserved. Choose another key.","このキーは操作用に予約されています。別のキーを押してください。"};
        }
    }
    return true;
}
Json ControllerSettings::State(bool locked) const {
    const auto& s=*impl_;Json devices=Json::array();bool found=false;
    for(const auto& d:s.devices) {
        auto label=d.identity.name=="Keyboard"?std::string(Text("Keyboard","キーボード")):d.label;
        if(!d.identity.guid.empty())label+=" ("+d.identity.guid.substr(0,8)+")";
        devices.push_back({{"id",Token(d.identity)},{"label",label}});
        if(d.identity==s.players[s.player]&&d.connected)found=true;
    }
    Json bindings=Json::array();
    for(int i=0;i<SaveStateBinding;++i)bindings.push_back(domain::ui::ControllerBindingName(s.binds[i]));
    Json result={{"player",s.player},{"devices",devices},{"selected",Token(s.players[s.player])},
        {"assigned",!s.players[s.player].Empty()},{"connected",found},{"bindings",bindings},
        {"capture",s.capture},{"locked",locked},{"notice",s.notice.c_str()}};
    if(diagnostics::input::Enabled())result["diagnostic"]={{"foreground",GetForegroundWindow()==guiWindow},
        {"armed",s.armed},{"observed",s.observed},{"initialHeld",s.initialHeld},{"pending",s.pending}};
    return result;
}
}
