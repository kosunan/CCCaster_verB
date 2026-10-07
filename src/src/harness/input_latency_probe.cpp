// MBAACC 1.07の通常更新・入力変換・描画投入・Present入口を読取り専用で観測。
// 製品DLLとは独立。標準側は起動中だけ正規Training入口と入力確認を使う。
// tool-native-inputは製品と同じ描画・待機で、標準の生入力書込み命令だけを復帰する。
// 派生入力・キャラ状態は書き換えない。製品DLLへ組み込まない測定専用コード。
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "MinHook.h"
#include "shared_contracts/GameBuild.hpp"
#include "core_dll/mbaa_mem/StartupDirectEntry.hpp"
#include "core_dll/mbaa_mem/StartupFileRead.hpp"
#include "core_dll/mbaa_mem/StartupSystemInfo.hpp"
#include <cstdlib>
void HookLog(const char* message) {
    if (auto* file=std::fopen("input_latency_boot.log","a")) { std::fprintf(file,"%s\n",message); std::fclose(file); }
}
struct Record {
    int64_t qpc;
    uint32_t phase, loop, world, mode, intro, rawDirection, rawButtons, converted;
    uint32_t actorDirection, x, y, pattern;
};
static_assert(sizeof(Record)==56);
struct Header { uint32_t magic, version, count, capacity; int64_t frequency; uint32_t status, recordSize; };
constexpr uint32_t capacity=262144, mapSize=4096+capacity*sizeof(Record);
static Header* header;
static Record* records;
static uint32_t loop;
static bool nativeBoot=false, bootDone=false;
static bool nativeInput=false, inputRestored=false;
extern "C" { void *latencyLoop=nullptr, *latencyInput=nullptr, *latencyDraw=nullptr, *latencyPresent=nullptr, *latencyRequest=nullptr; }
static uint32_t read(uintptr_t address) { return *reinterpret_cast<volatile uint32_t*>(address); }
static bool restoreNativeInput() {
    struct Site { uintptr_t address; uint8_t bytes[3]; unsigned size; };
    // MbaaPatcher::ApplyStartupPatchesと標準EXEの逆アセンブルで照合した9命令。
    const Site sites[]{
        {0x41f098,{0x89,0x06},2},{0x41f0a0,{0x89,0x46,0x0c},3},
        {0x4a024e,{0x89,0x07},2},{0x4a027f,{0x09,0x57,0x0c},3},
        {0x4a0291,{0x09,0x57,0x0c},3},{0x4a02a2,{0x09,0x57,0x0c},3},
        {0x4a02b4,{0x09,0x57,0x0c},3},{0x4a02e9,{0x89,0x17},2},
        {0x4a02f2,{0x09,0x47,0x0c},3}};
    for (const auto& site:sites)
        for (unsigned i=0;i<site.size;++i)
            if (*reinterpret_cast<uint8_t*>(site.address+i)!=0x90) return false;
    for (const auto& site:sites) {
        auto* target=reinterpret_cast<void*>(site.address);
        DWORD old{},ignored{};
        if (!VirtualProtect(target,site.size,PAGE_EXECUTE_READWRITE,&old)) return false;
        std::memcpy(target,site.bytes,site.size);
        const bool flushed=FlushInstructionCache(GetCurrentProcess(),target,site.size)!=FALSE;
        const bool protectedAgain=VirtualProtect(target,site.size,old,&ignored)!=FALSE;
        if (!flushed || !protectedAgain) return false;
    }
    // キーボード配列は無効のまま。標準_KeyConfig.iniで選んだコントローラだけを測る。
    HookLog("[NativeInputLatency] nine original input instructions restored; tool rendering and pacing retained");
    return true;
}
extern "C" __attribute__((force_align_arg_pointer,noinline)) void latencyCapture(uint32_t phase) {
    if (phase==0) ++loop;
    const auto n=header->count;
    if (n>=capacity) { header->status=3; return; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    auto& r=records[n]; r={}; r.qpc=now.QuadPart; r.phase=phase; r.loop=loop;
    r.world=read(0x55d1d4); r.mode=read(0x54eee8);
    r.intro=*reinterpret_cast<volatile uint8_t*>(0x55d20b);
    const auto input=read(0x76e6ac);
    if (input) { r.rawDirection=read(input+0x18); r.rawButtons=read(input+0x24); r.converted=read(input+0x468); }
    r.actorDirection=*reinterpret_cast<volatile uint8_t*>(0x55541b);
    r.x=read(0x555238); r.y=read(0x55523c); r.pattern=read(0x555134);
    MemoryBarrier(); InterlockedExchange(reinterpret_cast<volatile LONG*>(&header->count),n+1);
    if (nativeInput && !inputRestored && phase==0 && r.mode==1) {
        if (!restoreNativeInput()) {
            header->status=6;
            HookLog("[NativeInputLatency] input patch signature/protection failure; measurement aborted");
            ExitProcess(ERROR_DLL_INIT_FAILED);
        }
        inputRestored=true;
    }
    if (nativeBoot && !bootDone) {
        if (phase==0) {
            cccaster::game_memory::startup_system_info::Restore();
            if (r.mode==1) {
                cccaster::game_memory::startup_file_read::Restore();
                cccaster::game_memory::startup::SetBootFade(false);
                bootDone=true;
                HookLog("[NativeLatency] training battle reached; stock input, logic, rendering and pacing retained");
            } else {
                cccaster::game_memory::startup_direct_entry::TryTraining(r.mode);
                if (r.mode!=20 && (read(0x74d598)==1 || read(0x74d598)==99))
                    *reinterpret_cast<uint32_t*>(0x74d598)=101;
            }
        }
        if (phase==1 && input && (r.mode==2 || r.mode==3 || r.mode==20 || r.mode==25)) {
            // GameMem::WriteInputと同じ生入力境界。派生入力には書かない。
            *reinterpret_cast<uint32_t*>(input+0x18)=0;
            *reinterpret_cast<uint32_t*>(input+0x24)=loop%30==0 ? 0x400 : 0;
            *reinterpret_cast<uint32_t*>(input+0x2c)=0;
            *reinterpret_cast<uint32_t*>(input+0x38)=loop%30==0 ? 0x400 : 0;
        }
    }
}
#define GATE(name,phase,trampoline) extern "C" __attribute__((naked)) void name() { \
    asm volatile("pushfl\n\tpushal\n\tmov %esp,%ebx\n\tsub $528,%esp\n\tand $-16,%esp\n\tfxsave (%esp)\n\tpush $" #phase "\n\tcall _latencyCapture\n\tadd $4,%esp\n\tfxrstor (%esp)\n\tmov %ebx,%esp\n\tpopal\n\tpopfl\n\tjmp *_" #trampoline); }
GATE(latencyLoopGate,0,latencyLoop)
GATE(latencyInputGate,1,latencyInput)
GATE(latencyDrawGate,2,latencyDraw)
GATE(latencyPresentGate,3,latencyPresent)
GATE(latencyRequestGate,4,latencyRequest)
static DWORD WINAPI initialize(void*) {
    char name[96]; std::snprintf(name,sizeof(name),"Local\\CCCasterInputLatency_%lu",GetCurrentProcessId());
    const auto mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,mapSize,name);
    if (!mapping) return 1;
    header=static_cast<Header*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,mapSize));
    if (!header) return 2;
    std::memset(header,0,mapSize); records=reinterpret_cast<Record*>(reinterpret_cast<char*>(header)+4096);
    LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
    *header={0x4c494343,1,0,capacity,frequency.QuadPart,0,sizeof(Record)};
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    cccaster::game_build::PeIdentity identity;
    if (base!=0x400000 || !cccaster::game_build::ReadHeaders({reinterpret_cast<const uint8_t*>(base),4096},identity) ||
        !cccaster::game_build::SupportsRuntime(cccaster::game_build::IdentifyHeaders(identity))) { header->status=4; return 4; }
    nativeBoot=std::getenv("CCCASTER_LATENCY_NATIVE")!=nullptr;
    nativeInput=std::getenv("CCCASTER_LATENCY_NATIVE_INPUT")!=nullptr;
    if (nativeBoot) {
        char gate[96]; cccaster::diagnostics::startup::GateName(gate,sizeof(gate),GetCurrentProcessId());
        CreateEventA(nullptr,TRUE,FALSE,gate);
        constexpr uint8_t original[]{0x75,0x16,0x0f,0xb7,0x45,0x10,0x83,0xe8,0x01,0x74,0x11,
            0x83,0xe8,0x01,0x75,0x08,0x50,0x56,0xff,0x15,0x1c,0xb3,0x51,0x00,0x33,0xc0,0xeb,0x0e};
        auto* dialog=reinterpret_cast<uint8_t*>(0x4a1d42);
        if (std::memcmp(dialog,original,sizeof(original))) {header->status=4;return 4;}
        DWORD old{},ignored{}; if(!VirtualProtect(dialog,2,PAGE_EXECUTE_READWRITE,&old))return 6;
        dialog[0]=0xeb;dialog[1]=0x0e;
        FlushInstructionCache(GetCurrentProcess(),dialog,2);VirtualProtect(dialog,2,old,&ignored);
        cccaster::game_memory::startup_system_info::Initialize(1);
        cccaster::game_memory::startup_file_read::Initialize(1);
        cccaster::game_memory::startup::SetBootFade(true);
    }
    struct Hook { uintptr_t address; void* gate; void** trampoline; uint8_t bytes[6]; unsigned size=6; };
    const Hook hooks[]{
        {0x40e390,reinterpret_cast<void*>(latencyLoopGate),&latencyLoop,{0x51,0x8b,0x0d,0xb0,0xe6,0x76}},
        {0x41f0c0,reinterpret_cast<void*>(latencyInputGate),&latencyInput,{0x81,0xec,0x90,0,0,0}},
        {0x4330c0,reinterpret_cast<void*>(latencyDrawGate),&latencyDraw,{0x53,0x55,0x33,0xed,0x39,0x2d}},
        {0x4bdbc0,reinterpret_cast<void*>(latencyPresentGate),&latencyPresent,{0x55,0x8b,0xec,0x83,0xe4,0xf8}},
        // vtable + 0x44 = IDirect3DDevice9::Present。内部待機の後、呼出し直前を別途記録。
        {0x4bdd03,reinterpret_cast<void*>(latencyRequestGate),&latencyRequest,{0x8b,0x47,0x04,0x8b,0x08},5}};
    for (const auto& h:hooks) if (std::memcmp(reinterpret_cast<void*>(h.address),h.bytes,h.size)) { header->status=4; return 4; }
    if (MH_Initialize()!=MH_OK) { header->status=5; return 5; }
    for (const auto& h:hooks) if (MH_CreateHook(reinterpret_cast<void*>(h.address),h.gate,h.trampoline)!=MH_OK) { header->status=5; return 5; }
    if (MH_EnableHook(MH_ALL_HOOKS)!=MH_OK) { header->status=5; return 5; }
    header->status=1; return 0;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(instance); const auto worker=CreateThread(nullptr,0,initialize,nullptr,0,nullptr); if(worker)CloseHandle(worker); }
    return TRUE;
}
