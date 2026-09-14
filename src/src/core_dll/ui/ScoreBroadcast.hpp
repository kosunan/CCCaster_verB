#pragma once
#include "core_dll/engine/SessionScore.hpp"
#include <filesystem>

namespace cccaster::domain::ui::score_broadcast {
// InitThreadから呼ぶ。プロセスID別JSON/TXTで複数ゲームの同時起動を分離する。
bool Initialize(const std::filesystem::path &directory);
// 確定試合／セッション状態が変わったときのみ呼ぶ。ファイル操作はworkerが行う。
void Publish(const session::SessionScoreSnapshot &score);
// 終了通知はブロックしない。スコアと表示を空にし、workerの終了を要求する。
void Clear();
// DLLを明示unloadする際は必ずloader lock外から呼ぶ。DllMainからは呼ばない。
void Shutdown();
bool Healthy();
} // namespace cccaster::domain::ui::score_broadcast
