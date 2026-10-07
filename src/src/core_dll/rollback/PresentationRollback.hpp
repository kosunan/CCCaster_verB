#pragma once
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include <cfenv>
#include <vector>

namespace cccaster::sync {
// 描画のためだけに進めた標準更新を戻す。確定入力・通信フレームは所有しない。
// 復元失敗時は呼出側がゲームを停止し、不確かな状態での続行を防ぐ。
class PresentationRollback {
    std::vector<char> saved_, restored_;
    std::fenv_t floatingPoint_{};
    bool pending_ = false;
public:
    bool Pending() const { return pending_; }
    bool Begin(game_interface::IGameMemory& mem) {
        if (pending_) return false;
        const auto size = mem.PresentationSnapshotSize();
        if (!size) return false;
        saved_.resize(size); restored_.resize(size);
        if (!mem.SavePresentationSnapshot(saved_) || std::fegetenv(&floatingPoint_) != 0) return false;
        mem.SetPresentationPreview(true);
        pending_ = true;
        return true;
    }
    bool Restore(game_interface::IGameMemory& mem) {
        if (!pending_ || !mem.LoadPresentationSnapshot(saved_)) return false;
        if (std::fesetenv(&floatingPoint_) != 0) return false;
        // 実際に戻した入力/戦闘/リプレイ末尾の全保存バイトを照合する。
        if (!mem.SavePresentationSnapshot(restored_) || saved_ != restored_) return false;
        mem.SetPresentationPreview(false);
        pending_ = false;
        return true;
    }
};
}
