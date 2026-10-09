#include "core_dll/hook/HookBatch.hpp"
#include "core_dll/mbaa_mem/TrainingCharacterMenu.hpp"
#include "core_dll/mbaa_mem/TrainingPaletteMenu.hpp"
#include "core_dll/mbaa_mem/TrainingHitboxMenu.hpp"
#include "core_dll/ui/HudDisplay.hpp"
#include "core_dll/mbaa_mem/RealGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/MbaaInputDefs.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/common/Platform.hpp"
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <cstring>
#include <cstdio>

// ABIは test/logs/mbaa_ghidra_20261006/assembly/ の同名アドレスと照合。
// Ghidraが省略するレジスタ引数をC++の推定シグネチャで呼ばない。
extern "C" {
__attribute__((naked)) void cc_training_append(void* vector, void* item) {
    __asm__ __volatile__("pushl %esi; pushl %ebx; movl 12(%esp),%esi; leal 16(%esp),%ebx;"
                         "movl $0x42BA50,%eax; call *%eax; popl %ebx; popl %esi; ret");
}
__attribute__((naked)) void cc_training_describe(void* description, void* loading) {
    __asm__ __volatile__("pushl %edi; movl 8(%esp),%eax; movl 12(%esp),%edi;"
                         "movl $0x449030,%ecx; call *%ecx; popl %edi; ret");
}
__attribute__((naked)) void cc_training_free_slot(unsigned slot) {
    __asm__ __volatile__("movl 4(%esp),%eax; movl $0x41C2E0,%ecx; jmp *%ecx");
}
__attribute__((naked)) void cc_training_append_information(void* vector, void* entry) {
    // 0x4DAA30: ESI=説明のvector、スタック=文字列ペア、ret 4。文字列を深くコピーする。
    __asm__ __volatile__("pushl %esi; movl 8(%esp),%esi; pushl 12(%esp);"
                         "movl $0x4DAA30,%eax; call *%eax; popl %esi; ret");
}
__attribute__((naked)) void cc_training_load_presentation(unsigned slot, unsigned character, unsigned side, unsigned cutin) {
    // 0x426340: ECX=slot, EDX=画像用キャラ、スタック=side/CUT番号、呼出側で8バイト解放。
    // 元関数が同じslotの旧画像を解放する。HA6/PAT/CG/PALには触れない。
    __asm__ __volatile__("movl 4(%esp),%ecx; movl 8(%esp),%edx; pushl 16(%esp); pushl 16(%esp);"
                         "movl $0x426340,%eax; call *%eax; addl $8,%esp; ret");
}
__attribute__((naked)) uint32_t cc_training_file_size(void* file) {
    __asm__ __volatile__("pushl %esi; movl 8(%esp),%esi; movl $0x413DB0,%eax;"
                         "call *%eax; popl %esi; ret");
}
__attribute__((naked)) bool cc_training_file_exists(const char* path) {
    // 0x4DB9B0: ESI=アーカイブ一覧、スタックにパス、ret 4。
    __asm__ __volatile__("pushl %esi; movl $0x76E9C4,%esi; pushl 8(%esp);"
                         "movl $0x4DB9B0,%eax; call *%eax; popl %esi; ret");
}
}

namespace cccaster::game_interface { bool ConfigureMenuObserver(); }
namespace cccaster::training_character {
namespace {
using domain::session::DebugLog;
Selection selection;
bool installed = false, pending = false, restartDispatched = false, changed = false, suppressUntilRelease = false;
uint32_t* mainMenu = nullptr;
uint32_t* menuSet = nullptr;
uint32_t previousButtons = 0;
uint16_t previousDirection = 0;
int64_t repeatAt = 0;
const char* error = "";
using Constructor = uint32_t* (__thiscall*)(uint32_t*);
using Reset = void (__stdcall*)(void*);
Constructor originalConstructor = nullptr;
using DisplayConstructor = uint32_t* (__stdcall*)(uint32_t*);
DisplayConstructor originalDisplayConstructor = nullptr;
bool lastFrameBarValue = false;
Reset originalReset = nullptr;

uint32_t* Descriptor(uint32_t character) {
    auto* table = *reinterpret_cast<uint32_t**>(0x55DF18);
    if (!table || character >= table[3]) return nullptr;
    return reinterpret_cast<uint32_t*>(table[0] == 0 ? table[1] + table[2] * character
                                                     : reinterpret_cast<uint32_t*>(table[1])[character]);
}
const char* NativeString(uint32_t* base) {
    return base[6] < 16 ? reinterpret_cast<const char*>(base + 1)
                        : reinterpret_cast<const char*>(base[1]);
}
void AddInformation(uint32_t* menu, const char* key, const char* text) {
    auto* information = reinterpret_cast<uint8_t*>(menu[0xDC/4]);
    if (!information) return;
    // Information\\Training.iniの読込みと同じ文字列形式・アロケータを使用する。
    // 元のINIは変更せず、メニューが所有する説明一覧へ登録する。
    uint32_t entry[14]{};
    entry[6] = entry[13] = 15;
    const auto assign = reinterpret_cast<void (__thiscall*)(void*, const char*, size_t)>(0x407C10);
    assign(entry, key, std::strlen(key));
    assign(entry + 7, text, std::strlen(text));
    cc_training_append_information(information + 0x8C, entry);
    for (auto* value : {entry, entry + 7})
        if (value[6] >= 16) reinterpret_cast<void (__cdecl*)(void*)>(0x4E02F3)(reinterpret_cast<void*>(value[1]));
}
bool Exists(const char* path) {
    const auto attributes = GetFileAttributesA(path);
    return (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) ||
           cc_training_file_exists(path);
}
bool Available(Choice choice) {
    auto* info = Descriptor(choice.character);
    if (!info || choice.moon != MoonValue(choice.character,MoonSlot(choice.moon))) return false;
    const auto* first = reinterpret_cast<const char*>(info) + 0x34;
    const auto* partner = first + 0x20;
    char path[128]{};
    std::snprintf(path, sizeof(path), ".\\data\\%.31s_%u.txt", first, choice.moon);
    if (!Exists(path)) return false;
    if (*partner && *partner != '0') {
        std::snprintf(path, sizeof(path), ".\\data\\%.31s_%u.txt", partner, choice.moon);
        if (!Exists(path)) return false;
    }
    return true;
}
uint32_t FindItem(uint32_t* set, const char* key) {
    auto** begin = reinterpret_cast<uint32_t**>(set[0x4c/4]);
    auto** end = reinterpret_cast<uint32_t**>(set[0x50/4]);
    if (!begin || end < begin || end - begin > 64) return UINT32_MAX;
    for (auto** item = begin; item != end; ++item)
        if (*item && std::strcmp(NativeString(*item + 0x3c/4), key) == 0) return uint32_t(item-begin);
    return UINT32_MAX;
}
__attribute__((force_align_arg_pointer)) uint32_t* __fastcall Construct(uint32_t* self, void*) {
    auto* result = originalConstructor(self);
    mainMenu = result;
    menuSet = nullptr;
    selection.open = false;
    if (!result || !result[4] || result[5] <= result[4]) return result;
    auto* set = *reinterpret_cast<uint32_t**>(result[4]);
    if (!set || set[0] != 0x53873C) return result;
    auto* item = static_cast<uint32_t*>(reinterpret_cast<void* (__cdecl*)(size_t)>(0x4E0177)(0x58));
    if (!item) return result;
    reinterpret_cast<void* (__stdcall*)(void*, const char*, const char*, int)>(0x429140)(
        item, "CHARACTER", "CC_CHARACTER", 0);
    item[0] = 0x53604C; item[1] = item[3] = 1;
    cc_training_append(set + 0x48/4, item);
    AddInformation(result, "CC_CHARACTER", "Change the character and Moon style for P1 or P2.");
    auto** begin = reinterpret_cast<uint32_t**>(set[0x4c/4]);
    auto** end = reinterpret_cast<uint32_t**>(set[0x50/4]);
    std::rotate(begin, end - 1, end);
    auto* paletteItem = static_cast<uint32_t*>(reinterpret_cast<void* (__cdecl*)(size_t)>(0x4E0177)(0x58));
    if (paletteItem) {
        reinterpret_cast<void (__stdcall*)(void*, const char*, const char*, int)>(0x429140)(
            paletteItem, "COLOR PALETTE", "CC_PALETTE", 0);
        paletteItem[0] = 0x53604C; paletteItem[1] = paletteItem[3] = 1;
        cc_training_append(set + 0x48/4, paletteItem);
        AddInformation(result, "CC_PALETTE", "Edit character colors and save or load palettes.");
        begin = reinterpret_cast<uint32_t**>(set[0x4c/4]);
        end = reinterpret_cast<uint32_t**>(set[0x50/4]);
        std::rotate(begin + 1, end - 1, end);
    }
    set[0x40/4] = set[0x44/4] = 0;
    // サブメニューの説明も主メニューのInformationがTD_接頭辞で引く。
    AddInformation(result, "TD_CC_FRAME_BAR", "Show or hide the frame bar. F1 also toggles this setting.");
    AddInformation(result, "TD_CC_HITBOX", "Show or hide hitboxes and other collision boxes.");
    menuSet = set;
    error = "";
    // 起動済みのアーカイブ索引で確認。描画中や毎フレームの探索は行わない。
    for (unsigned i = 0; i < Characters.size(); ++i) {
        selection.available[i] = 0;
        for (unsigned moon = 0; moon < MoonCount; ++moon)
            if (Available({Characters[i],MoonValue(Characters[i],moon)})) selection.available[i] |= 1u << moon;
    }
    static bool loggedCatalogue = false;
    if (!loggedCatalogue) {
        for (unsigned i = 0; i < Characters.size(); ++i)
            DebugLog("[TrainingCharacter] OPTION char=%u styles=%u",Characters[i],selection.available[i]);
        loggedCatalogue = true;
    }
    DebugLog("[TrainingCharacter] MENU added count=%u", unsigned(end-begin));
    return result;
}
__attribute__((force_align_arg_pointer)) uint32_t* __stdcall ConstructDisplay(uint32_t* self) {
    auto* result = originalDisplayConstructor(self);
    if (!result || !result[4] || result[5] <= result[4]) return result;
    auto* set = *reinterpret_cast<uint32_t**>(result[4]);
    if (!set || set[0] != 0x5388F0) return result;
    const auto allocate = reinterpret_cast<void* (__cdecl*)(size_t)>(0x4E0177);
    auto* bar = static_cast<uint32_t*>(allocate(0x70));
    auto* off = allocate(0x3C);
    auto* on = allocate(0x3C);
    if (bar && off && on) {
        // 0x480810と同じSelectElement。0x42F8F0はECX=表示名、スタック=item/key/幅、ret 12。
        reinterpret_cast<void* (__thiscall*)(const char*, void*, const char*, int)>(0x42F8F0)(
            "FRAME BAR", bar, "CC_FRAME_BAR", 0xA0);
        bar[0] = 0x536654;
        const auto choice = reinterpret_cast<void* (__stdcall*)(void*, const char*, const char*, int)>(0x42F600);
        const auto append = reinterpret_cast<void (__thiscall*)(void*, void*)>(reinterpret_cast<uint32_t*>(bar[0])[0x44/4]);
        append(bar, choice(off, "OFF", "OFF", 0));
        append(bar, choice(on, "ON", "ON", 1));
        lastFrameBarValue = domain::ui::FrameBarDisplay::Visible(1);
        bar[0x58/4] = unsigned(lastFrameBarValue);
        cc_training_append(set + 0x48/4, bar);
        auto** begin = reinterpret_cast<uint32_t**>(set[0x4C/4]);
        auto** end = reinterpret_cast<uint32_t**>(set[0x50/4]);
        std::rotate(begin + 2, end - 1, end);
    } else {
        const auto release = reinterpret_cast<void (__cdecl*)(void*)>(0x4E02F3);
        release(bar); release(off); release(on);
    }
    auto* hitbox = static_cast<uint32_t*>(allocate(0x58));
    if (hitbox) {
        reinterpret_cast<void (__stdcall*)(void*, const char*, const char*, int)>(0x429140)(
            hitbox, "HITBOX", "CC_HITBOX", 0);
        hitbox[0] = 0x53604C; hitbox[1] = hitbox[3] = 1;
        cc_training_append(set + 0x48/4, hitbox);
        auto** begin = reinterpret_cast<uint32_t**>(set[0x4C/4]);
        auto** end = reinterpret_cast<uint32_t**>(set[0x50/4]);
        std::rotate(begin + (FindItem(set, "CC_FRAME_BAR") != UINT32_MAX ? 3 : 2), end - 1, end);
    }
    DebugLog("[TrainingDisplay] MENU frameBar=%u", unsigned(domain::ui::FrameBarDisplay::Visible(1)));
    return result;
}
bool ObserveDisplay(uint32_t* menu, uint32_t* command) {
    auto* parent = *reinterpret_cast<uint32_t**>(0x74D7FC);
    // 0x47E8C6のTraining Display所有欄。Battle Settingsは別の+0xC8。
    auto* child = parent ? reinterpret_cast<uint32_t*>(parent[0xD0/4]) : nullptr;
    if (!child || child[0] != 0x5388C0 || !child[4] ||
        *reinterpret_cast<uint32_t**>(child[4]) != menu) return false;
    if (training_hitbox::Active()) { *command = 0; return true; }
    using domain::ui::FrameBarDisplay;
    const auto index = FindItem(menu, "CC_FRAME_BAR");
    if (index != UINT32_MAX) {
        auto* bar = reinterpret_cast<uint32_t**>(menu[0x4C/4])[index];
        // 左右は標準項目がこのobserverの後で処理する。次の通常更新で取り込み、
        // F1だけが変わった場合は逆向きに反映。解放済みメニューのポインタは保持しない。
        if (bool(bar[0x58/4]) != lastFrameBarValue)
            FrameBarDisplay::SetTraining(bar[0x58/4] != 0);
        if (*command == 1 && menu[0x40/4] == index) {
            FrameBarDisplay::ToggleTraining();
            *command = 0;
        }
        if (*command == 1 && menu[0x40/4] == FindItem(menu, "DEFAULT")) {
            FrameBarDisplay::SetTraining(false);
            training_hitbox::ResetOptions();
        }
        const bool enabled = FrameBarDisplay::Visible(1);
        bar[0x58/4] = unsigned(enabled);
        if (lastFrameBarValue != enabled) DebugLog("[TrainingDisplay] FRAME_BAR enabled=%u", unsigned(enabled));
        lastFrameBarValue = enabled;
    }
    if (*command == 1 && menu[0x40/4] == FindItem(menu, "CC_HITBOX")) {
        training_hitbox::Open();
        *command = 0;
    }
    return true;
}
__attribute__((force_align_arg_pointer)) void __stdcall RoundReset(void* battle) {
    if (pending && *CC_GAME_MODE_ADDR == CC_GAME_MODE_IN_GAME &&
        *reinterpret_cast<uint32_t*>(0x562A74) == 0x1010) {
        pending = false;
        const auto side = selection.player;
        const auto choice = selection.choice;
        auto* final = reinterpret_cast<uint32_t*>(0x74D838) + side * 11;
        auto* cursor = side ? CC_P2_SELECTOR_MODE_ADDR : CC_P1_SELECTOR_MODE_ADDR;
        const auto started = platform::RealMonotonicUs();
        DebugLog("[TrainingCharacter] LOAD begin side=%u char=%u moon=%u old=%u/%u stage=%u",
                 side, choice.character, choice.moon, final[2], final[5], *CC_STAGE_SELECTOR_ADDR);
        final[2] = choice.character; final[5] = choice.moon;
        // 特殊キャラの選択時フラグを次のキャラへ持ち越さない。
        final[9] = final[10] = 0;
        // 通常キャラ選択へ戻るカーソルには、隠しID・スタイルを渡さない。
        const auto cursorCharacter = CursorCharacter(choice.character);
        cursor[3] = core::sync::SelectionState::CharacterCell(cursorCharacter);
        cursor[4] = cursorCharacter; cursor[5] = choice.moon < 3 ? choice.moon : 0;
        uint32_t description[0xB8/4]{}, loading[3]{};
        cc_training_describe(description, loading);
        for (unsigned slot = 0; slot < 4; ++slot) cc_training_free_slot(slot);
        // 通常ロード0x448FB0のうちキャラ資産だけ。背景読込み0x4B6BF0は不要。
        reinterpret_cast<int (__fastcall*)(void*, void*)>(0x4489E0)(description, nullptr);
        // ボス専用番号にない表示画像を補う。相手・子キャラも全て再ロードされるため4枠を処理。
        // 全般的な画像ロードや共有キャラ定義を書き換えず、このTraining切替だけで適用する。
        for (unsigned slot = 0; slot < 4; ++slot) {
            const auto* actor = reinterpret_cast<const uint8_t*>(0x555130 + slot * 0xAFC);
            if (!actor[0]) continue;
            const auto character = uint32_t(actor[5]);
            const auto presentation = PresentationCharacter(character);
            if (presentation == character) continue;
            cc_training_load_presentation(slot, presentation, slot % 2, presentation);
            const auto* images = reinterpret_cast<const uint32_t*>(0x5642C8 + slot * 0x20);
            unsigned cutMask = 0;
            for (unsigned i = 0; i < 5; ++i) if (images[2+i]) cutMask |= 1u << i;
            DebugLog("[TrainingCharacter] PRESENTATION slot=%u char=%u source=%u face=%u color=%u cutMask=%u",
                     slot, character, presentation, images[0] != 0, images[1] != 0, cutMask);
        }
        changed = true;
        DebugLog("[TrainingCharacter] LOAD end side=%u char=%u moon=%u elapsedUs=%lld stage=%u",
                 side, choice.character, choice.moon, platform::RealMonotonicUs()-started, *CC_STAGE_SELECTOR_ADDR);
    }
    originalReset(battle);
}
bool Hook(uintptr_t address, const unsigned char* bytes, size_t length, void* replacement, void** original) {
    if (std::memcmp(reinterpret_cast<void*>(address), bytes, length)) return false;
    return MH_CreateHook(reinterpret_cast<void*>(address), replacement, original) == MH_OK &&
           cccaster::hook_batch::Enable(reinterpret_cast<void*>(address)) == MH_OK;
}
}
const Selection& Current() { return selection; }
bool Busy() { return selection.open || pending || training_palette::Active() || training_hitbox::Active(); }
const char* Error() { return error; }
int PortraitIndex(uint32_t character) {
    if (!installed || character >= 101) return -1;
    const auto* icons = reinterpret_cast<int*>(0x5519F8);
    // ボス差分に専用顔がない場合は元キャラの顔を使い、欄にBOSSを添える。
    return icons[character] >= 0 ? icons[character] : icons[CursorCharacter(character)];
}
int MoonPortraitIndex(uint32_t moon) {
    if (moon == 8 || moon == 9) return 3;
    if (installed) for (int i = 0; i < 3; ++i)
        if (reinterpret_cast<uint32_t*>(0x54D3CC)[i] == moon) return i;
    return 0;
}
std::string CharacterName(uint32_t character) {
    auto* data = Descriptor(character);
    if (!data) return "?";
    const auto* name = reinterpret_cast<const char*>(data + 1);
    return std::string(name, strnlen(name, 32));
}
bool ReadImage(const char* path, std::vector<uint8_t>& bytes) {
    if (!game_build::RuntimeValidated()) return false;
    uint32_t* file = nullptr;
    if (!reinterpret_cast<int (__cdecl*)(const char*, void*, unsigned, unsigned)>(0x4C8B10)(path,&file,0,0) || !file)
        return false;
    ++file[0x48/4];
    auto* data = reinterpret_cast<uint8_t*>(file[0x30/4]);
    const auto size = cc_training_file_size(file);
    const bool valid = data && size >= 128 && size <= 16*1024*1024;
    if (valid) bytes.assign(data, data + size);
    if (data) reinterpret_cast<void (__cdecl*)(void*)>(0x4E00D0)(data);
    reinterpret_cast<void (__thiscall*)(void*)>(0x414000)(file);
    reinterpret_cast<void (__cdecl*)(void*)>(0x4E02F3)(file);
    return valid;
}
void ObserveMenu(uint32_t* menu, uint32_t* command) {
    if (!installed || *CC_GAME_MODE_ADDR != CC_GAME_MODE_IN_GAME) return;
    if (ObserveDisplay(menu, command)) return;
    if (menu != menuSet ||
        mainMenu != *reinterpret_cast<uint32_t**>(0x74D7FC)) return;
    if (pending) {
        if (restartDispatched) { *command = 0; return; }
        const auto restart = FindItem(menu, "RESTART");
        if (restart != UINT32_MAX) {
            menu[0x40/4] = restart; *command = 1; restartDispatched = true;
        }
        return;
    }
    if (selection.open || training_palette::Active() || training_hitbox::Active()) { *command = 0; return; }
    if (*command == 1 && menu[0x40/4] == FindItem(menu,"CC_PALETTE")) {
        training_palette::Open(*reinterpret_cast<uint8_t*>(0x55DF0F));
        *command = 0;
        return;
    }
    if (*command == 1 && menu[0x40/4] == 0) {
        selection.Open({Choice{*reinterpret_cast<uint32_t*>(0x74D840), *reinterpret_cast<uint32_t*>(0x74D84C)},
                        Choice{*reinterpret_cast<uint32_t*>(0x74D86C), *reinterpret_cast<uint32_t*>(0x74D878)}},
                       *reinterpret_cast<uint8_t*>(0x55DF0F));
        suppressUntilRelease = true;
        error = "";
        *command = 0;
        DebugLog("[TrainingCharacter] OPEN side=%u char=%u moon=%u", selection.player, selection.choice.character, selection.choice.moon);
    }
}
}

namespace cccaster::game_interface {
bool RealGameMemory::ConfigureTrainingMenu() {
    using namespace training_character;
    if (installed) return true;
    if (!game_build::RuntimeValidated() || !ConfigureMenuObserver() || !training_palette::Install() || !training_hitbox::Install()) return false;
    const unsigned char constructor[]{0x6a,0xff,0x68,0x67,0x75,0x51,0x00};
    const unsigned char displayConstructor[]{0x6a,0xff,0x68,0x01,0x72,0x51,0x00};
    const unsigned char selectConstructor[]{0x6a,0xff,0x68,0xf8,0x63,0x51,0x00};
    const unsigned char choiceConstructor[]{0x6a,0xff,0x68,0xf3,0x53,0x51,0x00};
    const unsigned char reset[]{0x83,0xec,0x10,0xa1,0x58,0xb4,0x54,0x00};
    const unsigned char presentation[]{0x81,0xec,0x14,0x01,0x00,0x00,0xa1,0x58,0xb4,0x54,0x00};
    const unsigned char appendInformation[]{0x83,0xec,0x0c,0x53,0x8b,0x5e,0x04};
    const unsigned char assignString[]{0x53,0x55,0x56,0x8b,0xf1,0x8b,0x4e,0x18};
    if (std::memcmp(reinterpret_cast<void*>(0x4DAA30), appendInformation, sizeof(appendInformation)) ||
        std::memcmp(reinterpret_cast<void*>(0x407C10), assignString, sizeof(assignString))) return false;
    if (std::memcmp(reinterpret_cast<void*>(0x426340), presentation, sizeof(presentation))) return false;
    if (std::memcmp(reinterpret_cast<void*>(0x42F8F0), selectConstructor, sizeof(selectConstructor)) ||
        std::memcmp(reinterpret_cast<void*>(0x42F600), choiceConstructor, sizeof(choiceConstructor))) return false;
    if (!Hook(0x47D3A0, constructor, sizeof(constructor), reinterpret_cast<void*>(Construct),
              reinterpret_cast<void**>(&originalConstructor)) ||
        !Hook(0x480810, displayConstructor, sizeof(displayConstructor), reinterpret_cast<void*>(ConstructDisplay),
              reinterpret_cast<void**>(&originalDisplayConstructor)) ||
        !Hook(0x423380, reset, sizeof(reset), reinterpret_cast<void*>(RoundReset),
              reinterpret_cast<void**>(&originalReset))) return false;
    installed = true;
    return true;
}
bool RealGameMemory::StepTrainingMenu(GameInput& p1, GameInput& p2, bool configuring) {
    using namespace training_character;
    training_hitbox::Step(p1,p2,configuring);
    training_palette::Step(p1,p2,configuring);
    const bool didChange = changed;
    changed = false;
    if (GameMode() != CC_GAME_MODE_IN_GAME) {
        selection.open = pending = false; mainMenu = menuSet = nullptr;
        suppressUntilRelease = false; previousButtons = previousDirection = 0;
        return didChange;
    }
    const auto buttons = p1.buttons | p2.buttons;
    const auto direction = p1.direction ? p1.direction : p2.direction;
    if (configuring && selection.open) { selection.open = false; suppressUntilRelease = true; }
    if (selection.open && !configuring && !suppressUntilRelease) {
        Action action = Action::None;
        const auto edge = buttons & ~previousButtons;
        if (edge & (CC_BUTTON_B | CC_BUTTON_CANCEL | CC_BUTTON_START)) action = Action::Cancel;
        else if (edge & (CC_BUTTON_A | CC_BUTTON_CONFIRM)) action = Action::Accept;
        else if (direction && (direction != previousDirection || platform::RealMonotonicUs() >= repeatAt)) {
            action = direction == 8 ? Action::Up : direction == 2 ? Action::Down :
                     direction == 4 ? Action::Left : direction == 6 ? Action::Right : Action::None;
            repeatAt = platform::RealMonotonicUs() + (direction == previousDirection ? 90000 : 350000);
        }
        const auto result = selection.Step(action);
        if (action != Action::None) error = "";
        if (result == Result::Apply) {
            if (!Available(selection.choice)) {
                selection.open = true; selection.field = Field::Moon;
                error = "This style is not available for this character.";
            } else if (selection.choice.character == selection.original[selection.player].character &&
                       selection.choice.moon == selection.original[selection.player].moon) {
                suppressUntilRelease = true;
            } else {
                pending = true; restartDispatched = false; suppressUntilRelease = true;
                DebugLog("[TrainingCharacter] COMMIT side=%u char=%u moon=%u", selection.player, selection.choice.character, selection.choice.moon);
            }
        } else if (result == Result::Cancelled) {
            suppressUntilRelease = true;
            DebugLog("[TrainingCharacter] CANCEL");
        }
    }
    previousButtons = buttons; previousDirection = direction;
    if (selection.open || pending || suppressUntilRelease || didChange) p1 = p2 = {};
    if (!buttons && !direction) suppressUntilRelease = false;
    return didChange;
}
}
