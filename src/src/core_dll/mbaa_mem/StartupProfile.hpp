#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <cstdlib>
#include "core_dll/common/StartupTrace.hpp"
#include "core_dll/mbaa_mem/StartupPatch.hpp"
#include "core_dll/mbaa_mem/StartupSystemInfo.hpp"
#include "core_dll/mbaa_mem/StartupAssets.hpp"

namespace cccaster::game_memory::startup_profile {
// 診断用CALLサイト計測。レジスタ・flags・x87/SSE・引数の位置を維持する。
// 対象版の停止中入口で設置し、通常利用時は生成も書換えもしない。
__attribute__((force_align_arg_pointer)) inline void __cdecl Record(uint32_t tag) {
    const DWORD error = GetLastError();
    cccaster::domain::session::DebugLog("[BootCall] id=%u edge=%u qpcUs=%lld",
        tag >> 1, tag & 1u, cccaster::diagnostics::startup::QpcUs());
    SetLastError(error);
}
inline bool InstallCall(uint32_t site, uint32_t target, uint32_t id) {
    auto *code = reinterpret_cast<uint8_t*>(site);
    int32_t relative{};
    std::memcpy(&relative, code + 1, 4);
    if (code[0] != 0xE8 || site + 5 + relative != target) return false;
    auto *memory = static_cast<uint8_t*>(VirtualAlloc(nullptr, 256, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE));
    if (!memory) return false;
    std::vector<uint8_t> bytes;
    auto raw = [&](std::initializer_list<uint8_t> values) { bytes.insert(bytes.end(), values); };
    auto word = [&](uint32_t value) { auto *p = reinterpret_cast<uint8_t*>(&value); bytes.insert(bytes.end(),p,p+4); };
    auto branch = [&](uint8_t opcode, uintptr_t dest) { raw({opcode}); word(static_cast<uint32_t>(dest - (reinterpret_cast<uintptr_t>(memory) + bytes.size() + 4))); };
    auto record = [&](uint32_t tag) {
        raw({0x9C,0x60,0x89,0xE3,0x81,0xEC}); word(528); // pushfd,pushad,mov ebx,esp,sub esp
        raw({0x83,0xE4,0xF0,0x0F,0xAE,0x04,0x24,0x68}); word(tag); // align,fxsave,push tag
        branch(0xE8,reinterpret_cast<uintptr_t>(&Record));
        raw({0x83,0xC4,0x04,0x0F,0xAE,0x0C,0x24,0x89,0xDC,0x61,0x9D});
    };
    record(id * 2); branch(0xE8,target); record(id * 2 + 1); branch(0xE9,site+5);
    std::memcpy(memory,bytes.data(),bytes.size());
    DWORD old{}, ignored{};
    if (!VirtualProtect(memory,256,PAGE_EXECUTE_READ,&old) || !FlushInstructionCache(GetCurrentProcess(),memory,bytes.size()))
        return false;
    if (!VirtualProtect(code,5,PAGE_EXECUTE_READWRITE,&old)) return false;
    code[0] = 0xE9;
    const auto jump = static_cast<int32_t>(reinterpret_cast<uintptr_t>(memory) - (site+5));
    std::memcpy(code+1,&jump,4);
    if (!VirtualProtect(code,5,old,&ignored) || !FlushInstructionCache(GetCurrentProcess(),code,5)) ExitProcess(ERROR_WRITE_FAULT);
    cccaster::domain::session::DebugLog("[BootSite] id=%u site=%08X target=%08X",id,site,target);
    return true;
}
inline void Initialize() {
    if (!std::getenv("CCCASTER_STARTUP_PROFILE") || cccaster::diagnostics::startup::Baseline() ||
        !cccaster::diagnostics::startup::HasGate() ||
        !cccaster::game_memory::startup::MatchesMenuCode()) return;
    constexpr uint32_t sites[][2] = {
        {0x4C817E,0x4A3CD0},{0x4C8189,0x4A3DB0},{0x4C8193,0x4A3DF0},
        {0x4C819F,0x41E470},{0x4C81C8,0x4A3E20},{0x4C822A,0x4BEFC0},
        {0x4C8253,0x4BF8D0},{0x4A3DF0,0x4BEE20},{0x4A3DF9,0x414F10},
        {0x40DE14,0x48E030},{0x40DF3C,0x421A00},{0x40DF55,0x4BD230},
        {0x40DF71,0x4BD230},{0x40DF8C,0x4BD230},{0x40DFA8,0x4BD230},{0x40DFC2,0x41E530},
        {0x4A3E2D,0x4A38D0},{0x4A3E32,0x48EFF0},{0x4A3E37,0x422A70},
        {0x4A3E6B,0x4A3FC0},{0x4A3E92,0x4A3B70},{0x4A3E97,0x48F600},
        {0x4A3EA0,0x40D960},{0x4A3EA5,0x4A3F80},
        {0x41E531,0x4011E0},{0x41E536,0x4015F0},{0x41E571,0x421820},
        {0x41E580,0x4DD840},{0x41E585,0x4DDF40},{0x41E5B0,0x418030},
        {0x41E5B5,0x401110},{0x41E5BA,0x41E0C0},
        {0x4A1D1D,0x4A1D90},{0x4A1427,0x4A0980},{0x4A1486,0x4A0BE0},
        {0x4A146D,0x4A0500},{0x4A2052,0x4BFA80},{0x4A1F3F,0x4A2190},
        {0x4A21FA,0x417020},{0x41E0C6,0x4186C0},{0x41E0D8,0x4C6DE0},
        {0x41E0DD,0x407880},{0x41E0E2,0x4DDA10},{0x41E0E7,0x453170},
        {0x41E0EC,0x41B960},{0x41E0F1,0x4A6420},{0x41E0F6,0x4AF4E0},
        {0x41E0FB,0x4A4070},{0x41E100,0x4A8280},{0x41E105,0x41CFA0},
        {0x41E18A,0x44ACB0},{0x41E18F,0x4496C0},
        {0x4AF554,0x4C8B10},{0x4AF5B7,0x4BD2D0},{0x4AF64C,0x4C8B10},
        {0x4BD328,0x4DE5D2},{0x4BD37F,0x4BF060},{0x4BD393,0x4BD150},
        {0x4BD3F2,0x4DE5CC},{0x4BD40B,0x4BCCA0},
        {0x4C8BF6,0x4DB9B0},{0x4C8C20,0x413CE0},{0x4C8C7B,0x413FB0}
    };
    for (unsigned i=0; i<sizeof(sites)/sizeof(sites[0]); ++i) {
        if (sites[i][0] == 0x4A1F3F && startup_system_info::active) continue;
        if (sites[i][0] == 0x4BD3F2 && startup_assets::Active()) continue;
        if (!InstallCall(sites[i][0],sites[i][1],i)) ExitProcess(ERROR_BAD_EXE_FORMAT);
    }
}
}
