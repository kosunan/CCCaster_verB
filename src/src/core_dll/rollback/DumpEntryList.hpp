#pragma once
// ============================================================================
// DumpEntryList — Constants.hppベースのダンプエントリ構築
// ============================================================================

#include <vector>
#include "core_dll/rollback/MemDumper.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"

namespace cccaster::sync {

// MBAAのゲーム状態をダンプするためのエントリリストを構築
// レガシーの rollback.bin (MemDumpList) の代替
inline std::vector<DumpEntry> BuildGameDumpEntries() {
    std::vector<DumpEntry> entries;

    // ===== Misc: ゲーム全般 =====
    
    // ワールドタイマー
    entries.push_back({ (uintptr_t)CC_WORLD_TIMER_ADDR, 4 });
    
    // ラウンドタイマー + リアルタイマー
    entries.push_back({ (uintptr_t)CC_ROUND_TIMER_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_REAL_TIMER_ADDR, 4 });
    
    // ポーズフラグ
    entries.push_back({ (uintptr_t)CC_PAUSE_FLAG_ADDR, 1 });
    
    // ゲーム状態
    entries.push_back({ (uintptr_t)CC_GAME_STATE_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_INTRO_STATE_ADDR, 1 });
    
    // 勝敗
    entries.push_back({ (uintptr_t)CC_P1_GAME_POINT_FLAG_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_P2_GAME_POINT_FLAG_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_P1_WINS_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_P2_WINS_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_ROUND_COUNT_ADDR, 4 });
    
    // ヒットスパーク
    entries.push_back({ (uintptr_t)CC_HIT_SPARKS_ADDR, 4 });
    
    // カメラ
    entries.push_back({ (uintptr_t)CC_CAMERA_X_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_CAMERA_Y_ADDR, 4 });
    
    // スキップフレーム（ロールバック後に復元が必要）
    // entries.push_back({ (uintptr_t)CC_SKIP_FRAMES_ADDR, 4 });  // 使用禁止
    entries.push_back({ (uintptr_t)CC_SKIPPABLE_FLAG_ADDR, 4 });
    
    // ===== RNG: 乱数状態 =====
    entries.push_back({ (uintptr_t)CC_RNG_STATE0_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_RNG_STATE1_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_RNG_STATE2_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_RNG_STATE3_ADDR, CC_RNG_STATE3_SIZE });
    
    // ===== Players: P1〜P4 構造体 =====
    // 各プレイヤーは CC_PLR_STRUCT_SIZE (0xAFC) の連続構造体
    // P1=0x555130, P2=P1+0xAFC, P3=P2+0xAFC, P4=P3+0xAFC
    // 4プレイヤー分を一括（パペットキャラ対応）
    entries.push_back({ (uintptr_t)CC_P1_ENABLED_FLAG_ADDR, CC_PLR_STRUCT_SIZE * 4 });
    
    // ===== 表示フラグ =====
    entries.push_back({ (uintptr_t)CC_SHOW_ATTACK_DISPLAY, 4 });
    entries.push_back({ (uintptr_t)CC_SHOW_INPUT_DISPLAY, 4 });
    
    // ===== 対戦ポーズ =====
    entries.push_back({ (uintptr_t)CC_VERSUS_PAUSE_ADDR, 4 });
    entries.push_back({ (uintptr_t)CC_TRAINING_PAUSE_ADDR, 4 });

    return entries;
}

} // namespace cccaster::sync
