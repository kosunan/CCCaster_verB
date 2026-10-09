#include "core_dll/mbaa_mem/RealGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/mbaa_mem/TrainingAnimation.hpp"
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
    // 初期化入口で入力・状態配置の互換性を照合済み。
    // パッチ後のコードを再照合せず、確認結果と固定基底を使用する。
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base != 0x400000 || !ReadableImageRange(base, 4096)) return false;
    return game_build::RuntimeValidated();
}
}

bool RealGameMemory::IsPauseMenuOpen() const {
    return IsAvailable() && GameMode() == CC_GAME_MODE_IN_GAME &&
        (*CC_PAUSE_FLAG_ADDR != 0 || *CC_TRAINING_PAUSE_ADDR != 0);
}
bool RealGameMemory::IsTrainingDummy() const {
    // kosunan/MBAACCTraining e4f2f976: Fn_01の2byte読取とFn_03の5/-1判定に合わせる。
    return IsAvailable() && GameMode() == CC_GAME_MODE_IN_GAME &&
        IsDummyEnemyStatus(Read<int16_t>(reinterpret_cast<uintptr_t>(CC_DUMMY_STATUS_ADDR)));
}
bool RealGameMemory::IsTrainingRecording() const {
    return IsAvailable() && GameMode() == CC_GAME_MODE_IN_GAME &&
        Read<int16_t>(reinterpret_cast<uintptr_t>(CC_DUMMY_STATUS_ADDR)) == CC_DUMMY_STATUS_RECORD;
}

TrainingFrameSample RealGameMemory::ReadTrainingFrame() const {
    // ゲームスレッドの通常更新後だけ。学習表示モードの選別は呼出元で行う。
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
    // 0x46192DはDWORDの非zero比較。下位BYTEだけでは256等を取り落とす。
    sample.globalFreeze = Read<int32_t>(0x562A48) != 0;
    sample.timerSuppressed = Read<uint32_t>(0x55DF00) != 0;
    sample.paused = Read<uint8_t>(0x55D203) != 0 || Read<uint32_t>(0x562A64) != 0;
    sample.stopped = sample.globalFreeze || sample.paused;
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
        // kosunan attack_status=0x555454、ETM ActorData::attackDataPtr=+0x320。
        // 攻撃属性の存在と、現在の状態データの攻撃矩形を別々に読む。
        sample.attacking[side] = Read<uint32_t>(actor + 0x320) != 0;
        sample.pattern[side] = Read<uint32_t>(actor + 0xC);
        sample.blockstun[side] = Read<uint8_t>(actor + 0x177) != 0;
        auto &detail = sample.detail[side];
        detail.valid = true;
        detail.thrown = Read<uint8_t>(actor + 0x172) != 0;
        detail.strikeProtected = Read<uint8_t>(actor + 0x181) != 0;
        detail.throwProtected = Read<uint8_t>(actor + 0x182) != 0;
        detail.stunRemaining = Read<int32_t>(actor + 0x1A8);
        // 0x4637C1..0x4637CFはsigned16比較。負値を巨大な残量に変換しない。
        detail.untechTotal = Read<int16_t>(actor + 0x18A);
        detail.untechElapsed = Read<int16_t>(actor + 0x18C);
        detail.patternFrame = Read<uint32_t>(actor + 0x330);
        detail.hitstop = Read<uint8_t>(actor + kActorHitstopOffset);
        detail.receivedHitstop = Read<uint8_t>(actor + kActorReceivedHitstopOffset);
        detail.remainingHits = Read<uint8_t>(actor + 0x176);
        detail.reservedPattern = Read<int16_t>(actor + 0x308);
        // kosunanの浮遊下線と同じ条件。stanceだけでは着地境界を取り落とす。
        detail.airborne = Read<int16_t>(actor + 0x122) != 0 ||
            Read<int32_t>(actor + 0x108) != 0 || Read<int32_t>(actor + 0x114) != 0;
        ReadTrainingAnimation(Read<uint32_t>(actor + 0x31C), sample.attacking[side], detail,
            [](uint32_t address, void *destination, size_t length) {
                if (address < 0x10000 || length > UINT32_MAX - address) return false;
                SIZE_T copied = 0;
                return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address),
                                         destination, length, &copied) && copied == length;
            });
        // owner側の補助値を参照する。交代キャラでも本体側と同じ資格条件。
        const unsigned owner = Read<uint8_t>(actor + 0x2F0);
        detail.guardEligible = owner < 2 && TrainingRecoveryGuardEligible(detail,
            Read<int16_t>(actor + 0x1EA), Read<uint32_t>(kAux + owner * kAuxStride + 0x10),
            Read<uint8_t>(actor + 0x1B2));
        sample.playerStopped[side] = Read<int32_t>(kFreeze + side * kFreezeStride) != 0 ||
            detail.hitstop != 0 || detail.receivedHitstop != 0;
        sample.stopped = sample.stopped || sample.playerStopped[side];
    }
    sample.valid = true;
    return sample;
}
} // namespace cccaster::game_interface
