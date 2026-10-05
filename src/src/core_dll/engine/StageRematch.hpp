#pragma once
#include "core_dll/sync/SelectionState.hpp"

namespace cccaster::domain::scene {
// ONCEの合意後だけ保持する。通常のキャラセレに戻る場合は新規選択になる。
struct StageRematch {
    bool active = false;
    uint32_t selectionEpoch = 0;
    core::sync::SelectionState saved{};
    bool Begin(int result, bool host, const core::sync::SelectionState &local,
               const core::sync::SelectionState &peer) {
        const auto &owner = host ? local : peer;
        active = result == 0 && owner.stageConfirmed && owner.randomStage;
        selectionEpoch = 0;
        saved = active ? local : core::sync::SelectionState{};
        return active;
    }
    core::sync::SelectionState Restore(uint32_t epoch, bool host, uint32_t stage) const {
        core::sync::SelectionState next;
        next.epoch = epoch; next.revision = 1;
        next.selector = core::sync::SelectionState::CharacterCell(saved.character);
        next.character = saved.character; next.moon = saved.moon; next.color = saved.color;
        next.confirmed = 1;
        next.delay = saved.delay; next.rollback = saved.rollback;
        if (host) { next.stage = stage; next.stageConfirmed = 1; next.randomStage = 1; }
        return next;
    }
};
}
