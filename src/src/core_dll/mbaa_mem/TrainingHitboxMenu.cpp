#include "core_dll/mbaa_mem/TrainingHitboxMenu.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/Platform.hpp"
#include "core_dll/hook/BorderlessDisplay.hpp"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdlib>

// 診断時だけ、純粋な座標変換0x419630の結果と比較する。書込先は自分のRect。
extern "C" __attribute__((naked)) void cc_hitbox_native_rect(void*,void*) {
    __asm__ __volatile__("movl 4(%esp),%ecx; movl 8(%esp),%eax; movl $0x419630,%edx; jmp *%edx");
}
namespace cccaster::training_hitbox {
namespace {
Options options;
Frame frame;
std::atomic<bool> open{false};
bool installed=false,suppress=false;
uint32_t previousButtons=0;
uint16_t previousDirection=0;
int64_t repeatAt=0;
bool Trace(){static const bool value=std::getenv("CCCASTER_HITBOX_TRACE")!=nullptr;return value;}
template<class T> T Read(uintptr_t address){T value;std::memcpy(&value,reinterpret_cast<const void*>(address),sizeof(T));return value;}
// 1描画内だけの範囲キャッシュ。キャラ切替・解放・FN復元を越えてポインタを保持しない。
struct Region{uintptr_t begin,end;};
std::vector<Region> readable;
bool Readable(uintptr_t address,size_t bytes) {
    if(address<0x10000 || address>UINT32_MAX-bytes)return false;
    for(const auto& r:readable)if(address>=r.begin && address+bytes<=r.end)return true;
    MEMORY_BASIC_INFORMATION m{};
    if(!VirtualQuery(reinterpret_cast<void*>(address),&m,sizeof(m)) || m.State!=MEM_COMMIT ||
       (m.Protect&(PAGE_NOACCESS|PAGE_GUARD)) || !(m.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
    Region r{uintptr_t(m.BaseAddress),uintptr_t(m.BaseAddress)+m.RegionSize};
    if(address+bytes>r.end)return false;
    readable.push_back(r);return true;
}
void Close(){open=false;suppress=true;escapeRequested=false;domain::session::DebugLog("[Hitbox] CLOSE mask=%u",options.Mask());}
void Actor(uintptr_t actor,unsigned index,std::array<unsigned,Count>& found,unsigned& checked,unsigned& mismatch) {
    const auto anim=Read<uint32_t>(actor+0x31c);
    if(!Readable(anim,0x54))return;
    Pose pose{Read<int32_t>(actor+0x104),Read<int32_t>(actor+0x108),Read<int32_t>(actor+0x154),Read<int32_t>(actor+0x158),
        Read<int32_t>(0x55dec4),Read<int32_t>(0x55dec8),Read<uint8_t>(actor+0x310)!=0,
        Read<uint8_t>(actor+0x172)!=0,Read<uint16_t>(anim)==0};
    const auto attack=Read<uint32_t>(anim+0x3c);
    const bool throwing=Readable(attack,0x32) && Read<uint8_t>(attack+0x31)!=0;
    for(unsigned pool=0;pool<2;++pool) {
        const unsigned count=Read<uint8_t>(anim+0x42+pool);
        const auto table=Read<uint32_t>(anim+0x4c+pool*4);
        if(!count || !Readable(table,count*4))continue;
        for(unsigned slot=0;slot<count;++slot) {
            const auto kind=pool ? (throwing?Kind::Throw:Kind::Attack) : DefenseKind(slot);
            if(kind==Kind::Count)continue;
            const auto ptr=Read<uint32_t>(table+slot*4);
            if(!Readable(ptr,8))continue;
            ++found[unsigned(kind)];
            if(!options.visible[unsigned(kind)])continue;
            const Rect raw{Read<int16_t>(ptr),Read<int16_t>(ptr+2),Read<int16_t>(ptr+4),Read<int16_t>(ptr+6)};
            const auto rect=Transform(raw,pose);
            if(Trace()) {
                auto native=raw;cc_hitbox_native_rect(reinterpret_cast<void*>(actor),&native);
                ++checked;if(native!=rect)++mismatch;
            }
            frame.boxes.push_back({rect,kind,index});
        }
    }
}
}
bool Install() {
    if(installed)return true;
    const uint8_t signature[]{0x80,0xb9,0x10,0x03,0,0,0,0x8b,0x50,0x08};
    installed=game_build::RuntimeValidated() && !std::memcmp(reinterpret_cast<void*>(0x419630),signature,sizeof(signature));
    return installed;
}
bool Active(){return open.load(std::memory_order_relaxed);}
const Options& Current(){return options;}
void ResetOptions(){options={};domain::session::DebugLog("[Hitbox] DEFAULT mask=0");}
void Open() {
    if(!installed)return;
    open=true;suppress=true;escapeRequested=false;
    previousButtons=0;previousDirection=0;
    domain::session::DebugLog("[Hitbox] OPEN mask=%u",options.Mask());
}
void Step(game_interface::GameInput& p1,game_interface::GameInput& p2,bool configuring) {
    const auto buttons=p1.buttons|p2.buttons;
    const auto direction=p1.direction?p1.direction:p2.direction;
    const bool wasOpen=Active();
    if(*CC_GAME_MODE_ADDR!=CC_GAME_MODE_IN_GAME){open=false;suppress=false;previousButtons=previousDirection=0;escapeRequested=false;return;}
    if(wasOpen && (configuring || !Read<uint32_t>(0x74d7fc)))Close();
    if(Active() && !suppress) {
        const auto edge=buttons&~previousButtons;
        const auto mask=options.Mask();
        if(escapeRequested.exchange(false) || (edge&(CC_BUTTON_B|CC_BUTTON_CANCEL|CC_BUTTON_START)))Close();
        else if(edge&(CC_BUTTON_A|CC_BUTTON_CONFIRM))options.Toggle();
        else if(direction && (direction!=previousDirection || platform::RealMonotonicUs()>=repeatAt)) {
            if(direction==8)options.Move(-1);
            if(direction==2)options.Move(1);
            if(direction==4)options.Set(false);
            if(direction==6)options.Set(true);
            repeatAt=platform::RealMonotonicUs()+(direction==previousDirection?90000:350000);
        }
        if(mask!=options.Mask())domain::session::DebugLog("[Hitbox] OPTION kind=%s enabled=%u mask=%u",Names[options.selected],unsigned(options.visible[options.selected]),options.Mask());
    }
    previousButtons=buttons;previousDirection=direction;
    if(Active() || suppress || wasOpen){p1=p2={};if(!buttons && !direction)suppress=false;}
}
const Frame& ReadFrame() {
    frame.boxes.clear();readable.clear();
    if(!installed || *CC_GAME_MODE_ADDR!=CC_GAME_MODE_IN_GAME || !options.Any())return frame;
    // 0x432E30はviewportを狭めず、黒帯を含むバックバッファへ画像を合成する。
    const auto engine=Read<uint32_t>(0x767448),settings=Read<uint32_t>(0x554140);
    if(!Readable(engine,0x30) || !Readable(settings,0x17c) || Read<uint8_t>(0x76e652))return frame;
    const int width=Read<int>(engine+0x28),height=Read<int>(engine+0x2c);
    const auto aspect=Read<unsigned>(settings+0x178);
    int ratioWidth=Read<int>(0x54d048),ratioHeight=Read<int>(0x54d04c);
    if(aspect==1 && Read<int>(0x54d040)==1) {
        RECT client{};
        const auto window=Read<HWND>(0x74dfac);
        if(!game_interface::borderless::RenderingClientRect(window,client) && !GetClientRect(window,&client))return frame;
        ratioWidth=client.right-client.left;ratioHeight=client.bottom-client.top;
    }
    frame.image=CompositeRect(width,height,aspect,ratioWidth,ratioHeight);
    if(frame.image.x1<=frame.image.x0 || frame.image.y1<=frame.image.y0)return frame;
    frame.zoom=Read<float>(0x54eb70);
    if(!std::isfinite(frame.zoom) || frame.zoom<=0 || frame.zoom>16)return frame;
    std::array<unsigned,Count> found{};unsigned checked=0,mismatch=0,afterimages=0;
    for(unsigned i=0;i<4;++i)if(Read<uint8_t>(0x555130+i*0xafc))Actor(0x555134+i*0xafc,i,found,checked,mismatch);
    for(unsigned i=0;i<1000;++i)if(Read<uint8_t>(0x67bde8+i*0x33c)) {
        const uintptr_t actor=0x67bdec+i*0x33c;
        // 0x455210は元のモーションを持つ残像をActor+5=0x1Fで生成する。
        // 0x453F70/0x46F100/0x46F2F0と同じく、矩形収集前に除外する。
        if(Read<uint8_t>(actor+5)==0x1f){++afterimages;continue;}
        Actor(actor,i+4,found,checked,mismatch);
    }
    if(Trace()) {
        static uint32_t lastFrame=UINT32_MAX,lastMask=UINT32_MAX;
        const auto now=Read<uint32_t>(0x55d1cc);
        if(now!=lastFrame || lastMask!=options.Mask()) {
            lastFrame=now;lastMask=options.Mask();
            unsigned effectBoxes=0,afterimageBoxes=0;
            for(const auto& box:frame.boxes)if(box.actor>=4) {
                ++effectBoxes;
                if(Read<uint8_t>(0x67bdec+(box.actor-4)*0x33c+5)==0x1f)++afterimageBoxes;
            }
            domain::session::DebugLog("[Hitbox] FRAME f=%u mask=%u found=%u,%u,%u,%u,%u,%u drawn=%u checked=%u mismatch=%u zoom=%.5f afterimages=%u effect_boxes=%u afterimage_boxes=%u surface=%dx%d aspect=%u image=%d,%d,%d,%d",
                now,lastMask,found[0],found[1],found[2],found[3],found[4],found[5],unsigned(frame.boxes.size()),checked,mismatch,frame.zoom,afterimages,effectBoxes,afterimageBoxes,
                width,height,aspect,frame.image.x0,frame.image.y0,frame.image.x1,frame.image.y1);
        }
    }
    return frame;
}
}
