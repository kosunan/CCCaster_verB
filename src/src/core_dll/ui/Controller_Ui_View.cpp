#include "core_dll/ui/Controller_Ui_View.hpp"
#include "core_dll/ui/Controller_Ui_Logic.hpp"
#include "core_dll/ui/ControllerMappingValidation.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/ui/HudTheme.hpp"
#include <algorithm>
#include <vector>

namespace cccaster::domain::ui {
namespace {
using L=ControllerUiLogic;
using Hook=cccaster::game_interface::DirectInputHook;
using namespace cccaster::hud;
constexpr const char* Labels[]{"Up","Down","Left","Right","A (confirm)","B (cancel)","C","D","E","Start","FN1","FN2","A+B"};
struct Column { std::vector<std::string> lines; int selection=-1; };
std::string DeviceName(int p) {
    auto name=L::Device(p).name;
    const auto devices=Hook::GetConnectedDevices();
    if(std::count_if(devices.begin(),devices.end(),[&](const auto& d){return name==d.name;})>1)
        name+=" ("+std::to_string(L::DeviceId(p)+1)+")";
    if(L::DeviceId(p)==-1) name+=" (disconnected)";
    return name;
}
Column PlayerColumn(int p,int headerRows) {
    Column column;
    if(L::Device(p).Empty()) {
        column.lines.push_back(p ? "Press Right on P2 controller" : "Press Left on P1 controller");
        return column;
    }
    const bool keyboard=L::Device(p).name=="Keyboard";
    const int row=L::SelectedRow(p);
    if(row==L::OverviewRow) {
        column.lines.push_back("Press Up or Down to set keys");
        column.lines.resize(headerRows);
        column.lines.push_back(DeviceName(p));
        column.selection=headerRows;
        return column;
    }
    if(keyboard) column.lines.push_back("Press Enter to set a direction key");
    column.lines.push_back(p ? "Press Right to delete a key" : "Press Left to delete a key");
    column.lines.resize(headerRows);
    column.lines.push_back(DeviceName(p));
    const int first=keyboard ? 0 : 4;
    for(int i=first;i<L::DoneRow;++i) {
        auto input=L::Binds(p)[i].empty() ? std::string{} : ControllerBindingName(L::Binds(p)[i]);
        if(i<4 && L::CaptureBinding(p)==i) input="...";
        column.lines.push_back(std::string(Labels[i])+" : "+input);
    }
    column.lines.push_back(keyboard ? "Done (press Enter)" : "Done (press any button)");
    column.selection=headerRows+1+row-first;
    return column;
}
}
void ControllerUiView::Draw() {
    L::BeginUiSession(); L::Update();
    const auto devices=Hook::GetConnectedDevices();
    // 旧版と同じ上端の三列。機器一覧の下から左右の割当を並べる。
    const int headerRows=3+int(devices.size())+1;
    const auto left=PlayerColumn(0,headerRows), right=PlayerColumn(1,headerRows);
    std::vector<std::string> center{"Controllers",""};
    if(L::DeviceId(0)!=-2 && L::DeviceId(1)!=-2) center.push_back("Keyboard");
    for(const auto& device:devices) {
        if(device.id==L::DeviceId(0) || device.id==L::DeviceId(1)) continue;
        auto name=std::string(device.name);
        if(std::count_if(devices.begin(),devices.end(),[&](const auto& d){return name==d.name;})>1)
            name+=" ("+std::to_string(device.id+1)+")";
        center.push_back(name);
    }
    const int rows=int((std::max)({left.lines.size(),right.lines.size(),center.size()}));
    const int errorRows=L::StatusError() ? 2 : 0;
    const float line=(std::min)(14.f,460.f/(rows+errorRows));
    const float size=15.f*(line/14.f);
    Canvas c{CurrentLayout(),ImGui::GetForegroundDrawList()};
    c.Fill({0,0,640,20+(rows+errorRows)*line},IM_COL32(0,0,0,220));
    const auto drawColumn=[&](const Column& column,int side) {
        const bool alignRight=side==1;
        for(int i=0;i<int(column.lines.size());++i) {
            const auto text=c.Fit(column.lines[i].c_str(),i<headerRows ? 240.f : 300.f,size,0);
            const float width=Font(0,c.layout.scale)->CalcTextSizeA(c.S(size),10000,0,text.c_str()).x/c.layout.scale;
            const float x=alignRight ? 630-width : 10;
            if(i==column.selection)
                c.Fill({x-5,10+i*line,width+10,line},alignRight ? IM_COL32(30,30,255,255) : IM_COL32(210,0,0,255));
            c.Text(x,10+i*line,text.c_str(),size,IM_COL32(255,255,255,255),0);
        }
    };
    drawColumn(left,0); drawColumn(right,1);
    for(int i=0;i<int(center.size());++i) {
        const auto text=c.Fit(center[i].c_str(),150,size,0);
        const float width=Font(0,c.layout.scale)->CalcTextSizeA(c.S(size),10000,0,text.c_str()).x/c.layout.scale;
        c.Text(320-width/2,10+i*line,text.c_str(),size,IM_COL32(255,255,255,255),0);
    }
    if(L::StatusError())
        c.Text(10,10+(rows+1)*line,L::Status().c_str(),size,Gold,0,620);
    if(L::TakeCloseRequest() && OnClose()) StateUiLogic::CloseMappingWindow();
}
bool ControllerUiView::OnClose() {
    if(!L::EndUiSession()) return false;
    StateUiLogic::NotifyControllerSettingsClosed();
    return true;
}
} // namespace cccaster::domain::ui
