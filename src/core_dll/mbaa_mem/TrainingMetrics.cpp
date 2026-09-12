#include "core_dll/mbaa_mem/RealGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "shared_contracts/GameBuild.hpp"
#include <windows.h>
#include <cstring>

namespace cccaster::game_interface {
namespace {
// 新規の読取値はETM 038887d7d8e6e70963ce9eb5780a25ac35cf1cacの
// Common.h/DebugInfo.hとdocs/memory/etm_fields.mdに対応。書込みは行わない。
constexpr uintptr_t kAux = 0x557DB8;
constexpr uintptr_t kAuxStride = 0x20C;
constexpr uintptr_t kInactionOffset = 0x208;
constexpr uintptr_t kPlayer = 0x555130;
constexpr uintptr_t kPlayerStride = 0xAFC;
// PlayerDataはEffectDataを継承: exists DWORDの後にActorData subObj。
// docs/memory/etm_fields.mdのActorData offsetは本体ではなく+4の基点。
constexpr uintptr_t kActorOffset = 4;
constexpr uintptr_t kActorHitstopOffset = 0x16E;         // BYTE: P1 VA 0x5552A2
constexpr uintptr_t kActorReceivedHitstopOffset = 0x1A0; // BYTE: P1 VA 0x5552D4
static_assert(kPlayer + kActorOffset + kActorHitstopOffset == 0x5552A2);
static_assert(kPlayer + kActorOffset + kActorReceivedHitstopOffset == 0x5552D4);
// Common.h: adP1Freeze/adP2Freeze。FrameBar::CalculateAdvantageは
// 操作キャラが交代しても本体P1/P2側のintを参照する（ActorDataとは別領域）。
constexpr uintptr_t kFreeze = 0x558908;
constexpr uintptr_t kFreezeStride = 0x30C;

bool ReadableImageRange(uintptr_t start, size_t length) {
    const uintptr_t end = start + length;
    for (uintptr_t cursor = start; cursor < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void *>(cursor), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            info.AllocationBase != reinterpret_cast<void *>(0x400000)) return false;
        const DWORD protection = info.Protect & 0xFF;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

template<class T> T Read(uintptr_t address) {
    T value{};
    std::memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
    return value;
}

bool SupportedImage() {
    // DLL入口では既存GameBuildGuardが.text全体も照合済み。
    // フック適用後にコードハッシュを取り直さず、ここでは版・固定基底を確認する。
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base != 0x400000 || !ReadableImageRange(base, 4096)) return false;
    game_build::PeIdentity identity;
    return game_build::ReadHeaders({reinterpret_cast<const uint8_t *>(base), 4096}, identity) &&
        game_build::IdentifyHeaders(identity) == game_build::Edition::Carnival140;
}
}

TrainingFrameSample RealGameMemory::ReadTrainingFrame() const {
    // ゲームスレッドの通常更新後だけ。モード1/2の選別は呼出元で行う。
    TrainingFrameSample sample;
    static const bool supported = SupportedImage();
    if (!supported || !ReadableImageRange(0x54EEE8, 4) ||
        !ReadableImageRange(0x555000, 0xB000) ||
        !ReadableImageRange(0x562A00, 0x100)) return sample;
    if (Read<uint32_t>(0x54EEE8) != CC_GAME_MODE_IN_GAME ||
        Read<uint8_t>(0x55D20B) != 0) return sample;
    // FrameBarは両時計をint*で読む。32bit幅は同じで、本実装はwrapも
    // 不連続として破棄するため非負のフレーム番号へ格納する。
    sample.trueFrame = Read<uint32_t>(0x562A40);       // adTrueFrameCount / CC_REAL_TIMER
    sample.simulationFrame = Read<uint32_t>(0x55D1CC); // adFrameCount（CC_WORLD_TIMERと別）
    sample.round = Read<uint32_t>(0x5550E0);           // CC_ROUND_COUNT
    // FrameBarのGlobalFreezeはchar読取。共通pauseはuint8_t、training pauseはuint32_t。
    sample.stopped = Read<uint8_t>(0x562A48) != 0 ||
        Read<uint8_t>(0x55D203) != 0 || Read<uint32_t>(0x562A64) != 0;
    for (unsigned side = 0; side < 2; ++side) {
        const uintptr_t aux = kAux + side * kAuxStride;
        const int active = Read<int32_t>(aux);
        // 操作キャラは自分側の本体またはパートナー。未知値を別枠へ丸めない。
        if (active != static_cast<int>(side) && active != static_cast<int>(side + 2)) return {};
        sample.activeCharacter[side] = active;
        sample.inactionable[side] = Read<int32_t>(aux + kInactionOffset);
        const uintptr_t player = kPlayer + static_cast<unsigned>(active) * kPlayerStride;
        if (Read<uint32_t>(player) == 0) return {};
        const uintptr_t actor = player + kActorOffset;
        sample.stopped = sample.stopped || Read<int32_t>(kFreeze + side * kFreezeStride) != 0 ||
            Read<uint8_t>(actor + kActorHitstopOffset) != 0 ||
            Read<uint8_t>(actor + kActorReceivedHitstopOffset) != 0;
    }
    sample.valid = true;
    return sample;
}
} // namespace cccaster::game_interface
