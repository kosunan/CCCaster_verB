#pragma once
/**
 * @file MbaaAddresses.hpp
 * @brief MELTY BLOOD Actress Again Current Code (MBAACC) メモリアドレス・定数定義
 *
 * 【責務】
 *   MBAAゲームプロセス内の全メモリアドレス、ゲームモードID、
 *   プレイヤー構造体オフセット等の定数を一元管理する。
 *
 * 【設計上の位置づけ】
 *   Domain 層（MBAA固有）に属する。
 *   Core 層のコードがこのファイルを直接参照することは設計違反。
 *
 * 【分割元】
 *   旧 MbaaAddresses.hpp から定数定義部分を抽出。
 */

// windows.h は不要（Win32 の型を一切使っていない）。
// harness を Linux でもビルドするため、OS 依存の include をヘッダから排除する。
#include <cstdint>

// ============================================================================
// ロールバック・入力バッファ設定
// ============================================================================

// Number of frames of inputs to send per message
#define NUM_INPUTS (30)

// Max allow rollback frames
#define MAX_ROLLBACK (15)

// Number of rollback states to allocate
#ifdef RELEASE
#define NUM_ROLLBACK_STATES (60)
#else
#define NUM_ROLLBACK_STATES (256)
#endif

// ============================================================================
// ゲーム基本情報
// ============================================================================

// Game constants and addresses are prefixed CC
#define CC_VERSION "1.4.0"
#define CC_TITLE "MELTY BLOOD Actress Again Current Code Ver.1.07 Rev." CC_VERSION
#define CC_STARTUP_TITLE CC_TITLE " "
#define CC_STARTUP_BUTTON "OK"
#define CC_NETWORK_CONFIG_FILE "System\\NetConnect.dat"
#define CC_NETWORK_USERNAME_KEY "UserName"
#define CC_APP_CONFIG_FILE "System\\_App.ini"
#define CC_APP_WINDOW_MODE_KEY "Windowed"

// Location of the keyboard config in the binary
#define CC_KEYBOARD_CONFIG_OFFSET (0x14D2C0)

// ============================================================================
// ゲームメモリアドレス — システム
// ============================================================================

#define CC_WINDOW_PROC_ADDR ((char *)0x40D4C0)      // Location of WindowProc
#define CC_LOOP_START_ADDR ((char *)0x40D330)       // Start of the main event loop
#define CC_SCREEN_WIDTH_ADDR ((uint32_t *)0x54D048) // The actual width of the main viewport
#define CC_WORLD_TIMER_ADDR ((uint32_t *)0x55D1D4)  // Frame step timer, always counting up
#define CC_PAUSE_FLAG_ADDR ((uint8_t *)0x55D203)    // 1 when paused
// #define CC_SKIP_FRAMES_ADDR         ( ( uint32_t * ) 0x55D25C ) // 使用禁止: 描画制御は API hook で行う
#define CC_INTRO_STATE_ADDR ((uint8_t *)0x55D20B) // 2 (character intros), 1 (pre-game), 0 (in-game)
#define CC_ALIVE_FLAG_ADDR ((uint8_t *)0x76E650)  // Flag that indicates the game is alive

// FPS/パフォーマンス
#define CC_FPS_COUNTER_ADDR ((uint32_t *)0x774A70) // Value of the displayed FPS counter
#define CC_PERF_FREQ_ADDR ((uint64_t *)0x774A80)   // Value of QueryPerformanceFrequency for game FPS

// Direct3D
#define CC_D3DX9_OBJ_ADDR ((uint32_t *)0x76E7D4) // The address for the IDirect3DDevice9

// ============================================================================
// ゲームメモリアドレス — 対戦設定
// ============================================================================

#define CC_DAMAGE_LEVEL_ADDR ((uint32_t *)0x553FCC)        // Damage level: default 2
#define CC_WIN_COUNT_VS_ADDR ((uint32_t *)0x553FDC)        // Win count: default 2
#define CC_TIMER_SPEED_ADDR ((uint32_t *)0x553FD0)         // Timer speed: default 2
#define CC_AUTO_REPLAY_SAVE_ADDR ((uint32_t *)0x553FE8)    // Auto replay saving: 0 to disable, 1 to enable
#define CC_STAGE_ANIMATION_OFF_ADDR ((uint32_t *)0x554124) // 1 if stage animations are off

// ============================================================================
// ゲームメモリアドレス — ラウンド・タイマー
// ============================================================================

#define CC_ROUND_TIMER_ADDR ((uint32_t *)0x562A3C)        // Counts down from 4752, may stop
#define CC_REAL_TIMER_ADDR ((uint32_t *)0x562A40)         // Counts up from 0 after round start
#define CC_ROUND_COUNT_ADDR ((uint32_t *)0x5550E0)        // Round count
#define CC_P1_GAME_POINT_FLAG_ADDR ((uint32_t *)0x559548) // P1 game point flag
#define CC_P2_GAME_POINT_FLAG_ADDR ((uint32_t *)0x55954C) // P2 game point flag
#define CC_P1_WINS_ADDR ((uint32_t *)0x559550)            // P1 number of wins
#define CC_P2_WINS_ADDR ((uint32_t *)0x559580)            // P2 number of wins
#define CC_TRAINING_PAUSE_ADDR ((uint32_t *)0x562A64)     // 1 when paused
#define CC_VERSUS_PAUSE_ADDR ((uint32_t *)0x564B30)       // 0xFFFFFFFF when paused
#define CC_HIT_SPARKS_ADDR ((uint32_t *)0x67BD78)         // Number of hit sparks?
#define CC_SKIPPABLE_FLAG_ADDR ((uint32_t *)0x74D99C) // Flag that indicates a skippable state when in-game

// Number of frames in the initial movement only phase
#define CC_PRE_GAME_INTRO_FRAMES (224)

// ============================================================================
// ゲームメモリアドレス — メニュー / UI
// ============================================================================

// メニュー階層カウンタ: メニュー開→+1, 閉→-1 のスタック深度
// RetryMenu で「サブメニュー（リプレイ保存等）が開いているか」を判定するために使用
#define CC_MENU_STATE_COUNTER_ADDR ((uint32_t *)0x767440)

// ステージセレクタ
#define CC_STAGE_SELECTOR_ADDR ((uint32_t *)0x74FD98) // Currently selected stage, can be assigned to directly

// リプレイ
#define CC_REPLAY_CREATED_ADDR                                                                               \
    ((uint32_t *)0x774C30) // Flag that indicates whether a replay file has been written
#define CC_REPROUND_TBL_ENDPTR_ADDR                                                                          \
    ((void *)0x77BF9C) // Pointer to the end of the table of replay round structs

// 表示フラグ
#define CC_SHOW_ATTACK_DISPLAY ((int *)0x5595B8)
#define CC_SHOW_INPUT_DISPLAY ((int *)0x5585F8)

// ============================================================================
// ゲームモード / ゲームステート
// ============================================================================

#define CC_GAME_MODE_ADDR ((uint32_t *)0x54EEE8) // Current game mode, constants below

// List of game modes relevant to netplay
#define CC_GAME_MODE_STARTUP (65535)
#define CC_GAME_MODE_OPENING (3)
#define CC_GAME_MODE_TITLE (2)
#define CC_GAME_MODE_LOADING_DEMO (13)
#define CC_GAME_MODE_HIGH_SCORES (11)
#define CC_GAME_MODE_MAIN (25)
#define CC_GAME_MODE_REPLAY (26)
#define CC_GAME_MODE_CHARA_SELECT (20)
#define CC_GAME_MODE_LOADING (8)
#define CC_GAME_MODE_IN_GAME (1)
#define CC_GAME_MODE_RETRY (5)

// RetryMenu (リマッチ) 定数
// Max allowed retry menu index (once again, chara select, save replay)
// Prevent returning to main menu
#define MAX_RETRY_MENU_INDEX (2)

#define CC_GAME_STATE_ADDR ((uint32_t *)0x74d598) // Intermediate game states, constants below

// List of states modes relevant to netplay
#define CC_GAME_STATE_CHARA_INTRO (1)
#define CC_GAME_STATE_INTRO_SKIP (101)
#define CC_GAME_STATE_INTRO_MID (100)
#define CC_GAME_STATE_INTRO_DONE (99)
#define CC_GAME_STATE_CINTRO_END (12)
#define CC_GAME_STATE_PREGAME_DONE (2)

// ============================================================================
// キャラクターセレクト
// ============================================================================

// Character select data, can be assigned to directly at the character select screen
#define CC_P1_SELECTOR_MODE_ADDR ((uint32_t *)0x74D8EC)
#define CC_P1_CHARA_SELECTOR_ADDR ((uint32_t *)0x74D8F8)
#define CC_P1_CHARACTER_ADDR ((uint32_t *)0x74D8FC)
#define CC_P1_MOON_SELECTOR_ADDR ((uint32_t *)0x74D900)
#define CC_P1_COLOR_SELECTOR_ADDR ((uint32_t *)0x74D904)
#define CC_P1_RANDOM_COLOR_ADDR ((uint8_t *)((*(uint32_t *)0x74D808) + 0 * 0x1DC + 0x2C + 0x0C))
#define CC_P2_SELECTOR_MODE_ADDR ((uint32_t *)0x74D910)
#define CC_P2_CHARA_SELECTOR_ADDR ((uint32_t *)0x74D91C)
#define CC_P2_CHARACTER_ADDR ((uint32_t *)0x74D920)
#define CC_P2_MOON_SELECTOR_ADDR ((uint32_t *)0x74D924)
#define CC_P2_COLOR_SELECTOR_ADDR ((uint32_t *)0x74D928)
#define CC_P2_RANDOM_COLOR_ADDR ((uint8_t *)((*(uint32_t *)0x74D808) + 1 * 0x1DC + 0x2C + 0x0C))

// Character select selection mode
#define CC_SELECT_CHARA (0)
#define CC_SELECT_MOON (1)
#define CC_SELECT_COLOR (2)

// ============================================================================
// トレーニングモード
// ============================================================================

#define CC_DUMMY_STATUS_ADDR ((int32_t *)0x74D7F8) // Training mode dummy status
#define CC_DUMMY_STATUS_STAND (0)
#define CC_DUMMY_STATUS_JUMP (1)
#define CC_DUMMY_STATUS_CROUCH (2)
#define CC_DUMMY_STATUS_CPU (3)
#define CC_DUMMY_STATUS_MANUAL (4)
#define CC_DUMMY_STATUS_DUMMY (5)
#define CC_DUMMY_STATUS_RECORD (-1)

#define CC_P1_COMBO_GUARD_ADDR ((uint32_t *)0x76E708)

// ============================================================================
// RNG
// ============================================================================

// Complete RngState
#define CC_RNG_STATE0_ADDR ((uint32_t *)0x563778)
#define CC_RNG_STATE1_ADDR ((uint32_t *)0x56377C)
#define CC_RNG_STATE2_ADDR ((uint32_t *)0x564068)
#define CC_RNG_STATE3_ADDR ((char *)0x564070)
#define CC_RNG_STATE3_SIZE (220)

// ============================================================================
// プレイヤー構造体
// ============================================================================

// Total size of a single player structure.
// Note: there are FOUR player structs in memory, due to the puppet characters.
#define CC_PLR_STRUCT_SIZE (0xAFC)

// Player addresses
// P1 start from 0x555130
// P2 start from 0x555C2C
#define CC_P1_ENABLED_FLAG_ADDR ((uint8_t *)0x555130)
#define CC_P1_SEQUENCE_ADDR ((uint32_t *)0x555140)
#define CC_P1_SEQ_STATE_ADDR ((uint32_t *)0x555144)
#define CC_P1_HEALTH_ADDR ((uint32_t *)0x5551EC)
#define CC_P1_RED_HEALTH_ADDR ((uint32_t *)0x5551F0)
#define CC_P1_GUARD_BAR_ADDR ((float *)0x5551F4)
#define CC_P1_GUARD_QUALITY_ADDR ((float *)0x555208)
#define CC_P1_METER_ADDR ((uint32_t *)0x555210)
#define CC_P1_HEAT_ADDR ((uint32_t *)0x555214)
#define CC_P1_NO_INPUT_FLAG_ADDR ((uint8_t *)0x5552A7) // Indicates when input is disabled, ie KO or time over
#define CC_P1_PUPPET_STATE_ADDR ((uint8_t *)0x5552A8) // 0 is not puppet, 1 is puppet, 2 is puppet w/ hurtbox?

#define CC_P1_X_POSITION_ADDR ((int32_t *)0x555238)
#define CC_P1_Y_POSITION_ADDR ((int32_t *)0x55523C)
#define CC_P1_X_PREV_POS_ADDR ((int32_t *)0x555244)
#define CC_P1_Y_PREV_POS_ADDR ((int32_t *)0x555248)
#define CC_P1_X_VELOCITY_ADDR ((int32_t *)0x55524C)
#define CC_P1_Y_VELOCITY_ADDR ((int32_t *)0x555250)
#define CC_P1_X_ACCELERATION_ADDR ((int16_t *)0x555254)
#define CC_P1_Y_ACCELERATION_ADDR ((int16_t *)0x555256)
#define CC_P1_SPRITE_ANGLE_ADDR ((uint32_t *)0x555430)
#define CC_P1_FACING_FLAG_ADDR ((uint8_t *)0x555444)     // 0 facing left, 1 facing right
#define CC_P1_COMBO_OFFSET_ADDR ((uint8_t *)0x557E59)    // Index into combo structure array
#define CC_P1_COMBO_HIT_BASE_ADDR ((uint32_t *)0x557E5C) // Base for combo strucure array

#define CC_P2_ENABLED_FLAG_ADDR ((uint8_t *)(((char *)CC_P1_ENABLED_FLAG_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_SEQUENCE_ADDR ((uint32_t *)(((char *)CC_P1_SEQUENCE_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_SEQ_STATE_ADDR ((uint32_t *)(((char *)CC_P1_SEQ_STATE_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_HEALTH_ADDR ((uint32_t *)(((char *)CC_P1_HEALTH_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_RED_HEALTH_ADDR ((uint32_t *)(((char *)CC_P1_RED_HEALTH_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_GUARD_BAR_ADDR ((float *)(((char *)CC_P1_GUARD_BAR_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_GUARD_QUALITY_ADDR ((float *)(((char *)CC_P1_GUARD_QUALITY_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_METER_ADDR ((uint32_t *)(((char *)CC_P1_METER_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_HEAT_ADDR ((uint32_t *)(((char *)CC_P1_HEAT_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_NO_INPUT_FLAG_ADDR ((uint8_t *)(((char *)CC_P1_NO_INPUT_FLAG_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_PUPPET_STATE_ADDR ((uint8_t *)(((char *)CC_P1_PUPPET_STATE_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_X_POSITION_ADDR ((int32_t *)(((char *)CC_P1_X_POSITION_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_Y_POSITION_ADDR ((int32_t *)(((char *)CC_P1_Y_POSITION_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P2_FACING_FLAG_ADDR ((uint8_t *)(((char *)CC_P1_FACING_FLAG_ADDR) + CC_PLR_STRUCT_SIZE))

#define CC_P3_ENABLED_FLAG_ADDR ((uint8_t *)(((char *)CC_P2_ENABLED_FLAG_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P3_SEQUENCE_ADDR ((uint32_t *)(((char *)CC_P2_SEQUENCE_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P3_NO_INPUT_FLAG_ADDR ((uint8_t *)(((char *)CC_P2_NO_INPUT_FLAG_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P3_PUPPET_STATE_ADDR ((uint8_t *)(((char *)CC_P2_PUPPET_STATE_ADDR) + CC_PLR_STRUCT_SIZE))

#define CC_P4_ENABLED_FLAG_ADDR ((uint8_t *)(((char *)CC_P3_ENABLED_FLAG_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P4_NO_INPUT_FLAG_ADDR ((uint8_t *)(((char *)CC_P3_NO_INPUT_FLAG_ADDR) + CC_PLR_STRUCT_SIZE))
#define CC_P4_PUPPET_STATE_ADDR ((uint8_t *)(((char *)CC_P3_PUPPET_STATE_ADDR) + CC_PLR_STRUCT_SIZE))

// ============================================================================
// カメラ
// ============================================================================

#define CC_CAMERA_X_ADDR ((int *)0x564B14)
#define CC_CAMERA_Y_ADDR ((int *)0x564B18)

// ============================================================================
// SFX（効果音）
// ============================================================================

// Array of sound effect flags, each byte corresponds to a specific SFX, set to 1 to start
#define CC_SFX_ARRAY_ADDR ((uint8_t *)0x76E008)
#define CC_SFX_ARRAY_LEN (1500)
#define DX_MUTED_VOLUME (0xFFFFD8F0u)

// ============================================================================
// ASM ハック用アドレス（MM = Modified Memory）
// ============================================================================

#define MM_HOOK_CALL1_ADDR ((char *)0x40D032)
#define MM_HOOK_CALL2_ADDR ((char *)0x40D411)

// Allows for multiple instances of melty
#define MULTIPLE_MELTY ((char *)0x40D25A)

// ============================================================================
// FastBoot / Patch 用アドレス
// ============================================================================
#define CC_AUTO_ACTIVATE_ADDR ((char *)0x40E0C0) // ウィンドウの非アクティブ判定関数
#define CC_FORCE_GOTO_ADDR ((char *)0x42B475)    // メニュー遷移時のJMP強制アドレス

// ============================================================================
// スプライト・フォント
// ============================================================================

// Addresses for sprite textures
#define BUTTON_SPRITE_TEX (0x74d5e8)
// Addresses for DrawText fonts
#define FONT0 (0x55D680)
#define FONT1 (0x55D260)
#define FONT2 (0x55DAA0)

// Defined Sequence numbers
#define CC_SEQ_CROUCH_TRANSITION (12)
