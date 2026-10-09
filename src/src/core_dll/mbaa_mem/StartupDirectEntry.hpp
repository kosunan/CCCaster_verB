#pragma once
#include <cstdint>
namespace cccaster::game_memory::startup_direct_entry {
// 起動ゲートの停止中に一度だけ設定。ロゴ処理前から正規モード初期化へ接続する。
void Initialize(uint8_t appMode, bool isHost);
// ゲームスレッドの起動処理からのみ呼ぶ。メニュー生成前の正規遷移予約を置き換える。
bool TryTraining(uint32_t currentMode);
bool TryVersus(uint32_t currentMode, bool isHost);
bool ReplayEnabled();
bool ReplayEntryPending(uint32_t currentMode);
bool TryReplay(uint32_t currentMode);
}
