#include "core_dll/ui/TrainingPaletteView.hpp"
#include "core_dll/ui/HudTheme.hpp"
#include "core_dll/mbaa_mem/TrainingPaletteMenu.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/engine/ExtraColorStore.hpp"
#include "core_dll/mbaa_mem/ExtraColorSelection.hpp"
#include <commdlg.h>
#include <cmath>

namespace cccaster::domain::ui::training_palette_view {
namespace {
using namespace training_palette;
IDirect3DDevice9* device = nullptr;
IDirect3DTexture9* preview = nullptr;
unsigned session = 0, slot = 0, spriteIndex = 0, selected = 1;
unsigned bank=0, baseColor=0;
int extraSlot=0, standardColor=0;
uint32_t directColor=0;
bool directSelected=false;
std::string fileNotice;
std::vector<ExtraPart> importedParts;
bool pencil = false, dirty = true, erase = false;
bool dropNextFrame=false;
int zoom = 0, lastX = -1, lastY = -1;
float brush[3]{1,0,0};
Edit edit;
History history;
void DropPreview() { if (preview) preview->Release(); preview = nullptr; dirty = true; }
void SelectColor(const Asset& asset, uint32_t key) {
    const int index = asset.Index(key);
    if(!asset.Valid(key))return;
    directSelected=index<0;bank=asset.Bank(key);
    if(index>=0)selected=unsigned(index);
    const auto* colors=edit.Bank(bank);
    const uint32_t color = pencil || directSelected ? asset.Color(edit,key) : colors ? (*colors)[selected] : 0;
    if(directSelected)directColor=asset.pages.at(key/65536).direct[key%65536]&0xffffff;
    brush[0] = (color&255)/255.f; brush[1] = ((color>>8)&255)/255.f; brush[2] = ((color>>16)&255)/255.f;
}
void Swatch(unsigned index) {
    selected = index;directSelected=false;
    const auto* colors=edit.Bank(bank);if(!colors){bank=0;colors=&edit.palette;}
    const auto color = (*colors)[index];
    brush[0]=(color&255)/255.f; brush[1]=((color>>8)&255)/255.f; brush[2]=((color>>16)&255)/255.f;
}
uint32_t Brush() {
    return IM_COL32(int(brush[0]*255+.5f),int(brush[1]*255+.5f),int(brush[2]*255+.5f),255);
}
void Load(unsigned nextSlot) {
    slot=nextSlot; spriteIndex=0; zoom=0; lastX=lastY=-1;
    history={};importedParts.clear();
    if (const auto* asset=Get(slot)) {edit=asset->applied;baseColor=asset->baseColor;standardColor=int(baseColor);}
    else edit={};
    selected=1;bank=0;fileNotice.clear();Swatch(1); DropPreview();
}
std::filesystem::path ChooseFile(bool save,bool full) {
    wchar_t path[32768]{};
    OPENFILENAMEW request{};request.lStructSize=sizeof(request);
    request.hwndOwner=static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
    request.lpstrFile=path;request.nMaxFile=std::size(path);
    request.lpstrFilter=full ? L"Extra color (*.cccolor)\0*.cccolor\0\0" : L"Adobe Color Table (*.act)\0*.act\0\0";
    request.lpstrDefExt=full ? L"cccolor" : L"act";
    request.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&request) : GetOpenFileNameW(&request)) ? std::filesystem::path(path) : std::filesystem::path{};
}
ExtraColor Package(const Asset& asset) {
    ExtraColor result{asset.character,baseColor,importedParts};
    for(unsigned i:{slot%2,slot%2+2})if(const auto* part=Get(i);part && part->character==asset.character) {
        auto found=std::find_if(result.parts.begin(),result.parts.end(),[&](const ExtraPart& saved){return saved.component==part->component;});
        if(found==result.parts.end())result.parts.push_back({part->component,part->layout,i==slot ? edit : part->applied});
        else if(i==slot)*found={part->component,part->layout,edit};
    }
    return result;
}
bool UsePackage(const Asset& asset,const ExtraColor& value) {
    if(value.character!=asset.character){fileNotice="This extra belongs to another character.";return false;}
    for(const auto& part:value.parts)if(part.component==asset.component) {
        if(part.layout!=asset.layout){fileNotice="Sprite layout differs from this character data.";return false;}
        history.Begin(edit);edit=part.edit;history.End(edit);baseColor=value.baseColor;standardColor=int(baseColor);importedParts=value.parts;
        dirty=true;Swatch(selected);fileNotice="Loaded. APPLY to use this color.";return true;
    }
    fileNotice="No data for this character component.";return false;
}
void Paint(const Asset& asset, const Sprite& sprite, int x, int y) {
    const auto key=asset.Locate(sprite,x,y);
    if (key==NoPixel) return;
    if (erase) edit.pixels.erase(key);
    else if (edit.pixels.size()<262144 || edit.pixels.count(key)) edit.pixels[key]=Brush();
    dirty=true;
}
void Stroke(const Asset& asset, const Sprite& sprite, int x, int y) {
    if (lastX<0) lastX=x,lastY=y;
    int dx=std::abs(x-lastX), sx=lastX<x ? 1 : -1, dy=-std::abs(y-lastY), sy=lastY<y ? 1 : -1, e=dx+dy;
    for (;;) {
        Paint(asset,sprite,lastX,lastY);
        if (lastX==x && lastY==y) break;
        const int twice=2*e;
        if(twice>=dy){e+=dy;lastX+=sx;}
        if(twice<=dx){e+=dx;lastY+=sy;}
    }
}
bool UpdatePreview(const Asset& asset, const Sprite& sprite) {
    if (!device) return false;
    if (!preview && FAILED(device->CreateTexture(sprite.width,sprite.height,1,0,D3DFMT_A8R8G8B8,
                                                  D3DPOOL_MANAGED,&preview,nullptr))) return false;
    if (!dirty) return true;
    D3DLOCKED_RECT lock{};
    if (FAILED(preview->LockRect(0,&lock,nullptr,0))) return false;
    for (int y=0;y<sprite.height;++y) for(int x=0;x<sprite.width;++x) {
        const auto color=asset.Color(edit,asset.Locate(sprite,x,y));
        reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch)[x]=
            (color&0xff00ff00)|((color&255)<<16)|((color>>16)&255);
    }
    const bool ok=SUCCEEDED(preview->UnlockRect(0)); dirty=!ok; return ok;
}
void PointFilter(const ImDrawList*, const ImDrawCmd*) {
    if (device) { device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT); device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT); }
}
}
void Prepare(IDirect3DDevice9* value) { device=value; ImGui::GetIO().MouseDrawCursor=Active(); }
void Release() { DropPreview(); device=nullptr; }
bool Draw() {
    if (!Active()) return false;
    if(dropNextFrame){DropPreview();dropNextFrame=false;}
    if (session!=Session()) { session=Session(); pencil=erase=false; Load(InitialSlot()); }
    hud::Canvas c;
    ImGui::GetIO().MouseDrawCursor=true;
    ImGui::SetNextWindowPos(c.At(6,6)); ImGui::SetNextWindowSize({c.S(628),c.S(468)});
    ImGui::PushFont(hud::Font(3,c.layout.scale));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{c.S(10),c.S(8)});
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,{c.S(4),c.S(3)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,{c.S(6),c.S(5)});
    ImGui::PushStyleColor(ImGuiCol_WindowBg,ImVec4(.025f,.04f,.085f,1));
    ImGui::Begin("COLOR PALETTE##training",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|
                 ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted("COLOR PALETTE / TRAINING");
    ImGui::SameLine(); ImGui::TextDisabled("  36 STANDARD + 6 EXTRA / CHARACTER");
    // 対象切替前の編集を不用意に失わない。適用・取消後に別の側を開く。
    const auto* asset=Get(slot);
    ImGui::BeginDisabled(asset && !(edit==asset->applied));
    for(unsigned i=0;i<4;++i) {
        if(i) ImGui::SameLine();
        ImGui::BeginDisabled(!Get(i));
        const char* names[]{"P1","P2","P1 PARTNER","P2 PARTNER"};
        if(ImGui::RadioButton(names[i],slot==i)) Load(i);
        ImGui::EndDisabled();
    }
    ImGui::EndDisabled();
    asset=Get(slot);
    ImGui::SameLine();
    if(ImGui::Button("CANCEL / B")) Cancel();
    if(ImGui::IsKeyPressed(ImGuiKey_Escape,false)) Cancel();
    if(!asset) {
        ImGui::TextWrapped("No editable indexed sprites are available for this character. Reload the character to try again.");
    } else {
        ImGui::SetNextItemWidth(c.S(105));
        const auto standardLabel="STANDARD "+std::to_string(standardColor+1);
        if(ImGui::BeginCombo("##standard",standardLabel.c_str())) {
            for(unsigned i=0;i<asset->standard.size();++i) {
                const auto label="COLOR "+std::to_string(i+1);
                if(ImGui::Selectable(label.c_str(),standardColor==int(i))) {
                    history.Begin(edit);edit.palette=asset->standard[i];history.End(edit);
                    standardColor=int(i);baseColor=i;bank=0;dirty=true;Swatch(selected);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();ImGui::SetNextItemWidth(c.S(91));
        ImGui::Combo("##extra",&extraSlot,"EXTRA 1\0EXTRA 2\0EXTRA 3\0EXTRA 4\0EXTRA 5\0EXTRA 6\0");
        ImGui::SameLine();if(ImGui::Button("LOAD EXTRA")) {
            ExtraColor value;if(LoadExtra(asset->character,extraSlot,value,fileNotice))UsePackage(*asset,value);
        }
        ImGui::SameLine();if(ImGui::Button("SAVE EXTRA")) {
            if(SaveExtra(asset->character,extraSlot,Package(*asset),fileNotice))fileNotice="Saved EXTRA "+std::to_string(extraSlot+1)+" for this character.";
        }
        ImGui::SameLine();if(ImGui::Button("IMPORT"))ImGui::OpenPopup("import format");
        if(ImGui::BeginPopup("import format")) {
            for(unsigned full=0;full<2;++full)if(ImGui::Selectable(full ? "Complete edit (.cccolor)" : "Palette (.act)",false,directSelected && !full ? ImGuiSelectableFlags_Disabled : 0)) {
                const auto path=ChooseFile(false,full);std::vector<uint8_t> bytes;
                if(!path.empty() && ReadColorFile(path,bytes,fileNotice)) {
                    if(full){ExtraColor value;if(DecodeExtra(bytes,value))UsePackage(*asset,value);else fileNotice="Invalid complete color file.";}
                    else {Palette colors;if(ReadAct(bytes,colors,fileNotice)) {
                        history.Begin(edit);auto* target=edit.Bank(bank);if(target)*target=colors;
                        history.End(edit);dirty=true;Swatch(selected);fileNotice="ACT imported into the selected palette. APPLY or SAVE EXTRA.";
                    }}
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();if(ImGui::Button("EXPORT"))ImGui::OpenPopup("export format");
        if(ImGui::BeginPopup("export format")) {
            for(unsigned full=0;full<2;++full)if(ImGui::Selectable(full ? "Complete edit (.cccolor)" : "Palette only (.act)",false,directSelected && !full ? ImGuiSelectableFlags_Disabled : 0)) {
                const auto path=ChooseFile(true,full);
                const auto* colors=edit.Bank(bank);
                if(!path.empty() && colors && WriteColorFile(path,full ? EncodeExtra(Package(*asset)) : WriteAct(*colors),fileNotice))
                    fileNotice=full ? "Complete edit exported." : "ACT exported: palette only; pencil and other objects are in .cccolor.";
            }
            ImGui::EndPopup();
        }
        if(ImGui::RadioButton("PALETTE",!pencil)) {history.End(edit);pencil=false;Swatch(selected);}
        ImGui::SameLine();
        if(ImGui::RadioButton("PIXEL PENCIL",pencil)) {history.End(edit);pencil=true;}
        ImGui::SameLine();
        if(ImGui::Button("UNDO")) {if(history.Undo(edit)){dirty=true;Swatch(selected);}}
        ImGui::SameLine();
        if(ImGui::Button("REDO")) {if(history.Redo(edit)){dirty=true;Swatch(selected);}}
        ImGui::SameLine();
        if(ImGui::Button("RESET ALL")) {history.Begin(edit);edit=asset->original;history.End(edit);importedParts.clear();dirty=true;Swatch(selected);}
        ImGui::SameLine();
        if(ImGui::Button("APPLY")) {history.End(edit);ApplyPackage(slot,Package(*asset));}

        ImGui::BeginChild("images",{c.S(396),c.S(308)},false,ImGuiWindowFlags_NoScrollbar);
        const auto changeSprite=[&](int delta) {
            history.End(edit); spriteIndex=(spriteIndex+asset->sprites.size()+delta)%asset->sprites.size();
            bank=asset->sprites[spriteIndex].bank;Swatch(selected);DropPreview();
        };
        if(ImGui::Button("<")) changeSprite(-1);
        ImGui::SameLine(); ImGui::SetNextItemWidth(c.S(258));
        const auto label=std::to_string(asset->sprites[spriteIndex].id)+" / "+asset->sprites[spriteIndex].name;
        if(ImGui::BeginCombo("##pose",label.c_str())) {
            for(unsigned i=0;i<asset->sprites.size();++i) {
                const auto name=std::to_string(asset->sprites[i].id)+" / "+asset->sprites[i].name;
                if(ImGui::Selectable(name.c_str(),spriteIndex==i)) {history.End(edit);spriteIndex=i;bank=asset->sprites[i].bank;Swatch(selected);DropPreview();}
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();if(ImGui::Button(">")) changeSprite(1);
        ImGui::SetNextItemWidth(c.S(90)); ImGui::Combo("ZOOM",&zoom,"FIT\0 1x\0 2x\0 4x\0 8x\0");
        if(pencil) { ImGui::SameLine(); ImGui::Checkbox("RESTORE PIXEL",&erase); }
        const auto& sprite=asset->sprites[spriteIndex];
        ImGui::BeginChild("canvas",{c.S(386),c.S(245)},true,ImGuiWindowFlags_HorizontalScrollbar);
        const auto available=ImGui::GetContentRegionAvail();
        const float scale=zoom ? c.S(float(1u<<(zoom-1))) : (std::min)(available.x/sprite.width,available.y/sprite.height);
        const ImVec2 extent{sprite.width*scale,sprite.height*scale}, origin=ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("sprite",extent,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
        auto* draw=ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin,{origin.x+extent.x,origin.y+extent.y},IM_COL32(30,33,40,255));
        if(UpdatePreview(*asset,sprite)) {
            draw->AddCallback(PointFilter,nullptr);
            draw->AddImage(reinterpret_cast<ImTextureID>(preview),origin,{origin.x+extent.x,origin.y+extent.y});
            draw->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
        }
        const auto mouse=ImGui::GetIO().MousePos;
        const int x=int(std::floor((mouse.x-origin.x)/scale)), y=int(std::floor((mouse.y-origin.y)/scale));
        const bool hovered=ImGui::IsItemHovered();
        if(hovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || (!pencil && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
            history.End(edit); SelectColor(*asset,asset->Locate(sprite,x,y));
        }
        const bool down=ImGui::IsMouseDown(ImGuiMouseButton_Left), released=ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        if(pencil && (ImGui::IsItemActive() || lastX>=0) && (down || released) && hovered) {
            history.Begin(edit);Stroke(*asset,sprite,x,y);
        } else {lastX=lastY=-1;}
        if(released) {history.End(edit);lastX=lastY=-1;}
        if(hovered && asset->Locate(sprite,x,y)!=NoPixel) {
            draw->AddRect({origin.x+x*scale,origin.y+y*scale},{origin.x+(x+1)*scale,origin.y+(y+1)*scale},IM_COL32_WHITE);
            if(!ImGui::IsMouseDown(ImGuiMouseButton_Left)) ImGui::SetTooltip("%d, %d / color %d",x,y,asset->Index(asset->Locate(sprite,x,y)));
        }
        ImGui::EndChild(); ImGui::EndChild(); ImGui::SameLine();
        ImGui::BeginChild("colors",{0,c.S(308)},false,ImGuiWindowFlags_NoScrollbar);
        ImGui::SetNextItemWidth(c.S(161));
        const auto bankLabel=bank ? "OBJECT "+std::to_string(bank) : "CHARACTER PALETTE";
        if(ImGui::BeginCombo("##bank",directSelected ? "RGB OBJECT" : bankLabel.c_str())) {
            if(ImGui::Selectable("CHARACTER PALETTE",!bank)){bank=0;Swatch(selected);}
            for(const auto& [id,colors]:edit.effects)if(ImGui::Selectable(("OBJECT "+std::to_string(id)).c_str(),bank==id)) {
                bank=id;Swatch(selected);
                // 色表を選ぶと、その色を使う画像も表示して編集対象を分かるようにする。
                for(unsigned i=0;i<asset->sprites.size();++i)if(asset->sprites[i].bank==id){spriteIndex=i;dropNextFrame=true;break;}
            }
            ImGui::EndCombo();
        }
        ImGui::Text("COLOR %03u / #%06X",selected,((Brush()&255)<<16)|(Brush()&0xff00)|((Brush()>>16)&255));
        const auto start=ImGui::GetCursorScreenPos();
        auto* grid=ImGui::GetWindowDrawList();
        const float cell=c.S(8);
        ImGui::InvisibleButton("swatches",{cell*16,cell*16});
        for(unsigned i=0;i<256;++i) {
            const ImVec2 p{start.x+(i%16)*cell,start.y+(i/16)*cell};
            const auto* colors=edit.Bank(bank);
            grid->AddRectFilled(p,{p.x+cell-1,p.y+cell-1},(colors ? (*colors)[i] : 0)|0xff000000);
            if(i==selected) grid->AddRect(p,{p.x+cell,p.y+cell},IM_COL32_WHITE,0,0,c.S(1.5f));
        }
        if(ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            history.End(edit);
            const auto pos=ImGui::GetIO().MousePos;
            Swatch((std::min)(255u,unsigned((pos.x-start.x)/cell)+16*unsigned((pos.y-start.y)/cell)));
        }
        ImGui::BeginDisabled(!pencil && !directSelected && !bank && selected==0);
        ImGui::SetNextItemWidth(c.S(128));
        // Beginは変更前に呼び、ドラッグ1回を1履歴として扱う。
        const bool changed=ImGui::ColorPicker3("##brush",brush,ImGuiColorEditFlags_NoSidePreview|
            ImGuiColorEditFlags_NoSmallPreview|ImGuiColorEditFlags_NoAlpha|ImGuiColorEditFlags_NoInputs);
        ImGui::SetNextItemWidth(c.S(161));
        const bool inputChanged=ImGui::ColorEdit3("##rgb",brush,ImGuiColorEditFlags_NoSmallPreview|
            ImGuiColorEditFlags_NoPicker|ImGuiColorEditFlags_DisplayHex|ImGuiColorEditFlags_Uint8);
        if((changed||inputChanged) && !pencil) {
            // 必ず変更前の編集全体を保存する。鉛筆の色選びは履歴へ含めない。
            history.Begin(edit);
            if(directSelected)edit.replacements[directColor]=Brush()&0xffffff;
            else if(auto* colors=edit.Bank(bank))(*colors)[selected]=Brush();
            dirty=true;
        }
        if(!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) history.End(edit);
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::TextUnformatted(pencil ? "Drag: paint pixel  /  Right click: eyedropper  /  Scroll: pan" :
                                         "Click sprite or swatch, then choose a color. All poses update.");
        ImGui::TextDisabled("Shared tiles change together. ACT: one palette. Complete edits: .cccolor / SAVE EXTRA.");
        if(!fileNotice.empty())ImGui::TextWrapped("%s",fileNotice.c_str());
        if(*Error()) ImGui::TextColored(ImVec4(1,.65f,.3f,1),"%s",Error());
    }
    ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar(3); ImGui::PopFont();
    if(!Active()) ImGui::GetIO().MouseDrawCursor=false;
    return true;
}
}
