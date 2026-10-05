#pragma once
#include "shared_contracts/CheckedPatch.hpp"
namespace cccaster::game_memory {
class MbaaPatcher {
public:
    // ゲーム入口停止中のみ。失敗時はゲームを開始しない。
    static patch::Result ApplyStartupPatches(bool training);
    // BgList読込み後・選択入力前に、ゲームスレッドで一度だけ呼ぶ。
    static patch::Result ApplyPostLoadStagePatches();
};
}
