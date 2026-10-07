#include "core_dll/mbaa_mem/NativeInputWrites.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/common/DeferredNumericLog.hpp"
#include "core_dll/common/ScriptedInput.hpp"
#include "core_dll/common/InputTrace.hpp"
#include <MinHook.h>
#include <cstring>

namespace {
cccaster::sync::InputWriteHistory history;
bool enabled = false, installed = false;
struct Registers { uint32_t edi,esi,ebp,esp,ebx,edx,ecx,eax,flags; };
template<class T> T Read(uintptr_t address) { return *reinterpret_cast<const T*>(address); }
bool Trace() { return cccaster::testing::IsInputTraceEnabled() || cccaster::testing::IsScriptedInputEnabled(); }
}
extern "C" {
void* cc_native_input_before_original = nullptr;
void* cc_native_input_after_original = nullptr;
__attribute__((force_align_arg_pointer)) void cc_native_input_before(const Registers* r) {
    if (!enabled || r->edx > 1 || r->ecx < 0x2E6) return;
    const auto actor = uintptr_t(r->ecx - 0x2E6);
    auto* w = history.Current(actor);
    if (!w) return;
    auto& c = w->context;
    c.actor = actor; c.source = uint8_t(r->edx);
    c.previousButtons = Read<uint32_t>(actor+0x2E8);
    c.facing = Read<uint8_t>(actor+0x310);
    c.reverse = Read<uint16_t>(actor+0x196);
    const auto player = Read<uintptr_t>(actor+0x324);
    const auto definition = Read<uintptr_t>(actor+0x31C);
    c.macroMode = Read<uint32_t>(player+0xAF8);
    c.stateKind = Read<uint8_t>(Read<uintptr_t>(definition+0x38)+0xC);
    const auto input = Read<uintptr_t>(0x76E6AC);
    std::memcpy(c.masks.data(),reinterpret_cast<void*>(input+0x68+c.source*0x80),sizeof(c.masks));
    w->applied = cccaster::game_interface::GameInput{
        uint16_t(Read<uint32_t>(input+0x18+c.source*0x14)),Read<uint16_t>(input+0x24+c.source*0x14)}.Pack();
    // Replay playback replaces these fields after 46B720. Netplay uses recording, not playback.
    w->prepared = Read<uint32_t>(0x77BF2C) != 2;
}
__attribute__((force_align_arg_pointer)) void cc_native_input_after(const Registers* r) {
    if (!enabled) return;
    auto* w = history.Current(r->edi);
    if (!w) return;
    const auto actor = uintptr_t(r->edi);
    w->actual = {Read<uint8_t>(actor+0x2E6),Read<uint8_t>(actor+0x2E7),
                 Read<uint32_t>(actor+0x2E8),Read<uint32_t>(actor+0x2EC)};
    w->observed = true;
    if (!w->prepared) return;
    const auto expected = cccaster::sync::ExpectedActorInput(w->context,w->applied);
    const bool equal = w->actual == expected;
    history.fault |= !equal;
    const bool corrected = history.Corrected(actor,w->applied,equal);
    if (Trace() || !equal)
        cccaster::diagnostics::DeferredNumericLog::Log(
            "[NativeInputWrite] WRITE frame=%u player=%u address=%08X raw=%08X direction=%u buttons=%08X released=%08X equal=%u",
            history.Active(),unsigned(w->context.source+1),unsigned(actor+0x2E7),w->applied,
            unsigned(w->actual.direction),w->actual.buttons,w->actual.released,unsigned(equal));
    if (corrected && Trace())
        cccaster::diagnostics::DeferredNumericLog::Log(
            "[NativeInputWrite] CORRECTED frame=%u player=%u address=%08X raw=%08X direction=%u buttons=%08X released=%08X",
            history.Active(),unsigned(w->context.source+1),unsigned(actor+0x2E7),w->applied,
            unsigned(w->actual.direction),w->actual.buttons,w->actual.released);
}
// Preserve registers, flags, x87 and SSE state around observation of native stores.
#define CC_INPUT_OBSERVER(name,callback,original) \
__attribute__((naked)) void name() { __asm__ __volatile__( \
    "pushfl; pushal; movl %esp,%esi; subl $528,%esp; andl $-16,%esp; fxsave (%esp); " \
    "subl $12,%esp; pushl %esi; call _" #callback "; addl $16,%esp; fxrstor (%esp); " \
    "movl %esi,%esp; popal; popfl; jmp *_" #original); }
CC_INPUT_OBSERVER(cc_native_input_before_hook,cc_native_input_before,cc_native_input_before_original)
CC_INPUT_OBSERVER(cc_native_input_after_hook,cc_native_input_after,cc_native_input_after_original)
#undef CC_INPUT_OBSERVER
}
namespace cccaster::game_interface::native_input_writes {
bool Configure(bool active) {
    enabled = false;
    history.Reset();
    if (!active) return true;
    if (!installed) {
        if (!game_build::RuntimeValidated()) return false;
        const uint8_t before[]{0x8B,0xC2,0x6B,0xC0,0x2C};
        // The observation runs immediately after MOV [EDI+2E7],AL at 46D908.
        const uint8_t after[]{0x88,0x87,0xE7,0x02,0,0,0x8B,0xC5,0xE8,0xFB,0xFC,0xFF,0xFF};
        if (std::memcmp(reinterpret_cast<void*>(0x46B720),before,sizeof(before)) ||
            std::memcmp(reinterpret_cast<void*>(0x46D908),after,sizeof(after))) return false;
        const auto init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
        if (MH_CreateHook(reinterpret_cast<void*>(0x46B720),reinterpret_cast<void*>(cc_native_input_before_hook),&cc_native_input_before_original) != MH_OK ||
            MH_CreateHook(reinterpret_cast<void*>(0x46D90E),reinterpret_cast<void*>(cc_native_input_after_hook),&cc_native_input_after_original) != MH_OK ||
            MH_EnableHook(reinterpret_cast<void*>(0x46B720)) != MH_OK ||
            MH_EnableHook(reinterpret_cast<void*>(0x46D90E)) != MH_OK) return false;
        installed = true;
    }
    enabled = true;
    domain::session::DebugLog("[NativeInputWrite] ACTIVE before=0046B720 after=0046D90E p1=0055541B p2=00555F17");
    return true;
}
void Begin(uint32_t frame) { if (enabled) history.Begin(frame); }
sync::InputWriteHistory* History() { return enabled ? &history : nullptr; }
}
