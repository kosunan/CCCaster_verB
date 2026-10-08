#include "core_dll/ui/TrainingPaletteView.hpp"
#include "core_dll/ui/HudTheme.hpp"
#include "core_dll/mbaa_mem/TrainingPaletteMenu.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/engine/ExtraColorStore.hpp"
#include "core_dll/engine/TrainingPaletteMotion.hpp"
#include "core_dll/mbaa_mem/ExtraColorSelection.hpp"
#include <commdlg.h>
#include <cmath>

namespace cccaster::domain::ui::training_palette_view {
namespace {
using namespace training_palette;
IDirect3DDevice9* device = nullptr;
IDirect3DTexture9* preview = nullptr;
IDirect3DTexture9* animationPreview = nullptr;
unsigned motionIndex=0, animationSprite=NoPixel;
bool stillDirty=true,animationDirty=true;
int motionFilter=0, speedIndex=2;
MotionPlayer player;
unsigned session = 0, slot = 0, spriteIndex = 0, selected = 1;
unsigned bank=0, baseColor=0;
int extraSlot=0, standardColor=0;
uint32_t directColor=0;
bool directSelected=false;
std::string fileNotice;
std::vector<ExtraPart> importedParts;
bool pencil = false, dirty = true, erase = false;
bool draftDirty=false;
uint64_t loadedGeneration=0;
bool dropNextFrame=false;
int zoom = 0, lastX = -1, lastY = -1;
float brush[3]{1,0,0};
Edit edit;
History history;
void DropPreview() { if (preview) preview->Release(); preview = nullptr; dirty = true; }
void DropAnimation() { if(animationPreview)animationPreview->Release();animationPreview=nullptr;animationSprite=NoPixel;animationDirty=true; }
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
    motionIndex=0;motionFilter=0;speedIndex=2;player.Reset();DropAnimation();
    history={};importedParts.clear();
    if (const auto* asset=Get(slot)) {
        edit=asset->applied;baseColor=asset->baseColor;loadedGeneration=asset->generation;
        if(const auto* pending=Pending(slot)) {
            importedParts=pending->parts;baseColor=pending->baseColor;
            for(const auto& part:pending->parts)if(part.component==asset->component)edit=part.edit;
        }
        standardColor=int(baseColor);
    }
    else {edit={};loadedGeneration=0;}
    draftDirty=false;
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
    if(result.parts.empty())if(const auto* pending=Pending(slot))result.parts=pending->parts;
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
        dirty=draftDirty=true;Swatch(selected);fileNotice="Loaded. Changes apply when the editor closes.";return true;
    }
    fileNotice="No data for this character component.";return false;
}
void Paint(const Asset& asset, const Sprite& sprite, int x, int y) {
    const auto key=asset.Locate(sprite,x,y);
    if (key==NoPixel) return;
    if (erase) edit.pixels.erase(key);
    else if (edit.pixels.size()<262144 || edit.pixels.count(key)) edit.pixels[key]=Brush();
    dirty=draftDirty=true;
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
bool UpdatePreview(const Asset& asset, const Sprite& sprite,IDirect3DTexture9*& texture,bool& needsUpdate) {
    if (!device) return false;
    if (!texture) {
        needsUpdate=true;
        if(FAILED(device->CreateTexture(sprite.width,sprite.height,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&texture,nullptr)))return false;
    }
    if (!needsUpdate) return true;
    D3DLOCKED_RECT lock{};
    if (FAILED(texture->LockRect(0,&lock,nullptr,0))) return false;
    for (int y=0;y<sprite.height;++y) for(int x=0;x<sprite.width;++x) {
        const auto color=asset.Color(edit,asset.Locate(sprite,x,y));
        reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lock.pBits)+y*lock.Pitch)[x]=
            (color&0xff00ff00)|((color&255)<<16)|((color>>16)&255);
    }
    const bool ok=SUCCEEDED(texture->UnlockRect(0)); needsUpdate=!ok; return ok;
}
void PointFilter(const ImDrawList*, const ImDrawCmd*) {
    if (device) { device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT); device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT); }
}
bool MotionVisible(const Motion& motion){return !motionFilter || motion.object==(motionFilter==2);}
void SelectMotion(unsigned index) {
    motionIndex=index;player.Reset();
    if(const auto* asset=Get(slot);asset && index<asset->motions.size())
        domain::session::DebugLog("[TrainingPalette] PREVIEW slot=%u motion=%u object=%u frames=%u duration=%u",slot,
            asset->motions[index].id,unsigned(asset->motions[index].object),unsigned(asset->motions[index].frames.size()),asset->motions[index].duration);
}
void Animation(const Asset& asset,const hud::Canvas& c) {
    ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("ANIMATION");ImGui::SameLine();ImGui::SetNextItemWidth(c.S(98));
    if(ImGui::Combo("##motionFilter",&motionFilter,"ALL\0CHARACTER\0OBJECT\0")) {
        motionIndex=NoPixel;
        for(unsigned i=0;i<asset.motions.size();++i)if(MotionVisible(asset.motions[i])){SelectMotion(i);break;}
    }
    const auto* motion=motionIndex<asset.motions.size() ? &asset.motions[motionIndex] : nullptr;
    ImGui::SetNextItemWidth(-1);
    const auto label=motion ? std::to_string(motion->id)+" / "+motion->name : "No motion available";
    if(ImGui::BeginCombo("##motion",label.c_str())) {
        for(unsigned i=0;i<asset.motions.size();++i)if(MotionVisible(asset.motions[i])) {
            const auto& item=asset.motions[i];
            const auto name=std::to_string(item.id)+" / "+item.name;
            if(ImGui::Selectable(name.c_str(),motionIndex==i))SelectMotion(i);
            if(motionIndex==i)ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    motion=motionIndex<asset.motions.size() ? &asset.motions[motionIndex] : nullptr;
    ImGui::BeginDisabled(!motion);
    if(ImGui::Button(player.playing ? "PAUSE" : "PLAY",{c.S(43),0}))player.playing=!player.playing;
    ImGui::SameLine();if(ImGui::Button("<F"))player.Step(-1,motion ? motion->duration : 0);
    ImGui::SameLine();if(ImGui::Button("F>"))player.Step(1,motion ? motion->duration : 0);
    ImGui::SameLine();ImGui::SetNextItemWidth(c.S(48));ImGui::Combo("##speed",&speedIndex,"0.25x\0 0.5x\0 1x\0 2x\0");
    int frame=int(player.Frame());ImGui::SetNextItemWidth(-1);
    if(ImGui::SliderInt("##frame",&frame,0,motion ? int(motion->duration-1) : 0,"FRAME %d")) {
        player.playing=false;player.Seek(unsigned(frame),motion ? motion->duration : 0);
    }
    ImGui::EndDisabled();
    ImGui::BeginChild("animation canvas",{0,0},true,ImGuiWindowFlags_NoScrollbar|ImGuiWindowFlags_NoScrollWithMouse);
    if(motion) {
        const auto frameIndex=MotionFrameIndex(*motion,player.Frame());
        const auto& current=motion->frames[frameIndex];
        if(std::getenv("CCCASTER_PALETTE_PREVIEW_TRACE")) {
            static std::array<unsigned,7> last{};
            const std::array<unsigned,7> now{session,slot,motion->id,player.Frame(),spriteIndex,unsigned(player.playing),unsigned(dirty || stillDirty || animationDirty)};
            if(now!=last)domain::session::DebugLog("[PaletteAnimation] slot=%u motion=%u object=%u frame=%u state=%u sprite=%u still=%u playing=%u",slot,
                motion->id,unsigned(motion->object),player.Frame(),current.state,current.sprite<asset.sprites.size() ? asset.sprites[current.sprite].id : NoPixel,
                asset.sprites[spriteIndex].id,unsigned(player.playing));
            last=now;
        }
        const auto available=ImGui::GetContentRegionAvail(),origin=ImGui::GetCursorScreenPos();
        auto* draw=ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin,{origin.x+available.x,origin.y+available.y},IM_COL32(30,33,40,255));
        if(current.sprite<asset.sprites.size()) {
            if(animationSprite!=current.sprite){DropAnimation();animationSprite=current.sprite;}
            const auto& sprite=asset.sprites[current.sprite];
            const float width=(std::max)(1.f,motion->right-motion->left),height=(std::max)(1.f,motion->bottom-motion->top);
            const float scale=(std::min)(available.x/width,available.y/height);
            auto quad=MotionQuad(sprite,current);ImVec2 points[4];
            for(unsigned i=0;i<4;++i)points[i]={origin.x+(available.x-width*scale)/2+(quad[i].x-motion->left)*scale,
                origin.y+(available.y-height*scale)/2+(quad[i].y-motion->top)*scale};
            if(UpdatePreview(asset,sprite,animationPreview,animationDirty)) {
                draw->AddCallback(PointFilter,nullptr);
                draw->AddImageQuad(reinterpret_cast<ImTextureID>(animationPreview),points[0],points[1],points[2],points[3]);
                draw->AddCallback(ImDrawCallback_ResetRenderState,nullptr);
            }
        } else ImGui::TextWrapped("No editable image in this frame.");
        if(ImGui::IsWindowHovered())ImGui::SetTooltip("Motion %u / state %u / %u frames%s",motion->id,current.state,motion->duration,
            motion->missing ? " / some images unavailable" : "");
        constexpr double speeds[]{.25,.5,1,2};
        player.Advance(ImGui::GetIO().DeltaTime,speeds[speedIndex],motion->duration);
    } else ImGui::TextWrapped("No motion uses the loaded images in this category.");
    ImGui::EndChild();
}
}
void Prepare(IDirect3DDevice9* value) { device=value; ImGui::GetIO().MouseDrawCursor=Active(); }
void Release() { DropPreview();DropAnimation();device=nullptr; }
bool Draw() {
    if (!Active()) return false;
    if(dropNextFrame){DropPreview();dropNextFrame=false;}
    if (session!=Session()) {
        session=Session();
        // B・Escape・F4で閉じても、同じ読込み資産なら編集中の内容とUndoを保持する。
        // アップロード失敗時も下書きを捨てず、開き直した際に再試行できる。
        const auto* retained=Get(slot);
        if(!retained || retained->generation!=loadedGeneration){pencil=erase=false;Load(InitialSlot());}
        history.End(edit);lastX=lastY=-1;
    }
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
    const auto* asset=Get(slot);
    for(unsigned i=0;i<4;++i) {
        if(i) ImGui::SameLine();
        ImGui::BeginDisabled(!Get(i));
        const char* names[]{"P1","P2","P1 PARTNER","P2 PARTNER"};
        if(ImGui::RadioButton(names[i],slot==i)) Load(i);
        ImGui::EndDisabled();
    }
    asset=Get(slot);
    ImGui::SameLine();
    const bool closeRequested=ImGui::Button("CLOSE / B") || ImGui::IsKeyPressed(ImGuiKey_Escape,false);
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
                    standardColor=int(i);baseColor=i;bank=0;dirty=draftDirty=true;Swatch(selected);
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
                        history.End(edit);dirty=draftDirty=true;Swatch(selected);fileNotice="ACT imported. Changes apply when the editor closes. SAVE EXTRA to keep for other matches.";
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
        if(ImGui::Button("UNDO")) {if(history.Undo(edit)){dirty=draftDirty=true;Swatch(selected);}}
        ImGui::SameLine();
        if(ImGui::Button("REDO")) {if(history.Redo(edit)){dirty=draftDirty=true;Swatch(selected);}}
        ImGui::SameLine();
        if(ImGui::Button("RESET ALL")) {history.Begin(edit);edit=asset->original;history.End(edit);importedParts.clear();dirty=draftDirty=true;Swatch(selected);}
        ImGui::SameLine();
        ImGui::TextDisabled("APPLY ON CLOSE");

        if(dirty){stillDirty=animationDirty=true;dirty=false;}
        ImGui::BeginChild("images",{c.S(396),c.S(308)},false,ImGuiWindowFlags_NoScrollbar);
        ImGui::BeginChild("animation",{c.S(192),0},false,ImGuiWindowFlags_NoScrollbar);
        Animation(*asset,c);
        ImGui::EndChild();ImGui::SameLine();
        ImGui::BeginChild("still",{0,0},false,ImGuiWindowFlags_NoScrollbar);
        ImGui::AlignTextToFramePadding();ImGui::TextUnformatted("STILL / EDIT");
        const auto changeSprite=[&](int delta) {
            history.End(edit); spriteIndex=(spriteIndex+asset->sprites.size()+delta)%asset->sprites.size();
            bank=asset->sprites[spriteIndex].bank;Swatch(selected);DropPreview();
        };
        if(ImGui::Button("<")) changeSprite(-1);
        ImGui::SameLine(); ImGui::SetNextItemWidth(c.S(134));
        const auto label=std::to_string(asset->sprites[spriteIndex].id)+" / "+asset->sprites[spriteIndex].name;
        if(ImGui::BeginCombo("##pose",label.c_str())) {
            for(unsigned i=0;i<asset->sprites.size();++i) {
                const auto name=std::to_string(asset->sprites[i].id)+" / "+asset->sprites[i].name;
                if(ImGui::Selectable(name.c_str(),spriteIndex==i)) {history.End(edit);spriteIndex=i;bank=asset->sprites[i].bank;Swatch(selected);DropPreview();}
                if(spriteIndex==i)ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();if(ImGui::Button(">")) changeSprite(1);
        ImGui::SetNextItemWidth(c.S(64)); ImGui::Combo("##zoom",&zoom,"FIT\0 1x\0 2x\0 4x\0 8x\0");
        if(pencil) { ImGui::SameLine(); ImGui::Checkbox("RESTORE",&erase); }
        const auto& sprite=asset->sprites[spriteIndex];
        ImGui::AlignTextToFramePadding();ImGui::Text("IMAGE %u / %d x %d",sprite.id,sprite.width,sprite.height);
        ImGui::BeginChild("canvas",{0,0},true,ImGuiWindowFlags_HorizontalScrollbar);
        const auto available=ImGui::GetContentRegionAvail();
        const float scale=zoom ? c.S(float(1u<<(zoom-1))) : (std::min)(available.x/sprite.width,available.y/sprite.height);
        const ImVec2 extent{sprite.width*scale,sprite.height*scale}, origin=ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("sprite",extent,ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);
        auto* draw=ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin,{origin.x+extent.x,origin.y+extent.y},IM_COL32(30,33,40,255));
        if(UpdatePreview(*asset,sprite,preview,stillDirty)) {
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
        ImGui::EndChild(); ImGui::EndChild();ImGui::EndChild(); ImGui::SameLine();
        ImGui::BeginChild("colors",{0,c.S(308)},false,ImGuiWindowFlags_NoScrollbar);
        ImGui::SetNextItemWidth(c.S(161));
        const auto bankLabel=bank ? "OBJECT "+std::to_string(bank) : "CHARACTER PALETTE";
        if(ImGui::BeginCombo("##bank",directSelected ? "RGB OBJECT" : bankLabel.c_str())) {
            if(ImGui::Selectable("CHARACTER PALETTE",!bank)){bank=0;Swatch(selected);}
            for(const auto& [id,colors]:edit.effects)if(ImGui::Selectable(("OBJECT "+std::to_string(id)).c_str(),bank==id)) {
                bank=id;Swatch(selected);
                // 色表を選ぶと、その色を使う画像も表示して編集対象を分かるようにする。
                for(unsigned i=0;i<asset->sprites.size();++i)if(asset->sprites[i].bank==id){spriteIndex=i;dropNextFrame=true;break;}
                for(unsigned i=0;i<asset->motions.size();++i) {
                    const auto& frames=asset->motions[i].frames;
                    if(std::any_of(frames.begin(),frames.end(),[&](const MotionFrame& f){return f.sprite<asset->sprites.size() && asset->sprites[f.sprite].bank==id;})) {
                        motionFilter=0;SelectMotion(i);break;
                    }
                }
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
            dirty=draftDirty=true;
        }
        if(!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsAnyItemActive()) history.End(edit);
        ImGui::EndDisabled();
        ImGui::EndChild();
        if(draftDirty) { StagePackage(slot,Package(*asset));draftDirty=false; }
        ImGui::TextUnformatted(pencil ? "Drag: paint pixel  /  Right click: eyedropper  /  Scroll: pan" :
                                         "Click sprite or swatch, then choose a color. All poses update.");
        ImGui::TextDisabled("Closing applies edits. SAVE EXTRA / EXPORT to keep after character reload or exit.");
        if(!fileNotice.empty())ImGui::TextWrapped("%s",fileNotice.c_str());
        if(*Error()) ImGui::TextColored(ImVec4(1,.65f,.3f,1),"%s",Error());
    }
    ImGui::End(); ImGui::PopStyleColor(); ImGui::PopStyleVar(3); ImGui::PopFont();
    if(closeRequested) {history.End(edit);Close();}
    if(!Active()) ImGui::GetIO().MouseDrawCursor=false;
    return true;
}
}
