#include "core_dll/mbaa_mem/MbaaPatcher.hpp"
#include "shared_contracts/ProcessMemory.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/mbaa_mem/StagePatches.hpp"
namespace cccaster::game_memory {
patch::Result MbaaPatcher::ApplyStartupPatches(bool training) {
    // Carnival140 / Community の実バイナリで照合した命令列。
    // 旧0x40E0C0の11B NOPはMOV即値と条件分岐を切断するため除外。
    // 0x4A1D42は起動設定ダイアログを閉じる分岐。分岐先を含む原命令を照合する。
    // 飛び越した命令の即値を壊す旧0x4A1D4Aの1B変更は不要。
    static constexpr uint8_t nop2[]{0x90,0x90}, nop3[]{0x90,0x90,0x90};
    static constexpr uint8_t a[]{0x89,0x06}, b[]{0x89,0x46,0x0c}, c[]{0x89,0x07};
    static constexpr uint8_t d[]{0x09,0x57,0x0c}, e[]{0x89,0x17}, f[]{0x09,0x47,0x0c};
    // キー配列はEXEごとに異なる。参照先と2人分のstrideを互換性検査で確認した上で、
    // 実際の元データを保存する。特定配布版の既定キーを起動条件にしない。
    uint8_t keyboard[20]{};
    patch::ProcessMemory memory;
    if (!memory.Read(0x54d2c0,keyboard,sizeof(keyboard)))
        return {patch::Error::Read,memory.LastError(),0x54d2c0,"keyboard_map"};
    static constexpr uint8_t zero[20]{};
    static constexpr uint8_t dialog[]{0x75,0x16,0x0f,0xb7,0x45,0x10,0x83,0xe8,0x01,0x74,0x11,
        0x83,0xe8,0x01,0x75,0x08,0x50,0x56,0xff,0x15,0x1c,0xb3,0x51,0x00,0x33,0xc0,0xeb,0x0e};
    static constexpr uint8_t closeDialog[]{0xeb,0x0e};
    static constexpr uint8_t music[]{0x75,0x05,0xe8,0xcc,0xb2,0x06,0x00}, skipMusic[]{0xeb,0x05};
    const patch::Spec targets[]{
        {"input_direction_1",0x41f098,a,nop2}, {"input_buttons_1",0x41f0a0,b,nop3},
        {"input_direction_2",0x4a024e,c,nop2}, {"input_buttons_2a",0x4a027f,d,nop3},
        {"input_buttons_2b",0x4a0291,d,nop3}, {"input_buttons_2c",0x4a02a2,d,nop3},
        {"input_buttons_2d",0x4a02b4,d,nop3}, {"input_direction_3",0x4a02e9,e,nop2},
        {"input_buttons_3",0x4a02f2,f,nop3}, {"keyboard_map",0x54d2c0,keyboard,zero,false},
        {"startup_dialog_close",0x4a1d42,dialog,closeDialog},
        // 追加するボス背景の暗転を通常サイズで描く。BgList読込み前に適用する。
        {"boss_stage_overlay",0x53b3c8,stages::BossOverlayKey,stages::BossOverlayDisabled,false},
        {"training_music",0x472c6d,music,skipMusic}};
    const auto result = patch::Apply(std::span(targets, training ? std::size(targets) : std::size(targets)-1));
    domain::session::DebugLog("[MbaaPatcher] success=%d patch=%s address=%08X error=%s win32=%u rollbackFailed=%d",
        bool(result), result.name, unsigned(result.address), patch::Name(result.error), result.systemError, result.rollbackFailed);
    return result;
}
patch::Result MbaaPatcher::ApplyPostLoadStagePatches() {
    // BgListによる初期化が終わったゲームスレッドで、一度だけ適用する。
    static bool applied = false;
    if (applied) return {};
    const auto targets = stages::PostLoadSpecs();
    const auto result = patch::Apply(targets);
    domain::session::DebugLog("[StagePatches] success=%d patch=%s address=%08X error=%s win32=%u rollbackFailed=%d",
        bool(result), result.name, unsigned(result.address), patch::Name(result.error), result.systemError, result.rollbackFailed);
    applied = bool(result);
    return result;
}
}
