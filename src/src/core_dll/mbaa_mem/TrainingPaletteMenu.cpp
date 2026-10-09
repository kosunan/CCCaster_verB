#include "core_dll/hook/HookBatch.hpp"
#include "core_dll/mbaa_mem/TrainingPaletteMenu.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/mbaa_mem/TrainingCharacterMenu.hpp"
#include "core_dll/engine/ExtraColorStore.hpp"
#include "core_dll/engine/TrainingPaletteMotion.hpp"
#include "core_dll/mbaa_mem/ExtraColorSelection.hpp"
#include <MinHook.h>
#include <d3d9.h>
#include <cstring>
#include <mutex>

extern "C" {
void* cc_palette_original = nullptr;
const uint32_t* __cdecl cc_palette_capture(uint32_t*, const uint32_t*, const uint8_t*);
__attribute__((naked)) void cc_palette_upload_hook() {
    // 0x4020E0: EAX=PAL、stack=CG/page-mask/keep。元ABIの全レジスタを保全。
    __asm__ __volatile__("pushfl; pushal; pushl 44(%esp); pushl 32(%esp); pushl 48(%esp);"
                         "call _cc_palette_capture; addl $12,%esp; movl %eax,28(%esp); popal; popfl; jmp *_cc_palette_original");
}
}
namespace cccaster::training_palette {
namespace {
std::array<Asset, 4> assets;
struct Draft { ExtraColor color; std::array<uint64_t,2> generations{}; bool pending=false; };
std::array<Draft,2> drafts;
std::mutex captureMutex;
bool installed = false, suppress = false;
bool& open = editorOpen;
uint32_t previous = 0;
unsigned session = 0, initialSlot = 0;
uint64_t assetGeneration = 0;
const char* error = "";
struct Identity { unsigned character=0, component=0, base=0; std::string resource; std::vector<Palette> standard; };
std::array<Identity,4> identities;
using Loader=void (__fastcall*)(uint32_t*,uint32_t*);
Loader originalLoader=nullptr;
bool Readable(const void* ptr, size_t bytes) {
    auto cursor = reinterpret_cast<uintptr_t>(ptr), end = cursor + bytes;
    if (!cursor || end < cursor) return false;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &m, sizeof(m)) || m.State != MEM_COMMIT ||
            (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const auto next = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}
template<class T> T* Ptr(uint32_t value) { return reinterpret_cast<T*>(value); }
bool Current(unsigned slot) {
    return installed && slot < 4 && *CC_GAME_MODE_ADDR == CC_GAME_MODE_IN_GAME &&
        *reinterpret_cast<uint8_t*>(0x555130 + slot * 0xAFC) &&
        assets[slot].owner == *reinterpret_cast<uint32_t*>(0x557D34 + slot * 12);
}
}
std::array<std::vector<uint32_t>,256> SampleBody(uint32_t* cg) {
    std::array<std::vector<uint32_t>,256> samples;
    if(!Readable(cg,0x3308) || !cg[0x3300/4])return samples;
    const unsigned unit=cg[0x32f0/4],count=cg[2];
    if(!unit || unit>256 || 256%unit || !count || count>512)return samples;
    const unsigned perRow=256/unit,perPage=perRow*perRow;
    // 先頭の通常体画像を使用し、飛び道具・技エフェクトの面積で代表色が変わるのを避ける。
    for(unsigned id=0;id<3000;++id) {
        const auto record=cg[0x410/4+id];if(record>=3000)continue;
        const auto* bmp=Ptr<uint32_t>(cg[0x32f4/4])+record*(0x58/4);
        if(!Readable(bmp,0x58) || (bmp[8]!=0 && bmp[8]!=5) || bmp[0x54/4]==UINT32_MAX)continue;
        const int width=int(bmp[0x38/4])-int(bmp[0x30/4])+1,height=int(bmp[0x3c/4])-int(bmp[0x34/4])+1;
        if(width<24 || width>512 || height<48 || height>512)continue;
        const unsigned n=bmp[0x50/4];if(!n || n>4096 || bmp[0x4c/4]>1000000)continue;
        const auto* cuts=Ptr<uint8_t>(cg[0x32f8/4])+bmp[0x4c/4]*12;
        if(!Readable(cuts,size_t(n)*12))continue;
        auto* source=Ptr<uint8_t>(cg[0x3300/4])+bmp[0x54/4];
        unsigned sampled=0;
        for(unsigned k=0;k<n;++k) {
            const auto* cut=cuts+k*12;
            const unsigned tile=*reinterpret_cast<const uint32_t*>(cut+4),page=tile/perPage;
            const int run=int(*reinterpret_cast<const int8_t*>(cut+8));
            const unsigned x=tile%perRow*unit,y=tile%perPage/perRow*unit;
            const unsigned w=run<=0 ? unsigned(-run)*unit : unit,h=run>0 ? unsigned(run)*unit : unit;
            if(!run || page>=count || x+w>256 || y+h>256){samples={};return samples;}
            if(cut[9])continue;
            const unsigned area=w*h;
            if(!Readable(source,size_t(area)*(bmp[8]==5 ? 2 : 1))){samples={};return samples;}
            for(unsigned dy=0;dy<h;++dy)for(unsigned dx=0;dx<w;++dx) {
                const auto index=source[dy*w+dx];
                if(!index || (bmp[8]==5 && source[area+dy*w+dx]<128))continue;
                samples[index].push_back(page*65536+(y+dy)*256+x+dx);++sampled;
            }
            source+=area*(bmp[8]==5 ? 2 : 1);
        }
        if(sampled)return samples;
    }
    return samples;
}
void Capture(uint32_t* cg, const uint32_t* palette, const uint8_t* mask) {
    std::lock_guard<std::mutex> lock(captureMutex);
    unsigned slot = 4;
    for (unsigned i = 0; i < 4; ++i)
        if (reinterpret_cast<uintptr_t>(cg) == *reinterpret_cast<uint32_t*>(0x557D34 + i*12)) { slot = i; break; }
    if (slot == 4) return;
    assets[slot] = {}; // 同じアドレスが再利用されても旧画像へ適用しない。
    drafts[slot%2]={};
    if (!Readable(cg, 0x3308) || !cg[0x3300/4]) return;
    const unsigned unit = cg[0x32f0/4], count = cg[2];
    if (!unit || unit > 256 || 256 % unit || !count || count > 512 || (mask && !Readable(mask,count))) return;
    const auto* pal = palette ? palette : cg + 4;
    if (!Readable(pal,1024)) return;
    Asset asset; asset.owner = reinterpret_cast<uintptr_t>(cg);
    asset.generation=++assetGeneration;
    asset.character=identities[slot].character;asset.component=identities[slot].component;
    asset.baseColor=identities[slot].base;asset.resource=identities[slot].resource;
    asset.standard=identities[slot].standard;
    asset.layout=2166136261u;
    const auto mix=[&](uint32_t value){asset.layout=(asset.layout^value)*16777619u;};
    mix(unit);mix(count);
    std::copy(pal,pal+256,asset.original.palette.begin());
    asset.original.palette[0] &= 0xff000000; // 元の予約バイトは保持する。
    for (unsigned i=1;i<256;++i)
        if (!(asset.original.palette[i]&0xffffff)) asset.original.palette[i] |= 0x010101;
    const unsigned perRow = 256/unit, perPage = perRow*perRow;
    for (unsigned id = 0; id < 3000; ++id) {
        const auto record = cg[0x410/4+id];
        if (record == UINT32_MAX || record >= 3000) continue;
        auto* bmp = Ptr<uint32_t>(cg[0x32f4/4]) + record*(0x58/4);
        if (!Readable(bmp,0x58) || bmp[0x20/4] > 5 || bmp[0x54/4] == UINT32_MAX) continue;
        for(unsigned b=0;b<0x58/4;++b)mix(bmp[b]);
        mix(id);
        const unsigned type=bmp[0x20/4];
        const uint32_t bank=(type==2 || type==3 || type==4) ? bmp[0x48/4]+1 : 0;
        if((type==2 || type==3 || type==4) && !bank)continue;
        if(bank && !asset.original.effects.count(bank)) {
            const auto* colors=Ptr<uint32_t>(cg[0x32fc/4])+(bank-1);
            if(!Readable(colors,type==3 ? 4 : 1024))continue;
            auto& dest=asset.original.effects[bank];
            if(type==3)dest.fill(*colors);else std::copy(colors,colors+256,dest.begin());
        }
        const int n = bmp[0x50/4];
        if (n <= 0 || n > 4096 || bmp[0x4c/4] > 1000000) continue;
        auto* cuts = Ptr<uint8_t>(cg[0x32f8/4]) + bmp[0x4c/4]*12;
        if (!Readable(cuts,size_t(n)*12)) continue;
        Sprite sprite; sprite.id = id;sprite.type=type;sprite.bank=bank;
        sprite.name.assign(reinterpret_cast<char*>(bmp),strnlen(reinterpret_cast<char*>(bmp),32));
        sprite.left = int(bmp[0x30/4]); sprite.top = int(bmp[0x34/4]);
        sprite.width = int(bmp[0x38/4])-sprite.left+1; sprite.height = int(bmp[0x3c/4])-sprite.top+1;
        if (sprite.width <= 0 || sprite.height <= 0 || sprite.width > 1024 || sprite.height > 1024) continue;
        auto* source = Ptr<uint8_t>(cg[0x3300/4]) + bmp[0x54/4];
        bool complete = true;
        for (int k=0; k<n; ++k) {
            const auto* cut = cuts + k*12;
            const unsigned index = *reinterpret_cast<const uint32_t*>(cut+4), page = index/perPage;
            const int run = int(*reinterpret_cast<const int8_t*>(cut+8));
            Tile tile{page,(index%perPage/perRow)*unit,(index%perRow)*unit,
                      run <= 0 ? unsigned(-run)*unit : unit,run > 0 ? unsigned(run)*unit : unit,
                      int(*reinterpret_cast<const int16_t*>(cut))+128,int(*reinterpret_cast<const int16_t*>(cut+2))+224};
            if (page >= count || tile.x+tile.width > 256 || tile.y+tile.height > 256 || !run) { complete=false; break; }
            const auto area = size_t(tile.width)*tile.height;
            const auto bytes = area*(type==1 ? 4 : type>=4 ? 2 : 1);
            if (!cut[9]) {
                if (!Readable(source,bytes)) { complete=false; break; }
                if (!mask || mask[page]) {
                    auto& dest = asset.pages[page];
                    if(bank && dest.banks.empty())dest.banks.resize(65536);
                    if((type==3 || type>=4) && dest.alpha.empty())dest.alpha.resize(65536);
                    if(type==1 && dest.direct.empty())dest.direct.resize(65536);
                    for (unsigned y=0; y<tile.height; ++y) for(unsigned x=0;x<tile.width;++x) {
                        const auto from=y*tile.width+x,to=(tile.y+y)*256+tile.x+x;
                        dest.indices[to]=type==3 ? 0 : source[from];
                        dest.valid[to]=type==1 ? 3 : (type==3 || type>=4) ? 2 : 1;
                        if(!dest.banks.empty())dest.banks[to]=bank;
                        if(type==3)dest.alpha[to]=source[from];
                        if(type>=4)dest.alpha[to]=source[area+from];
                        if(type==1)dest.direct[to]=uint32_t(source[from*4+2])|(uint32_t(source[from*4+1])<<8)|
                            (uint32_t(source[from*4])<<16)|(uint32_t(source[from*4+3])<<24);
                    }
                }
                source += bytes;
            }
            if (mask && !mask[page]) complete = false;
            sprite.tiles.push_back(tile);
        }
        if (complete && !sprite.tiles.empty()) asset.sprites.push_back(std::move(sprite));
    }
    asset.applied = asset.original;
    domain::session::DebugLog("[TrainingPalette] CAPTURE slot=%u owner=%p pages=%u sprites=%u effects=%u character=%u component=%u layout=%08x",slot,cg,
                             unsigned(asset.pages.size()),unsigned(asset.sprites.size()),unsigned(asset.original.effects.size()),asset.character,asset.component,asset.layout);
    assets[slot] = std::move(asset);
}
bool Upload(unsigned slot,const Edit& edit);
__attribute__((force_align_arg_pointer)) void __fastcall LoadCharacters(uint32_t* description,uint32_t* loading) {
    drafts={};
    for(unsigned slot=0;slot<4;++slot) {
        auto& identity=identities[slot];identity={};assets[slot]={};
        const auto* part=description+slot*11;
        if(part[4]>100 || part[5]>1)continue;
        identity.character=part[4];identity.component=part[5];identity.base=part[3];
        auto* table=*reinterpret_cast<uint32_t**>(0x55DF18);
        if(!table || part[4]>=table[3])continue;
        const auto address=table[0] ? Ptr<uint32_t>(table[1])[part[4]] : table[1]+table[2]*part[4];
        const auto* name=Ptr<char>(address+0x34+part[5]*32);
        identity.resource.assign(name,strnlen(name,32));
        std::vector<uint8_t> bytes;
        if(training_character::ReadImage((".\\data\\"+identity.resource+".pal").c_str(),bytes) && bytes.size()>=4) {
            uint32_t count=0;std::memcpy(&count,bytes.data(),4);
            if(count>=36 && count<=256 && bytes.size()>=4+size_t(count)*1024) {
                identity.standard.resize(36);
                for(unsigned i=0;i<36;++i)std::memcpy(identity.standard[i].data(),bytes.data()+4+i*1024,1024);
            }
        }
    }
    originalLoader(description,loading);
    for(unsigned slot=0;slot<4;++slot) {
        auto& asset=assets[slot];if(!asset.owner)continue;
        if(const auto extra=selection::Resolve(slot%2,asset.character))for(const auto& part:extra->parts) {
            if(part.component!=asset.component)continue;
            const bool valid=part.layout==asset.layout;
            const bool applied=valid && Upload(slot,part.edit);
            domain::session::DebugLog("[ExtraColor] LOAD slot=%u character=%u layout=%08x matched=%u applied=%u",
                slot,asset.character,asset.layout,unsigned(valid),unsigned(applied));
        }
    }
}
bool Install() {
    if (installed) return true;
    const uint8_t signature[]{0x83,0xec,0x60,0x53,0x55,0x8b,0x6c,0x24,0x6c};
    const uint8_t cache[]{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x8b,0x0d,0xac,0xe9,0x76,0x00};
    const uint8_t loader[]{0x81,0xec,0x4c,0x02,0x00,0x00};
    if (!game_build::RuntimeValidated() || std::memcmp(Ptr<void>(0x4020E0),signature,sizeof(signature)) ||
        std::memcmp(Ptr<void>(0x4063D0),cache,sizeof(cache)) || std::memcmp(Ptr<void>(0x4489E0),loader,sizeof(loader))) return false;
    if(MH_CreateHook(Ptr<void>(0x4489E0),reinterpret_cast<void*>(LoadCharacters),reinterpret_cast<void**>(&originalLoader))!=MH_OK ||
       cccaster::hook_batch::Enable(Ptr<void>(0x4489E0))!=MH_OK)return false;
    if (MH_CreateHook(Ptr<void>(0x4020E0),reinterpret_cast<void*>(cc_palette_upload_hook),&cc_palette_original) != MH_OK)
        return false;
    installed = cccaster::hook_batch::Enable(Ptr<void>(0x4020E0)) == MH_OK && selection::Install();
    return installed;
}
unsigned Session() { return session; }
unsigned InitialSlot() { return initialSlot; }
const char* Error() { return error; }
const Asset* Get(unsigned slot) {
    return Current(slot) && !assets[slot].sprites.empty() ? &assets[slot] : nullptr;
}
void Open(unsigned slot) {
    // キャラ読込み完了後、ゲームスレッド上で定義をコピーする。UIは生ポインターを保持しない。
    for(unsigned i=0;i<4;++i)if(Current(i) && !assets[i].sprites.empty()) {
        const auto read=[](uint32_t address,void* dest,size_t size) {
            if(!Readable(Ptr<void>(address),size))return false;
            std::memcpy(dest,Ptr<void>(address),size);return true;
        };
        uint32_t file=0,ha6=0,table=0;
        if(read(0x555130+i*0xAFC+0x330,&file,4) && read(file,&ha6,4) && ha6 && ha6<=UINT32_MAX-4 && read(ha6+4,&table,4))
            assets[i].motions=ReadMotions(assets[i],table,read);
        else assets[i].motions.clear();
        unsigned objects=0;for(const auto& motion:assets[i].motions)objects+=motion.object;
        domain::session::DebugLog("[TrainingPalette] MOTIONS slot=%u total=%u objects=%u",i,unsigned(assets[i].motions.size()),objects);
    }
    open = true; suppress = true; error = "";
    initialSlot = slot % 2; ++session;
    domain::session::DebugLog("[TrainingPalette] OPEN slot=%u",slot);
}
const ExtraColor* Pending(unsigned slot) {
    if(slot>=4)return nullptr;
    const auto& draft=drafts[slot%2];
    if(!draft.pending)return nullptr;
    for(unsigned n=0;n<2;++n) {
        const auto* asset=Get(slot%2+n*2);
        if(draft.generations[n]!=(asset ? asset->generation : 0))return nullptr;
    }
    return &draft.color;
}
void StagePackage(unsigned slot,const ExtraColor& value) {
    if(!open || !Get(slot))return;
    auto& draft=drafts[slot%2];draft.color=value;draft.pending=true;
    for(unsigned n=0;n<2;++n) {
        const auto* asset=Get(slot%2+n*2);
        draft.generations[n]=asset ? asset->generation : 0;
    }
}
void Close() {
    // B・Escape・F4・閉じるボタンは同じ経路。失敗した下書きは捨てない。
    if(open)for(unsigned side=0;side<2;++side)if(const auto* value=Pending(side)) {
        if(!ApplyPackage(side,*value,false))return;
    }
    drafts={};
    if (open) domain::session::DebugLog("[TrainingPalette] CLOSE");
    open = false; suppress = true;
}
void Step(game_interface::GameInput& p1, game_interface::GameInput& p2, bool configuring) {
    const auto buttons = p1.buttons | p2.buttons;
    const bool wasOpen = open;
    if (*CC_GAME_MODE_ADDR != CC_GAME_MODE_IN_GAME) { open = false; suppress = false; previous = 0; return; }
    if (open && configuring) Close();
    if (open && !*Ptr<uint32_t>(0x74D7FC)) { open=false; suppress=true; }
    if (open && !suppress && ((buttons & ~previous) & (CC_BUTTON_B | CC_BUTTON_CANCEL | CC_BUTTON_START))) Close();
    previous = buttons;
    if (open || suppress || wasOpen) {
        const bool released = !buttons && !p1.direction && !p2.direction;
        p1 = p2 = {};
        if (released) suppress = false;
    }
}
bool Upload(unsigned slot, const Edit& edit) {
    error = "";
    if(slot>=4 || !assets[slot].owner || assets[slot].owner!=*Ptr<uint32_t>(0x557D34+slot*12))return false;
    auto& asset = assets[slot];
    for(const auto& [id,colors]:asset.original.effects)
        if(!edit.effects.count(id)){error="Object palette data is incomplete.";return false;}
    auto* cg = reinterpret_cast<uint32_t*>(asset.owner);
    auto* list = Ptr<uint32_t>(cg[0x3304/4]);
    if (!Readable(list,16) || list[3] > 512) { error = "Texture list is unavailable."; return false; }
    for(const auto& [key,color]:edit.pixels)if(key>=512u*65536){error="Invalid pixel address.";return false;}
    struct Locked { IDirect3DTexture9* texture; D3DLOCKED_RECT lock; D3DFORMAT format; unsigned page; };
    std::vector<Locked> locked;
    auto unlock = [&] { bool ok=true; for (auto& page : locked) ok = SUCCEEDED(page.texture->UnlockRect(0)) && ok; return ok; };
    // 全対象を確認・ロックしてから書く。失敗時は1画素も変更しない。
    for (const auto& [page, pixels] : asset.pages) {
        if (page >= list[3]) { error = "Texture page is unavailable."; break; }
        auto* item = list[0] ? Ptr<uint32_t>(Ptr<uint32_t>(list[1])[page]) : Ptr<uint32_t>(list[1]+list[2]*page);
        if (!Readable(item,4) || !Readable(Ptr<void>(*item),0x44)) { error = "Texture page is unavailable."; break; }
        auto* texture = Ptr<IDirect3DTexture9>(Ptr<uint32_t>(*item)[2]);
        D3DSURFACE_DESC desc{}; D3DLOCKED_RECT rect{};
        if (!texture || FAILED(texture->GetLevelDesc(0,&desc)) || desc.Width != 256 || desc.Height != 256 ||
            (desc.Format != D3DFMT_A1R5G5B5 && desc.Format != D3DFMT_A4R4G4B4 && desc.Format != D3DFMT_A8R8G8B8) ||
            FAILED(texture->LockRect(0,&rect,nullptr,0))) { error = "Texture cannot be edited on this device."; break; }
        locked.push_back({texture,rect,desc.Format,page});
    }
    if (*error) { unlock(); return false; }
    for (auto& page : locked) {
        const auto& pixels = asset.pages.at(page.page);
        for (unsigned y=0; y<256; ++y) for (unsigned x=0; x<256; ++x) {
            if (!pixels.valid[y*256+x]) continue;
            const uint32_t color = asset.Color(edit,page.page*65536+y*256+x);
            auto* row = static_cast<uint8_t*>(page.lock.pBits)+y*page.lock.Pitch;
            if (page.format == D3DFMT_A8R8G8B8)
                reinterpret_cast<uint32_t*>(row)[x] = (color & 0xff00ff00) | ((color & 255)<<16) | ((color>>16)&255);
            else reinterpret_cast<uint16_t*>(row)[x] = Pack16(color,page.format == D3DFMT_A4R4G4B4);
        }
    }
    const bool uploaded = unlock();
    std::copy(edit.palette.begin(),edit.palette.end(),cg+4);
    cg[4]&=0xff000000; // ゲーム内の透明色0。ACTの元RGBは編集データに保持する。
    // 元の描画キャッシュを破棄し、次の描画で編集済みタイルからポーズを再生成。
    reinterpret_cast<void (__cdecl*)()>(0x4063D0)();
    asset.applied = edit;
    domain::session::DebugLog("[TrainingPalette] APPLY slot=%u pages=%u pixels=%u uploaded=%u",slot,
        unsigned(locked.size()),unsigned(edit.pixels.size()),unsigned(uploaded));
    if (!uploaded) { error = "Device upload failed. Reopen or reload the character."; return false; }
    return true;
}
bool Apply(unsigned slot,const Edit& edit,unsigned baseColor) {
    if(!open || !Get(slot) || !*Ptr<uint32_t>(0x74D7FC)){error="Character changed. Reopen the editor.";return false;}
    if(!Upload(slot,edit))return false;
    if(baseColor<36)assets[slot].baseColor=baseColor;
    Close();return true;
}
bool ApplyPackage(unsigned slot,const ExtraColor& value,bool close) {
    const auto* main=Get(slot);
    if(!open || !main || main->character!=value.character || !*Ptr<uint32_t>(0x74D7FC)) {
        error="Character changed. Reopen the editor.";return false;
    }
    std::vector<std::pair<unsigned,Edit>> previousEdits;
    for(unsigned i:{slot%2,slot%2+2})if(const auto* asset=Get(i);asset && asset->character==value.character) {
        for(const auto& part:value.parts)if(part.component==asset->component) {
            if(part.layout!=asset->layout){error="Sprite layout differs from this character data.";return false;}
            previousEdits.emplace_back(i,asset->applied);
        }
    }
    for(const auto& [i,before]:previousEdits)for(const auto& part:value.parts)if(part.component==assets[i].component) {
        if(part.edit==assets[i].applied)continue;
        if(!Upload(i,part.edit)) {
            const auto* failure=error;
            for(const auto& [restore,edit]:previousEdits)Upload(restore,edit);
            error=failure;return false;
        }
    }
    for(const auto& [i,before]:previousEdits)assets[i].baseColor=value.baseColor;
    if(close)Close();return true;
}
}
extern "C" __attribute__((force_align_arg_pointer)) const uint32_t* __cdecl cc_palette_capture(
    uint32_t* cg, const uint32_t* palette, const uint8_t* mask) {
    try { cccaster::training_palette::Capture(cg,palette,mask); }
    catch (...) { cccaster::domain::session::DebugLog("[TrainingPalette] CAPTURE failed"); }
    return cccaster::training_palette::selection::PreviewPalette(cg,palette);
}
