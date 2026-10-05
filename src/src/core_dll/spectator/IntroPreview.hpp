#pragma once
#include "core_dll/mbaa_mem/IGameMemory.hpp"
#include <cfenv>
#include <vector>

namespace cccaster::spectator {
// mode=1へ変わった直後の完成画像はまだロード画面。先頭画像を1回だけ作り、
// 表示後に更新前へ戻して確定入力を待つ。試合の再生位置には数えない。
class IntroPreview {
    std::vector<char> saved_;
    std::fenv_t fp_{};
    bool pending_ = false;
public:
    bool Pending() const { return pending_; }
    bool Begin(game_interface::IGameMemory &mem) {
        if (pending_ || mem.IntroState() != 2) return false;
        const auto size = mem.SnapshotSize();
        if (!size) return false;
        saved_.resize(size);
        if (!mem.SaveSnapshot(saved_)) return false;
        std::fegetenv(&fp_);
        if (!mem.SetIntroPreview(true)) return false;
        mem.WriteInput({}, {});
        pending_ = true;
        return true;
    }
    bool Restore(game_interface::IGameMemory &mem) {
        if (!pending_) return false;
        const bool ok = mem.LoadSnapshot(saved_);
        std::fesetenv(&fp_);
        mem.SetIntroPreview(false);
        pending_ = false;
        return ok;
    }
};
}
