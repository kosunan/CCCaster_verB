#pragma once
/**
 * @file GameInput.hpp
 * @brief ゲームに書き込む1プレイヤー分の入力
 *
 * 【なぜ型にするか】
 *   以前は入力を生の uint32_t で持ち回っており、符号化が3種類混在していた。
 *     (1) direction << 16 | buttons  ← DirectInputHook / FrameControl / FastBoot の実際の規約
 *     (2) BIT_UP=0x01 / BIT_DOWN=0x02 のビットマスク  ← MatchScene の Rematch がこの前提だった
 *     (3) COMBINE_INPUT = direction | buttons << 8    ← 使用箇所ゼロの死んだマクロ
 *   その結果、Rematch の自動ナビが方向値をボタンとして書き込み、
 *   「下」が CC_PLAYER_FACING、「上」が CC_BUTTON_START になっていた。
 *   uint32_t のままではコンパイラが何も守れないため、境界の外に生値を出さない。
 *
 * 【32bit 表現】
 *   通信パケットと入力バッファは 32bit で持つ。変換は Pack()/Unpack() のみを使う。
 */

#include <cstdint>

namespace cccaster::game_interface {

/// 方向キー。MBAA のメモリ表現はテンキー表記で、ニュートラルのみ 0。
namespace Dir {
inline constexpr uint16_t Neutral = 0;
inline constexpr uint16_t DownLeft = 1;
inline constexpr uint16_t Down = 2;
inline constexpr uint16_t DownRight = 3;
inline constexpr uint16_t Left = 4;
inline constexpr uint16_t Right = 6;
inline constexpr uint16_t UpLeft = 7;
inline constexpr uint16_t Up = 8;
inline constexpr uint16_t UpRight = 9;
} // namespace Dir

struct GameInput {
    uint16_t direction = Dir::Neutral; ///< テンキー表記 (0=ニュートラル, 1-9)
    uint16_t buttons = 0;              ///< CC_BUTTON_* の OR

    /// 通信・バッファ用の 32bit 表現
    constexpr uint32_t Pack() const {
        return (static_cast<uint32_t>(direction) << 16) | static_cast<uint32_t>(buttons);
    }

    static constexpr GameInput Unpack(uint32_t packed) {
        return GameInput{static_cast<uint16_t>((packed >> 16) & 0xFFFF),
                         static_cast<uint16_t>(packed & 0xFFFF)};
    }

    constexpr bool IsNeutral() const {
        return direction == Dir::Neutral && buttons == 0;
    }

    friend constexpr bool operator==(GameInput a, GameInput b) {
        return a.direction == b.direction && a.buttons == b.buttons;
    }
    friend constexpr bool operator!=(GameInput a, GameInput b) {
        return !(a == b);
    }
};

} // namespace cccaster::game_interface
