#include "NativeLoopOptimization.hpp"
#include "NativeLoopKernels.hpp"
#include "NativeLoopSignatures.hpp"
#include "core_dll/common/DebugLog.hpp"
#include "core_dll/mbaa_mem/GameBuildGuard.hpp"
#include "core_dll/hook/DxHook.hpp"
#include "core_dll/hook/ScenePairMerge.hpp"
#include "shared_contracts/ProcessMemory.hpp"
#include <array>
#include <cstdlib>
#include <vector>

// 診断専用。通常経路のC++検索トレースとは独立し、元の形状判定を実行する。
extern "C" {
uint32_t cc_loop_verify_entries = 0, cc_loop_verify_candidates[2]{}, cc_loop_verify_hits[2]{};
__attribute__((naked)) void cc_loop_verify_entry() {
    __asm__ __volatile__("pushfl\n\tincl _cc_loop_verify_entries\n\tpopfl\n\t"
        "subl $0xd0,%esp\n\tjmp *1f\n\t.p2align 2\n\t1: .long 0x46f2f6\n\t");
}
__attribute__((naked)) void cc_loop_verify_candidate() {
    // EBXは元関数の形状iterator。元の3引数を複製して同じネイティブ判定を呼ぶ。
    __asm__ __volatile__("pushl 12(%esp)\n\tpushl 12(%esp)\n\tpushl 12(%esp)\n\t"
        "call *3f\n\taddl $12,%esp\n\txorl %edx,%edx\n\t"
        "cmpl $0x67bdec,8(%ebx)\n\tjb 1f\n\tcmpl $0x74604c,8(%ebx)\n\tjae 1f\n\tincl %edx\n\t"
        "1: incl _cc_loop_verify_candidates(,%edx,4)\n\ttestl %eax,%eax\n\tje 2f\n\t"
        "incl _cc_loop_verify_hits(,%edx,4)\n\t2: ret\n\t.p2align 2\n\t3: .long 0x46e5c0\n\t");
}
}

namespace nl = cccaster::game_memory::native_loops;
extern "C" { bool cc_loop_trace = false; }
namespace {
bool vectorSound = false;
std::array<uint64_t, 8> calls{}, skipped{};
unsigned frames = 0;
void Verify() {
    static const bool requested = std::getenv("CCCASTER_NATIVE_LOOP_VERIFY") != nullptr;
    if (!requested) return;
    static bool attempted = false, installed = false;
    static unsigned sample = 0, maxLive = 0;
    if (!attempted) {
        attempted = true;
        if (!cccaster::game_build::RuntimeValidated() || uintptr_t(GetModuleHandleW(nullptr)) != 0x400000) return;
        constexpr uint8_t entry[]{0x81,0xec,0xd0,0,0,0}, call[]{0xe8,0x8e,0xf1,0xff,0xff};
        std::array<uint8_t,6> jump{0xe9,0,0,0,0,0x90};
        std::array<uint8_t,5> replacement{0xe8,0,0,0,0};
        auto relative = uint32_t(uintptr_t(&cc_loop_verify_entry) - 0x46f2f5);
        std::memcpy(jump.data()+1, &relative, 4);
        relative = uint32_t(uintptr_t(&cc_loop_verify_candidate) - 0x46f432);
        std::memcpy(replacement.data()+1, &relative, 4);
        const std::array<cccaster::patch::Spec,2> specs{{
            {"verify_collision_entry",0x46f2f0,entry,jump},
            {"verify_collision_candidate",0x46f42d,call,replacement}}};
        const auto result = cccaster::patch::Apply(specs);
        installed = bool(result);
        cccaster::domain::session::DebugLog("[NativeVerifyInstall] enabled=%u error=%s", unsigned(installed), cccaster::patch::Name(result.error));
        if (result.rollbackFailed) ExitProcess(ERROR_WRITE_FAULT);
    }
    if (!installed) return;
    unsigned live = 0;
    for (unsigned i = 0; i < nl::ObjectCount; ++i)
        live += reinterpret_cast<const uint8_t*>(0x67bde8)[i*nl::ObjectStride] != 0;
    maxLive = (std::max)(maxLive, live);
    if (++sample % 120 == 0)
        cccaster::domain::session::DebugLog("[NativeVerify] sample=%u entries=%u players=%u objects=%u playerHits=%u objectHits=%u live=%u maxLive=%u",
            sample, cc_loop_verify_entries, cc_loop_verify_candidates[0], cc_loop_verify_candidates[1],
            cc_loop_verify_hits[0], cc_loop_verify_hits[1], live, maxLive);
}
void Count(unsigned group, uint32_t amount = 0) {
    if (cc_loop_trace) { ++calls[group]; skipped[group] += amount; }
}
uint64_t Hash(uintptr_t address, size_t size) {
    auto* bytes = reinterpret_cast<const uint8_t*>(address);
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}
}

extern "C" {
__attribute__((force_align_arg_pointer)) uint32_t __cdecl cc_loop_collision(uint32_t first, const uint8_t* actor) {
    const auto next = nl::NextCollision(reinterpret_cast<const uint8_t*>(0x67bde8), first, actor);
    Count(0, next - first);
    return next;
}
// 46F3B8: プレイヤー4枠の走査は元のまま。オブジェクト側だけ候補へ進める。
// [元ESP+14h]が枠番号。EBPはオブジェクト側では参照されず、出口で元値へ復帰する。
__attribute__((naked)) void cc_loop_collision_gate() {
    __asm__ __volatile__(
        "cmpb $0,-4(%esi)\n\tjne 3f\n\tcmpb $0,_cc_loop_trace\n\tjne 4f\n\t"
        "movl 0x14(%esp),%eax\n\t5: incl %eax\n\taddl $0x33c,%esi\n\t"
        "cmpl $1000,%eax\n\tjae 6f\n\tcmpb $0,-4(%esi)\n\tje 5b\n\t"
        "movl %eax,0x14(%esp)\n\tjmp 3f\n\t6: movl %eax,0x14(%esp)\n\tjmp 1f\n\t"
        "4: pushfl\n\tpushal\n\tmovl 0x38(%esp),%eax\n\tpushl %ebx\n\tpushl %eax\n\t"
        "call _cc_loop_collision\n\taddl $8,%esp\n\tmovl %eax,0x38(%esp)\n\t"
        "imull $0x33c,%eax,%edx\n\taddl $0x67bdec,%edx\n\tmovl %edx,4(%esp)\n\tmovl %edx,0(%esp)\n\t"
        "popal\n\tpopfl\n\tcmpl $0x74604c,%esi\n\tjae 1f\n\tjmp *8f\n\t"
        "1: jmp *9f\n\t3: jmp *7f\n\t.p2align 2\n\t7: .long 0x46f3c2\n\t8: .long 0x46f3dc\n\t9: .long 0x46f463\n\t");
}

// 使用中の枠は元コードへ即座に戻す。空き枠が連続するときだけまとめて進める。
#define CC_LIVE_HELPER(name, end, offset, group) \
__attribute__((force_align_arg_pointer)) uintptr_t __cdecl name(uintptr_t cursor) { \
    const auto next = nl::NextLive(cursor, end, offset); \
    Count(group, (next - cursor) / nl::ObjectStride); return next; }
CC_LIVE_HELPER(cc_loop_live_a, 0x7461c2, 0x17a, 2)
CC_LIVE_HELPER(cc_loop_live_b, 0x746314, 0x2cc, 3)
CC_LIVE_HELPER(cc_loop_live_c, 0x74604c, 4, 4)
CC_LIVE_HELPER(cc_loop_live_d, 0x74604c, 4, 5)
#undef CC_LIVE_HELPER

#define CC_LIVE_GATE(name, helper, reg, saved, offset, end, body, done) \
__attribute__((naked)) void name() { \
    __asm__ __volatile__( \
        "cmpb $0," offset "(" reg ")\n\tjne 2f\n\t" \
        "cmpb $0,_cc_loop_trace\n\tjne 4f\n\t" \
        "3: addl $0x33c," reg "\n\tcmpl $" end "," reg "\n\tjae 1f\n\t" \
        "cmpb $0," offset "(" reg ")\n\tje 3b\n\tjmp 2f\n\t4: " \
        "pushfl\n\tpushal\n\tpushl " reg "\n\tcall _" #helper "\n\taddl $4,%esp\n\t" \
        "movl %eax," saved "(%esp)\n\tpopal\n\tpopfl\n\t" \
        "cmpl $" end "," reg "\n\tjae 1f\n\t" \
        "2: jmp *8f\n\t1: jmp *9f\n\t.p2align 2\n\t8: .long " body "\n\t9: .long " done "\n\t"); }
CC_LIVE_GATE(cc_loop_live_a_gate, cc_loop_live_a, "%ebp", "8", "-0x17a", "0x7461c2", "0x453f89", "0x453fd9")
CC_LIVE_GATE(cc_loop_live_b_gate, cc_loop_live_b, "%edi", "0", "-0x2cc", "0x746314", "0x453fe9", "0x454036")
CC_LIVE_GATE(cc_loop_live_c_gate, cc_loop_live_c, "%esi", "4", "-4", "0x74604c", "0x45405e", "0x454087")
CC_LIVE_GATE(cc_loop_live_d_gate, cc_loop_live_d, "%esi", "4", "-4", "0x74604c", "0x45409c", "0x4540b8")
#undef CC_LIVE_GATE

__attribute__((force_align_arg_pointer)) void __cdecl cc_loop_commands(uint8_t* actor, unsigned phase) {
    Count(6);
    nl::RunCommands(phase,
        [=] { return *reinterpret_cast<const uint8_t* const*>(actor + 0x31c); },
        [](const uint8_t* state) { return state[0x40]; },
        [](const uint8_t* state, unsigned i) { return (*reinterpret_cast<const uint32_t* const* const*>(state + 0x44))[i]; },
        [](const uint32_t* entry) { return reinterpret_cast<const uint8_t*>(0x54ea00)[*entry]; },
        [=](const uint32_t* entry) { reinterpret_cast<void (__cdecl*)(uint8_t*, const uint32_t*)>(0x4693e0)(actor, entry); });
}
// 4684F7はポーズ/スロー判定後。ESI保存と8バイト整列は元のプロローグが済ませている。
__attribute__((naked)) void cc_loop_commands_gate() {
    __asm__ __volatile__(
        "movl 0x31c(%edi),%eax\n\tcmpb $0,0x40(%eax)\n\tje 1f\n\t"
        "pushl %ebx\n\tpushl %edi\n\tcall _cc_loop_commands\n\taddl $8,%esp\n\t"
        "1: xorl %eax,%eax\n\tpopl %esi\n\tmovl %ebp,%esp\n\tpopl %ebp\n\tret\n\t");
}

__attribute__((force_align_arg_pointer)) uint32_t __cdecl cc_loop_sound(uint32_t first) {
    const std::span<const uint8_t> flags(reinterpret_cast<const uint8_t*>(0x76e008), nl::SoundCount);
    const auto next = vectorSound ? nl::NextSoundVector(flags, first) : nl::NextSoundScalar(flags, first);
    Count(7, next - first);
    return next;
}
// 再生そのもの・ReplayEffectsの抑止フック・フラグの履歴コピー/全消去は元のまま。
__attribute__((naked)) void cc_loop_sound_gate() {
    __asm__ __volatile__(
        "pushfl\n\tpushal\n\tpushl %esi\n\tcall _cc_loop_sound\n\taddl $4,%esp\n\t"
        "movl %eax,4(%esp)\n\tpopal\n\tpopfl\n\tcmpl $1500,%esi\n\tjae 1f\n\t"
        "jmp *8f\n\t1: jmp *9f\n\t.p2align 2\n\t8: .long 0x4de210\n\t9: .long 0x4de22e\n\t");
}

// EAXとXMM0はこの関数では生存値を持たない。16バイトの検索をブリッジ内で完結し、
// 要求が多い場合も要求ごとのC++呼出し・全レジスタ退避を避ける。
__attribute__((naked)) void cc_loop_sound_vector_gate() {
    __asm__ __volatile__(
        "cmpb $0,_cc_loop_trace\n\tjne _cc_loop_sound_gate\n\t"
        "cmpl $1500,%esi\n\tjae 4f\n\tcmpb $1,0x76e008(%esi)\n\tje 5f\n\t"
        "1: cmpl $1484,%esi\n\tja 3f\n\t"
        "movdqu 0x76e008(%esi),%xmm0\n\tpcmpeqb 7f,%xmm0\n\tpmovmskb %xmm0,%eax\n\t"
        "testl %eax,%eax\n\tjne 2f\n\taddl $16,%esi\n\tjmp 1b\n\t"
        "2: bsfl %eax,%eax\n\taddl %eax,%esi\n\tjmp *8f\n\t"
        "3: cmpl $1500,%esi\n\tjae 4f\n\tcmpb $1,0x76e008(%esi)\n\tje 5f\n\tincl %esi\n\tjmp 3b\n\t"
        "4: jmp *9f\n\t5: jmp *8f\n\t.p2align 4\n\t"
        "7: .byte 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1\n\t"
        "8: .long 0x4de210\n\t9: .long 0x4de22e\n\t");
}

__attribute__((force_align_arg_pointer)) HRESULT __cdecl cc_loop_scene_pair(IDirect3DDevice9* device) {
    Count(1);
    return cccaster::game_interface::DxHook::AdjacentScenePair(device);
}
__attribute__((naked)) void cc_loop_scene_gate() {
    __asm__ __volatile__(
        "movl (%ebx),%edx\n\tmovl 4(%edx),%ebp\n\tpushl %ebp\n\t"
        "call _cc_loop_scene_pair\n\taddl $4,%esp\n\tjmp *8f\n\t.p2align 2\n\t8: .long 0x4be366\n\t");
}
}

namespace cccaster::game_memory::native_loops {
void Install() {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;
    cc_loop_trace = std::getenv("CCCASTER_NATIVE_LOOP_TRACE") != nullptr;
    if (std::getenv("CCCASTER_DISABLE_NATIVE_LOOPS")) {
        domain::session::DebugLog("[NativeLoops] disabled=1"); return;
    }
    if (!game_build::RuntimeValidated() || reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x400000) return;
    for (const auto& range : signatures::Ranges) {
        if (Hash(range.address, range.size) != range.hash) {
            domain::session::DebugLog("[NativeLoops] signature mismatch address=%08X; unchanged", unsigned(range.address));
            return;
        }
    }
    vectorSound = IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE) != FALSE;
    const std::array<uintptr_t, 8> targets{
        uintptr_t(&cc_loop_collision_gate), uintptr_t(&cc_loop_scene_gate),
        uintptr_t(&cc_loop_live_a_gate), uintptr_t(&cc_loop_live_b_gate),
        uintptr_t(&cc_loop_live_c_gate), uintptr_t(&cc_loop_live_d_gate),
        uintptr_t(&cc_loop_commands_gate), vectorSound ? uintptr_t(&cc_loop_sound_vector_gate) : uintptr_t(&cc_loop_sound_gate)};
    std::array<std::vector<uint8_t>, 8> replacement;
    std::vector<patch::Spec> specs;
    // ScenePairMergeが命令照合できた場合だけ、既存の統合を呼出し元側へまとめる。
    const bool scene = game_interface::scene_pair_merge::endCaller == 0x4be35a;
    // 試験専用の5群選択。未指定は従来どおり全群。速度比較で役割を交代できる。
    unsigned mask = 31;
    if (const char* value = std::getenv("CCCASTER_TEST_NATIVE_MASK")) {
        char* end = nullptr;
        const auto parsed = std::strtoul(value, &end, 10);
        if (end != value && *end == '\0' && parsed <= 31) mask = unsigned(parsed);
    }
    constexpr unsigned bits[]{1,2,4,4,4,4,8,16};
    for (size_t i = 0; i < targets.size(); ++i) {
        if (!(mask & bits[i])) continue;
        if (i == 1 && !scene) continue;
        const auto& site = signatures::Sites[i];
        replacement[i].assign(site.original.size(), 0x90);
        replacement[i][0] = 0xe9;
        const auto relative = uint32_t(targets[i] - (site.address + 5));
        std::memcpy(replacement[i].data() + 1, &relative, 4);
        specs.push_back({site.name, site.address, site.original, replacement[i]});
    }
    const auto result = patch::Apply(specs);
    domain::session::DebugLog("[NativeLoops] enabled=%u scene=%u sse2=%u patch=%s error=%s rollbackFailed=%u",
        unsigned(bool(result)), unsigned(scene), unsigned(vectorSound), result.name,
        patch::Name(result.error), unsigned(result.rollbackFailed));
    if (result.rollbackFailed) ExitProcess(ERROR_WRITE_FAULT);
    domain::session::DebugLog("[NativeLoopMask] requested=%u applied=%u", mask, result ? mask & (scene ? 31u : 29u) : 0u);
}
void Trace() {
    Verify();
    if (!cc_loop_trace || ++frames % 300 != 0) return;
    for (unsigned i = 0; i < calls.size(); ++i)
        domain::session::DebugLog("[NativeLoopCount] group=%u calls=%llu skipped=%llu", i,
            static_cast<unsigned long long>(calls[i]), static_cast<unsigned long long>(skipped[i]));
}
}
