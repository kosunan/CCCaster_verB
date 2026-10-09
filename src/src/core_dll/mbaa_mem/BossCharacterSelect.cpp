#include "core_dll/hook/HookBatch.hpp"
#include "core_dll/mbaa_mem/BossCharacterSelect.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "shared_contracts/BossCharacters.hpp"
#include <MinHook.h>
#include <atomic>
#include <cstring>
#include <string>
#include <cstdio>

extern "C" {
void* cc_boss_preview_original=nullptr;
void* cc_boss_palette_original=nullptr;
void* cc_boss_scale_original=nullptr;
void* cc_boss_scale_resume=reinterpret_cast<void*>(0x486008);
float __cdecl cc_boss_preview_scale(unsigned*);
unsigned __cdecl cc_boss_preview(unsigned,unsigned,unsigned*);
unsigned __cdecl cc_boss_preview_id(unsigned);
unsigned __cdecl cc_boss_grid_depth(unsigned);
unsigned __cdecl cc_boss_cursor_depth(const unsigned*,unsigned);
void* cc_boss_grid_depth_resume=reinterpret_cast<void*>(0x48BC80);
void* cc_boss_face_depth_resume=reinterpret_cast<void*>(0x48BDC1);
void* cc_boss_cursor_depth_resume=reinterpret_cast<void*>(0x48BE6B);
void* cc_boss_cursor_front_resume=reinterpret_cast<void*>(0x48BEAF);
// 4か所のpush depthだけを置換。元の条件分岐用FLAGS・全レジスタとx87スタックを保持する。
__attribute__((naked)) void cc_boss_grid_depth_hook() {
    __asm__ __volatile__("pushl $360; pushfl; pushal; pushl %ebx; call _cc_boss_grid_depth; addl $4,%esp;"
                         "movl %eax,36(%esp); popal; popfl; jmp *_cc_boss_grid_depth_resume");
}
__attribute__((naked)) void cc_boss_face_depth_hook() {
    __asm__ __volatile__("pushl $361; pushfl; pushal; pushl $361; pushl %ebx; call _cc_boss_cursor_depth; addl $8,%esp;"
                         "movl %eax,36(%esp); popal; popfl; jmp *_cc_boss_face_depth_resume");
}
__attribute__((naked)) void cc_boss_cursor_depth_hook() {
    // 元ESP+0x10は現在の側のキャラ番号ポインタ。保存分40バイトを加算する。
    __asm__ __volatile__("pushl $362; pushfl; pushal; movl 56(%esp),%eax; pushl $362; pushl %eax;"
                         "call _cc_boss_cursor_depth; addl $8,%esp; movl %eax,36(%esp); popal; popfl;"
                         "jmp *_cc_boss_cursor_depth_resume");
}
__attribute__((naked)) void cc_boss_cursor_front_hook() {
    // 直前のcdecl描画の引数48バイトが残るため、同じポインタは元ESP+0x40。
    __asm__ __volatile__("pushl $363; pushfl; pushal; movl 104(%esp),%eax; pushl $363; pushl %eax;"
                         "call _cc_boss_cursor_depth; addl $8,%esp; movl %eax,36(%esp); popal; popfl;"
                         "jmp *_cc_boss_cursor_front_resume");
}
// ネイティブの描画キューへラベルを登録。幅だけEDX、残りはcdeclスタック。
__attribute__((naked)) void __cdecl cc_boss_label_sprite(unsigned,unsigned,int,int,int,int,int,int,int,unsigned,unsigned,int,int) {
    __asm__ __volatile__("movl 52(%esp),%edx; movl $0x415580,%eax; jmp *%eax");
}
// 0x485EB0/0x485E00: ECX=component, EDX=character, EDI=出力先。通常fastcallにEDIを落とさない。
__attribute__((naked)) void cc_boss_preview_hook() {
    __asm__ __volatile__("pushl %edi; pushl %edx; pushl %ecx; call _cc_boss_preview; addl $12,%esp; ret");
}
__attribute__((naked)) unsigned cc_boss_preview_call(unsigned,unsigned,unsigned*) {
    __asm__ __volatile__("pushl %edi; movl 8(%esp),%ecx; movl 12(%esp),%edx; movl 16(%esp),%edi;"
                         "call *_cc_boss_preview_original; popl %edi; ret");
}
__attribute__((naked)) void cc_boss_palette_hook() {
    __asm__ __volatile__("pushfl; pushal; pushl %edx; call _cc_boss_preview_id; addl $4,%esp;"
                         "movl %eax,20(%esp); popal; popfl; jmp *_cc_boss_palette_original");
}
// 0x486002のfldを置換。元と同じx87結果1つを残し、全レジスター・フラグを保持。
__attribute__((naked)) void cc_boss_scale_hook() {
    __asm__ __volatile__("pushfl; pushal; pushl %edi; call _cc_boss_preview_scale; addl $4,%esp;"
                         "popal; popfl; jmp *_cc_boss_scale_resume");
}
}

namespace cccaster::boss::selection {
namespace {
bool installed=false,enabled=false;
std::atomic<bool> presentation{false};
using FileLoader=int (__cdecl*)(const char*,void*,unsigned,unsigned);
FileLoader originalFile=nullptr;
using GridDraw=unsigned (__stdcall*)(unsigned);
GridDraw originalGridDraw=nullptr;
void* originalDepth[4]{};
unsigned* Grid(){return *reinterpret_cast<unsigned**>(0x77181C);}
unsigned* Cursor(unsigned side){return side ? CC_P2_SELECTOR_MODE_ADDR : CC_P1_SELECTOR_MODE_ADDR;}

void DrawBossLabel(int x,int y) {
    // 7x9の字形を原寸の矩形で描く。フォント画像の縮小サンプリングで線を欠かさない。
    // 2pxの縦線・字間を確保し、4文字34pxを元の顔幅内へ収める。
    constexpr uint8_t glyphs[][9]{
        {0b1111110,0b1100011,0b1100011,0b1100011,0b1111110,0b1100011,0b1100011,0b1100011,0b1111110}, // B
        {0b0111110,0b1100011,0b1100011,0b1100011,0b1100011,0b1100011,0b1100011,0b1100011,0b0111110}, // O
        {0b0111111,0b1100000,0b1100000,0b1100000,0b0111110,0b0000011,0b0000011,0b0000011,0b1111110}, // S
    };
    constexpr unsigned letters[]{0,1,2,2};
    for(unsigned i=0;i<4;++i) {
        const auto& glyph=glyphs[letters[i]];
        for(unsigned row=0;row<9;) {
            unsigned end=row+1;
            while(end<9 && glyph[end]==glyph[row])++end;
            for(unsigned column=0;column<7;) {
                if(!(glyph[row]&(0x40u>>column))){++column;continue;}
                const auto begin=column++;
                while(column<7 && (glyph[row]&(0x40u>>column)))++column;
                cc_boss_label_sprite(0,0,x+int(i*9+begin),y+int(row),int(end-row),0,0,0,0,
                    0xFFFFE223,0,345,int(column-begin));
            }
            row=end;
        }
    }
    cc_boss_label_sprite(0,0,x-2,y-1,11,0,0,0,0,0xD7181200,0,344,38);
}

// 元のファイルが見つからない表示画像だけを共用する。戦闘定義の名前は変更しない。
std::string ImageAlias(const char* path) {
    std::string value=path ? path : "";
    for(auto& ch:value){if(ch=='/')ch='\\';if(ch>='A' && ch<='Z')ch+=32;}
    struct Prefix{const char* text;bool giant;};
    constexpr Prefix prefixes[]={{".\\grp\\c_sel_aa\\chara\\csel_c",true},
        {".\\grp\\c_sel_aa\\palette\\color_c",true},{".\\grp\\gauge_aa\\face\\face",false},
        {".\\grp\\cut\\cut_",false},
        {".\\grp\\vsdemo_aa\\vs_cut\\vs_cut",true},
        {".\\grp\\vsdemo_aa\\vs_flash\\vs_fl",true},
        {".\\grp\\vsdemo_aa\\vs_color\\vs_chcolor",true},
        {".\\grp\\vsdemo_aa\\vs_name00\\vs_name00_",true},
        {".\\grp\\vsdemo_aa\\vs_name01\\vs_name01_",true},
        {".\\grp\\vsdemo_aa\\vs_command\\vs_com",true}};
    for(const auto& prefix:prefixes) {
        const size_t n=std::strlen(prefix.text);
        if(value.compare(0,n,prefix.text) || value.size()<n+2 || value[n]<'0' || value[n]>'9' || value[n+1]<'0' || value[n+1]>'9')continue;
        const unsigned id=(value[n]-'0')*10+value[n+1]-'0';
        if(!IsBoss(id) || id==32 || (id==16 && !prefix.giant))return {};
        auto base=Base(id);
        // 巨大秋葉の固有技表は0のみ。ボスタッグの元キャラ35にも3の画像はない。
        if(value.find("\\vs_command\\")!=std::string::npos && (id==16 || id==85)) {
            if(id==16)base=16;
            if(value.size()>n+3 && value[n+2]=='_')value[n+3]='0';
        }
        value[n]=char('0'+base/10);value[n+1]=char('0'+base%10);
        return value;
    }
    return {};
}
__attribute__((force_align_arg_pointer)) int __cdecl LoadFile(const char* path,void* out,unsigned flags,unsigned wait) {
    const auto result=originalFile(path,out,flags,wait);
    if(result || !presentation.load(std::memory_order_relaxed))return result;
    const auto alias=ImageAlias(path);
    if(alias.empty())return result;
    const auto loaded=originalFile(alias.c_str(),out,flags,wait);
    domain::session::DebugLog("[BossSelect] IMAGE from=%s to=%s loaded=%d",path,alias.c_str(),loaded);
    return loaded;
}
template<class T> bool Hook(uintptr_t address,const unsigned char* signature,size_t size,void* hook,T* original) {
    if(std::memcmp(reinterpret_cast<void*>(address),signature,size))return false;
    return MH_CreateHook(reinterpret_cast<void*>(address),hook,reinterpret_cast<void**>(original))==MH_OK &&
           cccaster::hook_batch::Enable(reinterpret_cast<void*>(address))==MH_OK;
}
__attribute__((force_align_arg_pointer)) unsigned __stdcall DrawGrid(unsigned texture) {
    const auto result=originalGridDraw(texture);
    DrawLabels();
    return result;
}
bool Install() {
    if(installed)return true;
    if(!game_build::RuntimeValidated())return false;
    const unsigned char file[]{0x81,0xec,0x34,0x01,0,0,0xa1,0x58,0xb4,0x54,0};
    const unsigned char preview[]{0x81,0xec,0x10,0x01,0,0,0xa1,0x58,0xb4,0x54,0};
    const unsigned char palette[]{0x81,0xec,0x0c,0x01,0,0};
    const unsigned char scale[]{0xd9,0x05,0x3c,0xd8,0x53,0};
    const unsigned char gridDraw[]{0x55,0x8b,0xec,0x83,0xe4,0xf8};
    const unsigned char sprite[]{0x55,0x8b,0xec,0x83,0xe4,0xf8,0x81,0xec,0x80,0,0,0};
    if(std::memcmp(reinterpret_cast<void*>(0x415580),sprite,sizeof(sprite)))return false;
    if(!Hook(0x4C8B10,file,sizeof(file),reinterpret_cast<void*>(LoadFile),&originalFile) ||
       !Hook(0x485EB0,preview,sizeof(preview),reinterpret_cast<void*>(cc_boss_preview_hook),&cc_boss_preview_original) ||
       !Hook(0x485E00,palette,sizeof(palette),reinterpret_cast<void*>(cc_boss_palette_hook),&cc_boss_palette_original) ||
       !Hook(0x486002,scale,sizeof(scale),reinterpret_cast<void*>(cc_boss_scale_hook),&cc_boss_scale_original) ||
       !Hook(0x48BB80,gridDraw,sizeof(gridDraw),reinterpret_cast<void*>(DrawGrid),&originalGridDraw))return false;
    const uintptr_t depthAddresses[]{0x48BC7B,0x48BDBC,0x48BE66,0x48BEAA};
    void* depthHooks[]{reinterpret_cast<void*>(cc_boss_grid_depth_hook),reinterpret_cast<void*>(cc_boss_face_depth_hook),
        reinterpret_cast<void*>(cc_boss_cursor_depth_hook),reinterpret_cast<void*>(cc_boss_cursor_front_hook)};
    for(unsigned i=0;i<4;++i) {
        const unsigned char pushDepth[]{0x68,static_cast<unsigned char>(0x64+i),0x01,0,0};
        if(!Hook(depthAddresses[i],pushDepth,sizeof(pushDepth),depthHooks[i],&originalDepth[i]))return false;
    }
    // csel_icon00 / csel_nameの元アトラス番号。固有絵のある16/32は保持。
    for(const auto id:Characters)if(id>=50) {
        reinterpret_cast<int*>(0x5519F8)[id]=reinterpret_cast<const int*>(0x5519F8)[Base(id)];
        reinterpret_cast<int*>(0x551B90)[id]=reinterpret_cast<const int*>(0x551B90)[Base(id)];
    }
    // 巨大秋葉の名前表の既存番号は別キャラの画像を指す。秋葉の名前画像を使う。
    reinterpret_cast<int*>(0x551B90)[16]=reinterpret_cast<const int*>(0x551B90)[3];
    installed=true;
    domain::session::DebugLog("[BossSelect] INSTALLED nativeGrid=63 randomCell=49 sides=4+4");
    return true;
}
}
bool Enabled(){return enabled;}
void DrawLabels() {
    if(!enabled || *CC_GAME_MODE_ADDR!=CC_GAME_MODE_CHARA_SELECT)return;
    const auto* grid=Grid();
    if(!grid)return;
    // HUD最前面ではなく、顔・カーソルの直後かつ標準/EXTRAカラー一覧の下へ合成する。
    // ラベルも元の画像座標・拡大に追従させる。
    auto& mode=*reinterpret_cast<uint8_t*>(0x56447F);
    const auto previousMode=mode;mode=2;
    for(const auto cell:Cells) {
        const auto* entry=grid+cell*6;
        if(!IsBoss(entry[2]) || *reinterpret_cast<const float*>(entry+5)>.01f)continue;
        DrawBossLabel(int(entry[3])+7,int(entry[4])+9);
    }
    mode=previousMode;
}
bool Configure(unsigned mode,bool allow) {
    // 同一PCのVersusにはフック・セル・選択値を適用しない。
    if(mode==5)return true;
    if(mode!=0 && mode!=1 && mode!=2 && mode!=3)return true;
    if(!Install())return false;
    presentation.store(true,std::memory_order_relaxed);
    if(enabled!=allow){enabled=allow;domain::session::DebugLog("[BossSelect] ENABLED mode=%u value=%u",mode,unsigned(enabled));}
    if(*CC_GAME_MODE_ADDR!=CC_GAME_MODE_CHARA_SELECT)return true;
    if(auto* grid=Grid())for(unsigned i=0;i<Characters.size();++i)grid[Cells[i]*6+2]=enabled ? Characters[i] : UINT32_MAX;
    if(enabled)for(unsigned side=0;side<2;++side) {
        auto* cursor=Cursor(side);
        if(!IsBoss(cursor[4]))continue;
        // 標準の再入場は静的な通常キャラ表を検索するため、ボスのセルが-1になる。
        // 動的一覧を使う通常入力へ戻す前に、この列の対応セルを復元する。
        if(cursor[3]>=63)cursor[3]=Cell(cursor[4]);
        cursor[5]=Moon(cursor[4]);
        // 各ボスは収録スタイルが1つだけ。3択の標準ムーンを回して未収録TXTへ入らない。
        if(cursor[0]==1) {
            cursor[0]=2;
            domain::session::DebugLog("[BossSelect] STYLE side=%u character=%u moon=%u",side,cursor[4],cursor[5]);
        }
    }
    return true;
}
}

extern "C" __attribute__((force_align_arg_pointer)) unsigned cc_boss_grid_depth(unsigned offset) {
    const auto* grid=*reinterpret_cast<unsigned**>(0x77181C);
    return cccaster::boss::selection::Enabled() && grid && offset<63*24 &&
        cccaster::boss::IsBoss(grid[offset/4+2]) ? 340u : 360u;
}
extern "C" __attribute__((force_align_arg_pointer)) unsigned cc_boss_cursor_depth(const unsigned* character,unsigned depth) {
    return cccaster::boss::selection::Enabled() && character && cccaster::boss::IsBoss(*character) ? depth-20 : depth;
}
extern "C" __attribute__((force_align_arg_pointer)) unsigned cc_boss_preview_id(unsigned character) {
    return cccaster::boss::Base(character);
}
extern "C" __attribute__((force_align_arg_pointer)) float cc_boss_preview_scale(unsigned* resource) {
    const float scale=*reinterpret_cast<const float*>(0x53D83C);
    const auto base=*reinterpret_cast<uintptr_t*>(0x74D808);
    if(base)for(unsigned side=0;side<2;++side) {
        const auto preview=base+side*0x1dc;
        if(reinterpret_cast<uintptr_t>(resource)==preview+0x1a8 &&
           *reinterpret_cast<const unsigned*>(preview+4)==32)return scale*.125f;
    }
    return scale;
}
extern "C" __attribute__((force_align_arg_pointer)) unsigned cc_boss_preview(unsigned part,unsigned character,unsigned* destination) {
    if(character==32) {
        // ヘルメスには_csel専用TXTがない。固有のCG/PATを使う通常定義からプレビューを生成。
        reinterpret_cast<void (__thiscall*)(void*,const char*)>(0x448920)(reinterpret_cast<void*>(destination[2]),".\\data\\hermes_0.txt");
        return 1;
    }
    return cc_boss_preview_call(part,cccaster::boss::Base(character),destination);
}
