#include "core_dll/mbaa_mem/NativeFrameWait.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "shared_contracts/ProcessMemory.hpp"

namespace cccaster::game_memory::native_frame_wait {
patch::Result Enable() {
    static bool installed = false;
    if (installed) return {};
    if (!game_build::RuntimeValidated())
        return {patch::Error::Mismatch, 0, SleepSite, "frame_wait_build"};
    patch::ProcessMemory memory;
    const auto result = Apply(memory);
    installed = bool(result);
    domain::session::DebugLog(
        "[NativeFrameWait] installed=%d patches=2 apiHooks=0 clockScale=1 patch=%s address=%08X error=%s rollbackFailed=%d",
        installed, result.name, unsigned(result.address), patch::Name(result.error), result.rollbackFailed);
    return result;
}
} // namespace cccaster::game_memory::native_frame_wait
