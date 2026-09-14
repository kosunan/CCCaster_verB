#pragma once
/**
 * @file MbaaInputDefs.hpp
 * @brief MBAACC 入力アドレス・ボタンビットマスク・入力合成マクロ
 *
 * 【責務】
 *   ゲーム入力に関する定数定義を集約する。
 *   方向キー、ボタン、入力書き込みアドレス、合成マクロを含む。
 *
 * 【分割元】
 *   旧 MbaaAddresses.hpp の入力系セクション（L139-L177）を抽出。
 */

#include <cstdint>

// ============================================================================
// 入力アドレス
// ============================================================================

#define CC_PTR_TO_WRITE_INPUT_ADDR ((char *)0x76E6AC) // Pointer to the location to write game input
#define CC_P1_OFFSET_DIRECTION (0x18)                 // Offset to write P1 direction input
#define CC_P1_OFFSET_BUTTONS (0x24)                   // Offset to write P1 buttons input
#define CC_P2_OFFSET_DIRECTION (0x2C)                 // Offset to write P2 direction input
#define CC_P2_OFFSET_BUTTONS (0x38)                   // Offset to write P2 buttons input

// 方向キーは GameInput.hpp の Dir:: を使う（テンキー表記, ニュートラル=0）。
// 以前ここにあった BIT_UP/DOWN/LEFT/RIGHT のビットマスク定義は、
// 実際の書込み規約（テンキー表記）と食い違ったまま Rematch から参照され、
// 「下」が CC_PLAYER_FACING、「上」が CC_BUTTON_START になる原因だったため削除した。

// ============================================================================
// ボタン定義
// ============================================================================

#define CC_BUTTON_A (0x0010)
#define CC_BUTTON_B (0x0020)
#define CC_BUTTON_C (0x0008)
#define CC_BUTTON_D (0x0004)
#define CC_BUTTON_E (0x0080)
#define CC_BUTTON_AB (0x0040)
#define CC_BUTTON_START (0x0001)
#define CC_BUTTON_FN1 (0x0100) // Control dummy
#define CC_BUTTON_FN2 (0x0200) // Training reset
#define CC_BUTTON_CONFIRM (0x0400)
#define CC_BUTTON_CANCEL (0x0800)
#define CC_PLAYER_FACING (0x0002)

// 32bit への詰め替えは GameInput::Pack() / Unpack() のみを使う。
// 以前ここにあった COMBINE_INPUT (direction | buttons << 8) は使用箇所ゼロで、
// 3つ目の非互換な符号化として残っていたため削除した。
