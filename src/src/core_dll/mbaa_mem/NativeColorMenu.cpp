#include "core_dll/hook/HookBatch.hpp"
#include "core_dll/mbaa_mem/ExtraColorSelection.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/mbaa_mem/TrainingPaletteMenu.hpp"
#include "core_dll/engine/ExtraColorPage.hpp"
#include "core_dll/engine/ExtraColorSample.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <MinHook.h>
#include <d3d9.h>
#include <cmath>
#include <cstring>

extern "C" {
void* cc_color_input_original=nullptr;
int __cdecl cc_native_color_input(unsigned,unsigned,unsigned);
// 元の色入力はESI=側、stack=方向/ボタン、ret 8。ゲームのリピート後の入力を受ける。
__attribute__((naked)) void cc_color_input_hook() {
    __asm__ __volatile__("pushl 8(%esp); pushl 8(%esp); pushl %esi; call _cc_native_color_input; addl $12,%esp; ret $8");
}
__attribute__((naked)) int __cdecl cc_native_color_original(unsigned,unsigned,unsigned) {
    __asm__ __volatile__("pushl %esi; movl 8(%esp),%esi; pushl 16(%esp); pushl 16(%esp); call *_cc_color_input_original; popl %esi; ret");
}
__attribute__((naked)) void __cdecl cc_native_color_refresh(unsigned) {
    __asm__ __volatile__("movl 4(%esp),%eax; movl $0x48c830,%edx; jmp *%edx");
}
__attribute__((naked)) void __cdecl cc_native_color_recolor(unsigned) {
    __asm__ __volatile__("movl 4(%esp),%eax; movl $0x4899f0,%edx; jmp *%edx");
}
// 元の描画キューへ登録。最後の引数だけEDXへ移し、それ以外は元のcdeclスタック。
__attribute__((naked)) void __cdecl cc_color_sprite(unsigned,unsigned,int,int,int,int,int,int,int,unsigned,unsigned,int,int) {
    __asm__ __volatile__("movl 52(%esp),%edx; movl $0x415580,%eax; jmp *%eax");
}
}

namespace cccaster::training_palette::selection {
namespace {
using DrawList=void (__stdcall*)(unsigned,int,unsigned);
DrawList originalDraw=nullptr;
using UpdateList=int (__cdecl*)(int);
UpdateList originalUpdate=nullptr;
std::array<int,2> previewChoices{-1,-1};
struct PageState {
    uintptr_t owner=0,texture=0;
    unsigned character=UINT32_MAX,page=UINT32_MAX;
    std::array<unsigned,MenuColors> previous{},ranks{};
    float blend=0;
    bool thumbnails=false;
};
std::array<PageState,2> pages;

void Sprite(unsigned texture,int x,int y,int width,int height,int sx,int sy,int sw,int sh,unsigned tint,int depth) {
    cc_color_sprite(0,texture,x,y,height,sx,sy,sw,sh,tint,0,depth,width);
}
void ExtraLabel(unsigned extra,int x,int y,unsigned tint,int depth) {
    // 元の英数字フォントの字形を同じ描画キューへ載せ、ページの移動・陰影に追従させる。
    const auto* font=reinterpret_cast<const unsigned*>(0x55D680);
    const unsigned width=font[0x408/4],height=font[0x40c/4],columns=font[0x410/4],texture=font[0x41c/4];
    if(!texture || !width || !height || !columns)return;
    const char label[]{'E','X','T','R','A',' ',char('1'+extra),0};
    auto& mode=*reinterpret_cast<uint8_t*>(0x56447f);const auto previousMode=mode;mode=2;
    // 同じ深度は後から登録したものが先に描かれる。白文字、縁、行背景の順に登録する。
    for(unsigned pass=0;pass<5;++pass)for(unsigned i=0;i<7;++i) {
        if(label[i]==' ')continue;
        const unsigned glyph=unsigned(label[i]-' ');
        const int dx=pass==1 ? -1 : pass==2 ? 1 : 0,dy=pass==3 ? -1 : pass==4 ? 1 : 0;
        Sprite(texture,x+int(i)*8+dx,y+dy,9,14,int(glyph%columns*width),int(glyph/columns*height),
            int(width),int(height),pass ? 0xff000000 : tint,depth);
    }
    mode=previousMode;
}
int X(unsigned side,unsigned rank) {
    return side ? ((29-int(rank/6))*4-int(rank%6))*4 : (int(rank%6)-32+int(rank/6)*4)*4;
}
int Y(unsigned rank) {return 290+int(rank%6)*16+int(rank/6)*3;}
int Mix(int oldValue,int value,float blend) {return int(std::lround(oldValue*blend+value*(1-blend)));}

// 元の36セルは保持し、同じテクスチャの未使用セル36〜41へ保存色を生成する。
// 標準色と同じUV・フィルター・40×14への拡大を使い、矩形分割の丸め差をなくす。
bool Thumbnails(unsigned side,unsigned wrapper) {
    if(!wrapper)return false;
    const auto object=*reinterpret_cast<uintptr_t*>(wrapper);
    if(!object)return false;
    auto* texture=*reinterpret_cast<IDirect3DTexture9**>(object+8);
    if(!texture)return false;
    D3DSURFACE_DESC desc{};
    if(FAILED(texture->GetLevelDesc(0,&desc)))return false;
    if(desc.Width<256 || desc.Height<96 || *reinterpret_cast<unsigned*>(object+0x28)!=desc.Width ||
       *reinterpret_cast<unsigned*>(object+0x2c)!=desc.Height)return false;
    const bool shortColor=desc.Format==D3DFMT_A1R5G5B5 || desc.Format==D3DFMT_A4R4G4B4;
    if(!shortColor && desc.Format!=D3DFMT_A8R8G8B8 && desc.Format!=D3DFMT_X8R8G8B8)return false;
    const auto* base=*reinterpret_cast<uint8_t**>(0x74D808);
    const auto* nativePalette=*reinterpret_cast<const uint32_t* const*>(base+side*0x1dc+0x1ac);
    if(!nativePalette || *nativePalette<36 || *nativePalette>256)return false;
    std::array<Palette,36> palettes;
    std::memcpy(palettes.data(),nativePalette+1,sizeof(palettes));
    const auto* triple=*reinterpret_cast<const uint32_t* const*>(base+side*0x1dc+0x1b0);
    const auto samples=SampleBody(triple ? reinterpret_cast<uint32_t*>(triple[1]) : nullptr);
    std::array<unsigned,256> usage{};
    for(unsigned i=0;i<256;++i)usage[i]=unsigned(samples[i].size());
    D3DLOCKED_RECT lock{};
    if(FAILED(texture->LockRect(0,&lock,nullptr,0)))return false;
    auto pack=[&](uint32_t color) {
        if(!(color&0xffffff))color|=0x010101;
        return shortColor ? unsigned(Pack16(color|0xff000000,desc.Format==D3DFMT_A4R4G4B4)) :
            0xff000000u|((color&255)<<16)|(color&0xff00)|((color>>16)&255);
    };
    auto unpack=[&](const uint8_t* pixel) {
        if(!shortColor) {
            const auto value=*reinterpret_cast<const uint32_t*>(pixel);
            return 0xff000000u|((value&255)<<16)|(value&0xff00)|((value>>16)&255);
        }
        const auto value=*reinterpret_cast<const uint16_t*>(pixel);
        if(desc.Format==D3DFMT_A4R4G4B4)
            return 0xff000000u|(value>>8&15)*17|((value>>4&15)*17<<8)|((value&15)*17<<16);
        return 0xff000000u|(value>>10&31)*255/31|((value>>5&31)*255/31<<8)|((value&31)*255/31<<16);
    };
    std::array<uint32_t,36> standardSwatches{};
    for(unsigned color=0;color<36;++color)
        standardSwatches[color]=unpack(static_cast<uint8_t*>(lock.pBits)+(color/8*16+8)*lock.Pitch+(color%8*32+16)*(shortColor ? 2 : 4));
    const unsigned representative=RepresentativeIndex(palettes,standardSwatches,usage);
    for(unsigned extra=0;extra<6;++extra) {
        const auto* saved=Saved(side,extra);const Edit* edit=nullptr;
        if(saved)for(const auto& part:saved->parts)if(part.component==0)edit=&part.edit;
        if(!edit)continue;
        const unsigned color=36+extra;
        const auto rgb=RepresentativeColor(*edit,representative,samples[representative]),value=pack(rgb);
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<32;++x) {
            auto* dest=static_cast<uint8_t*>(lock.pBits)+(color/8*16+y)*lock.Pitch+(color%8*32+x)*(shortColor ? 2 : 4);
            if(shortColor)*reinterpret_cast<uint16_t*>(dest)=uint16_t(value);
            else *reinterpret_cast<uint32_t*>(dest)=value;
        }
        domain::session::DebugLog("[ExtraColor] SWATCH side=%u extra=%u index=%u pixels=%u rgb=%06x",side,extra+1,
            representative,unsigned(samples[representative].size()),((rgb&255)<<16)|(rgb&0xff00)|((rgb>>16)&255));
    }
    texture->UnlockRect(0);return true;
}

__attribute__((force_align_arg_pointer)) int __cdecl Update(int active) {
    const int result=originalUpdate(active);
    auto* base=*reinterpret_cast<uint8_t**>(0x74D808);
    if(!base){previewChoices={-1,-1};return result;}
    for(unsigned side=0;side<2;++side) {
        if(!NativePage(side) || !*reinterpret_cast<unsigned*>(base+side*0x1dc+0x10))continue;
        const int choice=Choice(side);
        if(previewChoices[side]!=choice) {
            cc_native_color_recolor(side);
            reinterpret_cast<void (__cdecl*)()>(0x4063D0)();
            previewChoices[side]=choice;
            domain::session::DebugLog("[ExtraColor] PREVIEW side=%u extra=%d",side,choice+1);
        }
    }
    return result;
}

__attribute__((force_align_arg_pointer)) void __stdcall Draw(unsigned side,int active,unsigned texture) {
    if(side>1 || !NativePage(side)){originalDraw(side,active,texture);return;}
    auto* base=*reinterpret_cast<uint8_t**>(0x74D808);if(!base)return;
    auto* menu=base+side*0x1dc+0x2c;
    if(!active && *reinterpret_cast<float*>(menu+4)==0)return;
    const auto character=Character(side),selected=MenuColor(side);
    auto& state=pages[side];
    const unsigned thumbnail=*reinterpret_cast<unsigned*>(base+side*0x1dc+0x1d0);
    if(state.owner!=reinterpret_cast<uintptr_t>(base) || state.character!=character || *reinterpret_cast<int*>(menu)==0) {
        state={};state.owner=reinterpret_cast<uintptr_t>(base);state.character=character;
    }
    if(state.texture!=thumbnail){state.texture=thumbnail;state.thumbnails=false;}
    if(!state.thumbnails && *reinterpret_cast<unsigned*>(base+side*0x1dc+0x14))state.thumbnails=Thumbnails(side,thumbnail);
    reinterpret_cast<void (__fastcall*)(void*)>(active ? 0x4865B0 : 0x486610)(menu);
    if(state.page!=selected/6) {
        state.previous=state.ranks;
        for(unsigned i=0;i<MenuColors;++i)state.ranks[i]=ColorRank(i,selected);
        state.blend=state.page==UINT32_MAX ? 0.f : 1.f;state.page=selected/6;
    }
    state.blend=(std::max)(0.f,state.blend-.08f);
    const bool random=*reinterpret_cast<unsigned*>(menu+12)!=0;
    const int slide=int(std::lround((1-*reinterpret_cast<float*>(menu+4))*(side ? 276 : -276)));
    const int shift=(std::min)(32,*reinterpret_cast<int*>(menu+8)*4)*(side ? -1 : 1);
    for(unsigned color=0;color<MenuColors;++color) {
        const auto rank=state.ranks[color],old=state.previous[color];
        const bool selectedRow=color==selected && !random,available=color<36 || Available(side,color-36);
        const int x=Mix(X(side,old),X(side,rank),state.blend)+slide+(selectedRow ? shift : 0);
        const int y=Mix(Y(old),Y(rank),state.blend),depth=352-int(rank/6);
        auto shade=unsigned(Mix(255/(old/6+1),255/(rank/6+1),state.blend));
        if(!available)shade=shade*2/5;
        const unsigned tint=0xff000000|shade*0x010101;
        if(rank<6) {
            if(color>=36)ExtraLabel(color-36,x+(side ? 17 : 231),y+1,tint,depth);
            else {
                const int nx=x+(side ? 17 : 262);
                Sprite(texture,nx,y,16,16,int((color+1)/10)*16,96,16,16,tint,depth);
                Sprite(texture,nx+9,y,16,16,int((color+1)%10)*16,96,16,16,tint,depth);
            }
        }
        Sprite(texture,x,y,304,16,0,(side ? 48 : 0)+(selectedRow && rank<6 ? 0 : 16),304,16,tint,depth);
        if(thumbnail && *reinterpret_cast<unsigned*>(base+side*0x1dc+0x14) && available && (color<36 || state.thumbnails))
            Sprite(thumbnail,x+(side ? 10 : 254),y+1,40,14,int(color%8)*32,int(color/8)*16,32,16,tint,depth);
        else Sprite(0,x+(side ? 10 : 254),y+1,40,14,0,0,0,0,0xff000000,depth);
    }
    const int rx=(side ? 440 : -104)+slide+(random ? shift : 0);
    Sprite(texture,rx+(side ? 12 : 212),386,80,16,160,96,80,16,0xffffffff,352);
    Sprite(texture,rx,386,304,16,0,side ? 80 : 32,304,16,0xffffffff,352);
    const int hx=(side ? 416 : -32)+slide;
    Sprite(texture,hx,268,256,24,0,160,256,24,0xffffffff,352);
    Sprite(texture,hx+(side ? 8 : 148),270,96,16,304,0,96,16,0xffffffff,353);
}
}
bool Install() {
    const uint8_t input[]{0x85,0xf6,0x8a,0x4c,0x24,0x04};
    const uint8_t draw[]{0x83,0xec,0x2c,0x53,0x8b,0x1d,0x08,0xd8,0x74,0x00};
    const uint8_t update[]{0x83,0xec,0x14,0xd9,0xee,0x53};
    if(!game_build::RuntimeValidated() || std::memcmp(reinterpret_cast<void*>(0x48AB00),input,sizeof(input)) ||
       std::memcmp(reinterpret_cast<void*>(0x48ADD0),draw,sizeof(draw)) ||
       std::memcmp(reinterpret_cast<void*>(0x48A4E0),update,sizeof(update)))return false;
    if(MH_CreateHook(reinterpret_cast<void*>(0x48AB00),reinterpret_cast<void*>(cc_color_input_hook),&cc_color_input_original)!=MH_OK ||
       MH_CreateHook(reinterpret_cast<void*>(0x48ADD0),reinterpret_cast<void*>(Draw),reinterpret_cast<void**>(&originalDraw))!=MH_OK ||
       MH_CreateHook(reinterpret_cast<void*>(0x48A4E0),reinterpret_cast<void*>(Update),reinterpret_cast<void**>(&originalUpdate))!=MH_OK)return false;
    const bool ok=cccaster::hook_batch::Enable(reinterpret_cast<void*>(0x48AB00))==MH_OK && cccaster::hook_batch::Enable(reinterpret_cast<void*>(0x48ADD0))==MH_OK &&
        cccaster::hook_batch::Enable(reinterpret_cast<void*>(0x48A4E0))==MH_OK;
    domain::session::DebugLog("[ExtraColor] NATIVE_PAGES installed=%u pages=7 rows=6",unsigned(ok));return ok;
}
}
extern "C" __attribute__((force_align_arg_pointer)) int __cdecl cc_native_color_input(unsigned side,unsigned direction,unsigned buttons) {
    return cccaster::training_palette::selection::NativeInput(side,direction,buttons);
}
