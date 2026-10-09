#include "core_dll/mbaa_mem/NativeHud.hpp"
#include "core_dll/mbaa_mem/RoundDelayImage.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/engine/SceneRunner.hpp"
#include "core_dll/ui/HudDisplay.hpp"
#include "core_dll/ui/State_Ui_Logic.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <MinHook.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>

namespace cccaster::game_interface::native_hud {
namespace {
using Runner = domain::session::SceneRunner;
using domain::ui::HudDisplay;
using domain::ui::StateUiLogic;
using LabelDraw = void (__thiscall*)(unsigned,const unsigned*);
LabelDraw originalLabel = nullptr;
using HudDraw = void (__cdecl*)();
HudDraw originalHud = nullptr;
IDirect3DTexture9* delayTexture=nullptr;
// 0x4163B0はハンドルを1回間接参照し、+8のD3D textureと+28/+2Cの寸法だけを読む。
// キューにはtexture本体が記録され、0x4C0220でAddRefされる。ゲーム資源リストへは登録しない。
std::array<uint32_t,12> textureView{};
uint32_t* textureHandle=textureView.data();
static_assert(sizeof(void*)==4);

// 0x415580は幅だけEDX、残りはcdecl。ROUNDと同じ画像キューへ登録する。
__attribute__((naked)) void __cdecl Sprite(unsigned,unsigned,int,int,int,int,int,int,int,unsigned,unsigned,int,int) {
    __asm__ __volatile__("movl 52(%esp),%edx; movl $0x415580,%eax; jmp *%eax");
}
bool IdentityVisible() {
    const auto mode=Runner::AppMode();
    return (mode==0 || mode==2 || mode==4) && HudDisplay::Visible() && !StateUiLogic::IsMappingWindowOpen();
}
__attribute__((force_align_arg_pointer)) void __fastcall DrawLabel(unsigned side,void*,const unsigned* wins) {
    if(!IdentityVisible() || *CC_GAME_MODE_ADDR!=CC_GAME_MODE_IN_GAME)originalLabel(side,wins);
    else *reinterpret_cast<unsigned char*>(0x56447f)=1;
}
__attribute__((force_align_arg_pointer)) void __cdecl DrawHud() {
    originalHud();
    const auto mode=Runner::AppMode();
    if(*CC_GAME_MODE_ADDR!=CC_GAME_MODE_IN_GAME || (mode!=0 && mode!=2) || !IdentityVisible() || !delayTexture)return;
    const bool ready=BattleHudReady();
    static const bool trace=std::getenv("CCCASTER_TEST_NATIVE_HUD")!=nullptr;
    static int previous=-1;
    if(trace && previous!=int(ready)) {
        domain::session::DebugLog("[NativeHud] ready=%u slide=%.3f hidden=%u intro=%u wt=%u",unsigned(ready),
            *reinterpret_cast<float*>(0x55def0),unsigned(*reinterpret_cast<unsigned char*>(0x5545f1)),
            unsigned(*CC_INTRO_STATE_ADDR),*CC_WORLD_TIMER_ADDR);
        previous=int(ready);
    }
    if(!ready)return;
    const auto gauge=*reinterpret_cast<unsigned*>(0x555108);
    if(!gauge)return;
    const int delay=(std::clamp)(mode==2 ? int(Runner::SpectatorStatus().delay) : StateUiLogic::GetDelay(),0,8);
    const auto scale=*reinterpret_cast<unsigned char*>(0x56447f),filter=*reinterpret_cast<unsigned char*>(0x564b02);
    *reinterpret_cast<unsigned char*>(0x56447f)=2;
    *reinterpret_cast<unsigned char*>(0x564b02)=1;
    // ROUNDと同じ画像方式を保ち、指定により縦横87.5%へ。中央と直下の位置は維持。
    Sprite(0,reinterpret_cast<unsigned>(&textureHandle),296,86,14,0,0,48,16,0xffffffff,0,995,42);
    Sprite(0,gauge,334,86,14,(delay%8)*16,(delay/8)*16,16,16,0xffffffff,0,995,14);
    *reinterpret_cast<unsigned char*>(0x56447f)=scale;
    *reinterpret_cast<unsigned char*>(0x564b02)=filter;
    static bool traced=false;
    if(!traced) {
        domain::session::DebugLog("[NativeHud] delay=image size=42x14 number=GAUGE04 14x14 pos=296,86");
        traced=true;
    }
}
}
bool BattleHudReady() {
    // 0x424DF0: HPバーの横移動、0x425B00: ROUNDの縦移動に同じ55DEF0を使う。
    // 0x472A20で1→0へクランプされる。5545F1は標準HUD全体の描画抑止。
    return game_build::RuntimeValidated() && *CC_GAME_MODE_ADDR==CC_GAME_MODE_IN_GAME &&
        *reinterpret_cast<float*>(0x55def0)==0.f && *reinterpret_cast<unsigned char*>(0x5545f1)==0;
}
void Prepare(IDirect3DDevice9* device) {
    Install();
    if(delayTexture || !device || !game_build::RuntimeValidated())return;
    // managed textureは標準Resetで内容を保持し、HUD全体のReleaseで解放する。
    if(FAILED(device->CreateTexture(64,16,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&delayTexture,nullptr)))return;
    D3DLOCKED_RECT lock{};
    if(FAILED(delayTexture->LockRect(0,&lock,nullptr,0))) {Release();return;}
    const auto pixels=RoundDelayPixels();
    for(unsigned y=0;y<16;++y) {
        auto* row=static_cast<unsigned char*>(lock.pBits)+y*lock.Pitch;
        std::memset(row,0,64*4);
        std::memcpy(row,pixels.data()+y*48,48*4);
    }
    delayTexture->UnlockRect(0);
    textureView[2]=reinterpret_cast<uint32_t>(delayTexture);
    textureView[10]=64;textureView[11]=16;
}
void Release() {
    if(delayTexture)delayTexture->Release();
    delayTexture=nullptr;textureView={};
}
void Install() {
    static bool attempted=false;
    if(attempted || !game_build::RuntimeValidated())return;
    attempted=true;
    const unsigned char label[]{0x81,0xec,0x04,0x01,0,0,0xa1,0x58,0xb4,0x54,0};
    const unsigned char hud[]{0x6a,0xff,0x68,0x90,0x6d,0x51,0};
    if(std::memcmp(reinterpret_cast<void*>(0x426ac0),label,sizeof(label)) ||
       std::memcmp(reinterpret_cast<void*>(0x432d30),hud,sizeof(hud)))return;
    const auto init=MH_Initialize();
    if(init!=MH_OK && init!=MH_ERROR_ALREADY_INITIALIZED)return;
    auto* labelAt=reinterpret_cast<void*>(0x426ac0);
    auto* hudAt=reinterpret_cast<void*>(0x432d30);
    if(MH_CreateHook(labelAt,reinterpret_cast<void*>(DrawLabel),reinterpret_cast<void**>(&originalLabel))!=MH_OK)return;
    if(MH_CreateHook(hudAt,reinterpret_cast<void*>(DrawHud),reinterpret_cast<void**>(&originalHud))!=MH_OK) {
        MH_RemoveHook(labelAt);originalLabel=nullptr;return;
    }
    // 標準文字だけ消えて追加HUDが出ない部分適用を避ける。
    if(MH_QueueEnableHook(labelAt)!=MH_OK || MH_QueueEnableHook(hudAt)!=MH_OK || MH_ApplyQueued()!=MH_OK) {
        MH_DisableHook(labelAt);MH_DisableHook(hudAt);MH_RemoveHook(labelAt);MH_RemoveHook(hudAt);
        originalLabel=nullptr;originalHud=nullptr;return;
    }
    domain::session::DebugLog("[NativeHud] installed label=00426AC0 hud=00432D30");
}
}
