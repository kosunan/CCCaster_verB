#include "core_dll/mbaa_mem/ExtraColorSelection.hpp"
#include "core_dll/engine/ExtraColorStore.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/engine/ExtraColorPage.hpp"

extern "C" int __cdecl cc_native_color_original(unsigned,unsigned,unsigned);
extern "C" void __cdecl cc_native_color_refresh(unsigned);

namespace cccaster::training_palette::selection {
namespace {
unsigned mode=99;bool host=true,inSelect=false;
struct Side {
    unsigned character=UINT32_MAX;
    int choice=-1,menuColor=-1;
    std::array<std::shared_ptr<const ExtraColor>,6> colors;
};
std::array<Side,2> sides;
uint32_t publishedEpoch=0,serial=1,publishedCharacter=UINT32_MAX,publishedRevision=0;
int publishedChoice=-2;
std::string notice;
uint32_t* Cursor(unsigned side){return side ? CC_P2_SELECTOR_MODE_ADDR : CC_P1_SELECTOR_MODE_ADDR;}
bool Owned(unsigned side){return side<2 && (mode==1 || mode==5 || (mode==0 && side==unsigned(!host)));}
}
void Configure(unsigned value,bool isHost){mode=value;host=isHost;}
const char* Notice(){return notice.c_str();}
void Tick() {
    if(mode!=0 && mode!=1 && mode!=5)return;
    if(*CC_GAME_MODE_ADDR!=CC_GAME_MODE_CHARA_SELECT){inSelect=false;return;}
    if(!inSelect){sides={};notice.clear();publishedChoice=-2;inSelect=true;}
    for(unsigned side=0;side<2;++side) {
        if(!Owned(side))continue;
        auto& state=sides[side];auto* cursor=Cursor(side);
        if(state.character!=cursor[4]) {
            state={};state.character=cursor[4];
            if(state.character<=100)for(unsigned i=0;i<6;++i) {
                ExtraColor color;std::string error;
                if(LoadExtra(state.character,i,color,error))
                    state.colors[i]=std::make_shared<const ExtraColor>(std::move(color));
            }
        }
        if(cursor[0]<CC_SELECT_COLOR){state.choice=-1;state.menuColor=-1;}
        if(state.choice>=0 && cursor[0]<4) {
            cursor[6]=state.colors[state.choice]->baseColor;
        }
    }
}
bool Editable(unsigned side) {
    return Owned(side) && *CC_GAME_MODE_ADDR==CC_GAME_MODE_CHARA_SELECT && Cursor(side)[0]<4 &&
        Cursor(side)[0]>=CC_SELECT_COLOR && (mode!=0 || network::Store::Supported());
}
bool Visible(unsigned side){return Owned(side) && *CC_GAME_MODE_ADDR==CC_GAME_MODE_CHARA_SELECT;}
unsigned Character(unsigned side){return side<2 ? sides[side].character : UINT32_MAX;}
int Choice(unsigned side){return side<2 ? sides[side].choice : -1;}
bool Available(unsigned side,unsigned extra){return side<2 && extra<6 && bool(sides[side].colors[extra]);}
const ExtraColor* Saved(unsigned side,unsigned extra){return Available(side,extra) ? sides[side].colors[extra].get() : nullptr;}
unsigned MenuColor(unsigned side){return sides[side].menuColor>=36 ? unsigned(sides[side].menuColor) : (std::min)(35u,Cursor(side)[6]);}
bool NativePage(unsigned side){return Owned(side) && (mode!=0 || network::Store::Supported());}
const uint32_t* PreviewPalette(uint32_t* cg,const uint32_t* original) {
    if(*CC_GAME_MODE_ADDR!=CC_GAME_MODE_CHARA_SELECT)return original;
    auto* base=*reinterpret_cast<uint8_t**>(0x74D808);if(!base)return original;
    for(unsigned side=0;side<2;++side) {
        if(!NativePage(side) || Choice(side)<0)continue;
        const auto* saved=Saved(side,unsigned(Choice(side)));if(!saved)continue;
        for(const auto& part:saved->parts) {
            const auto* triple=*reinterpret_cast<uint32_t**>(base+side*0x1dc+0x1b0+part.component*12);
            if(triple && triple[1]==reinterpret_cast<uintptr_t>(cg))return part.edit.palette.data();
        }
    }
    return original;
}
bool Choose(unsigned side,int extra) {
    if(!Editable(side) || extra< -1 || extra>=6 || (extra>=0 && !Available(side,extra)))return false;
    sides[side].choice=extra;notice.clear();Tick();
    domain::session::DebugLog("[ExtraColor] CHOOSE side=%u character=%u extra=%d",side,sides[side].character,extra+1);
    return true;
}
void Filter(game_interface::GameInput& p1,game_interface::GameInput& p2) {
    Tick();
    // カラー操作は元ゲームのリピート・確定・取消を受け取るフックへ集約。
}
int NativeInput(unsigned side,unsigned direction,unsigned buttons) {
    if(!NativePage(side))return cc_native_color_original(side,direction,buttons);
    Tick();auto& state=sides[side];auto* cursor=Cursor(side);
    auto* base=*reinterpret_cast<uint8_t**>(0x74D808);
    if(!base)return cc_native_color_original(side,direction,buttons);
    auto* menu=base+side*0x1dc+0x2c;
    auto& random=*reinterpret_cast<uint32_t*>(menu+12);
    unsigned navigation=direction;
    if(side && (navigation==4 || navigation==6))navigation=10-navigation;
    auto color=MenuColor(side);
    bool changed=false;
    if(!random && (navigation==4 || navigation==6)) {
        color=ColorPageMove(color,navigation==6);changed=true;
    } else if(state.menuColor>=36) {
        if(random && (navigation==2 || navigation==8)) {
            color=36+(navigation==2 ? 0 : 5);random=0;changed=true;
        } else if(!random && (navigation==2 || navigation==8)) {
            if((navigation==2 && color%6==5) || (navigation==8 && color%6==0)) {
                random=1;*reinterpret_cast<unsigned*>(menu+8)=0;
            } else {color+=navigation==2 ? 1 : -1;changed=true;}
        }
    } else return cc_native_color_original(side,direction,buttons);
    if(changed) {
        state.menuColor=color>=36 ? int(color) : -1;
        state.choice=color>=36 && Available(side,color-36) ? int(color-36) : -1;
        if(state.choice>=0)cursor[6]=state.colors[state.choice]->baseColor;
        else if(color<36)cursor[6]=color;
        cc_native_color_refresh(side);
        *reinterpret_cast<uint8_t*>(0x76e0a0)=1;
        domain::session::DebugLog("[ExtraColor] PAGE side=%u color=%u saved=%u",side,color+1,unsigned(color<36 || state.choice>=0));
    }
    if(state.menuColor>=36 && !random) {
        if(state.choice<0)buttons&=~1u;
        else if(buttons&1)domain::session::DebugLog("[ExtraColor] CHOOSE side=%u character=%u extra=%d",side,state.character,state.choice+1);
    }
    if((buttons&2) || (random && (buttons&1))){state.choice=-1;state.menuColor=-1;}
    return cc_native_color_original(side,0,buttons);
}
void Publish(uint32_t epoch,uint32_t revision) {
    if(mode!=0)return;
    Tick();const auto& state=sides[unsigned(!host)];
    if(!epoch || state.character>100)return;
    if(publishedEpoch==epoch && publishedCharacter==state.character && publishedChoice==state.choice && publishedRevision==revision)return;
    const auto* color=state.choice>=0 ? state.colors[state.choice].get() : nullptr;
    const auto blob=network::Make(epoch,++serial,state.character,state.choice<0 ? UINT32_MAX : unsigned(state.choice),color,revision);
    if(!blob){notice="Cannot transfer this extra color.";return;}
    network::Store::Local(blob);publishedEpoch=epoch;publishedCharacter=state.character;publishedChoice=state.choice;publishedRevision=revision;
    domain::session::DebugLog("[ExtraColor] SEND epoch=%u serial=%u character=%u extra=%d bytes=%u hash=%08x",
        epoch,serial,state.character,state.choice+1,unsigned(blob->bytes.size()),blob->hash);
}
bool Ready(uint32_t epoch,uint32_t localCharacter,uint32_t peerCharacter,uint32_t localRevision,uint32_t peerRevision) {
    const bool ready=network::Store::Ready(epoch,localCharacter,peerCharacter,localRevision,peerRevision);
    notice=ready ? "" : "SYNCING EXTRA COLORS...";return ready;
}
std::shared_ptr<const ExtraColor> Resolve(unsigned side,unsigned character) {
    if(side>=2)return {};
    if(mode==0) {
        const auto blob=side==unsigned(!host) ? network::Store::Local() : network::Store::Peer();
        if(!blob || blob->Character()!=character || blob->Extra()>=6)return {};
        ExtraColor value;
        if(DecodeExtra(std::span(blob->bytes).subspan(12),value))return std::make_shared<const ExtraColor>(std::move(value));
        return {};
    }
    const auto& state=sides[side];
    return state.character==character && state.choice>=0 ? state.colors[state.choice] : nullptr;
}
}
