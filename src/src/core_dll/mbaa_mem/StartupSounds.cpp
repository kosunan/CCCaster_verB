#include "StartupSounds.hpp"
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/hook/HookBatch.hpp"
#include "core_dll/rollback/ReplayEffects.hpp"

namespace cccaster::game_memory::startup_sounds {
void Initialize(uint8_t mode) {
    if (mode > 1 || !diagnostics::startup::HasGate()) return;
    // 共通SEは元の41E0E2→4DDA10でBGM開始前に準備する。
    // BGMストリーミング中に未読SEをデコードすると初回選択時の音が飛ぶ。
    // 再生要求フックは軽量な再計算抑止だけにし、ファイル読込みを持ち込まない。
    hook_batch::Scope hooks(true);
    const bool installed = sync::InstallReplayEffects();
    domain::session::DebugLog("[StartupSounds] eager=1 count=200 replayHooks=%u", unsigned(installed));
}
}
