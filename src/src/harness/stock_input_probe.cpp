// MBAACC 1.07 Rev.1.4.0専用。製品DLLを使わず、標準DI受取り→描画データ読出しを測る。
// 対戦中の書込みはGetDeviceStateが返したDIJOYSTATE2のPOV/ボタンだけ。
// GameMem生入力への起動用確認はキャラ選択まで。対戦開始時に起動パッチを全て復帰する。
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dinput.h>
#include "MinHook.h"
#include "shared_contracts/GameBuild.hpp"
#include "core_dll/mbaa_mem/StartupDirectEntry.hpp"
#include "core_dll/mbaa_mem/StartupFileRead.hpp"
#include "core_dll/mbaa_mem/StartupSystemInfo.hpp"

void HookLog(const char* text) {
    if (auto* f=std::fopen("stock_input_boot.log","a")) { std::fprintf(f,"%s\n",text); std::fclose(f); }
}
struct Registers { uint32_t edi,esi,ebp,esp,ebx,edx,ecx,eax,flags; };
struct Record {
    int64_t qpc;
    uint32_t phase,loop,world,sample,stimulus,mode,intro,deviceIndex,result,buffer,pov,buttons;
    uint32_t rawDirection,rawButtons,converted,actorDirection,sequence,state,animation,x,y;
    uint32_t actor,renderX,renderY,renderAnimation,actorButtons;
};
static_assert(sizeof(Record)==112);
struct Header {
    uint32_t magic,version,count,capacity; int64_t frequency; uint32_t status,recordSize;
    volatile uint32_t command,completed; uint32_t bootDone,reserved;
};
constexpr uint32_t Capacity=262144, MapSize=4096+Capacity*sizeof(Record);
static Header* header; static Record* records;
static uint32_t loop,sample,stimulus,startLoop;
static bool bootDone;
static bool toolMode;
static uintptr_t productBegin,productEnd;
static GUID measuredGuid;
using DeviceState=HRESULT (WINAPI*)(IDirectInputDevice8A*,DWORD,void*);
static DeviceState originalDeviceState;
static IDirectInputDevice8A* measuredDevice;
static IDirectInput8A* probeInput;
static IDirectInputDevice8A* probeDevice;
static uint32_t externalPrevious;
static CRITICAL_SECTION captureLock;
struct CaptureLock {
    CaptureLock() { EnterCriticalSection(&captureLock); }
    ~CaptureLock() { LeaveCriticalSection(&captureLock); }
};
extern "C" { void *stockLoop,*stockConvert,*stockPresent,*stockReceive,*stockLogic,*stockRead,*stockSubmit,*stockAdvance,*stockDecision; }
static uint32_t read(uintptr_t p) { return *reinterpret_cast<volatile uint32_t*>(p); }
static bool patch(uintptr_t address,const void* bytes,unsigned size) {
    auto* p=reinterpret_cast<void*>(address); DWORD old{},ignored{};
    if (!VirtualProtect(p,size,PAGE_EXECUTE_READWRITE,&old)) return false;
    std::memcpy(p,bytes,size);
    const bool flushed=FlushInstructionCache(GetCurrentProcess(),p,size)!=FALSE;
    return VirtualProtect(p,size,old,&ignored)!=FALSE && flushed;
}
static void capture(uint32_t phase,const Registers* regs) {
    const auto n=header->count;
    if (n>=Capacity) { header->status=3; return; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    auto& r=records[n]; r={}; r.qpc=now.QuadPart; r.phase=phase; r.loop=loop;
    r.world=read(0x55d1d4); r.sample=sample; r.stimulus=stimulus;
    r.mode=read(0x54eee8); r.intro=*reinterpret_cast<const uint8_t*>(0x55d20b);
    const auto input=read(0x76e6ac);
    if (input) { r.rawDirection=read(input+0x18);r.rawButtons=read(input+0x24);r.converted=read(input+0x468); }
    r.actorDirection=*reinterpret_cast<const uint8_t*>(0x55541b);
    r.actorButtons=read(0x55541c);
    r.sequence=read(0x555140);r.state=read(0x555144);r.animation=read(0x555450);
    r.x=read(0x555238);r.y=read(0x55523c);
    if (phase==5 || phase==6) {
        r.deviceIndex=regs->ebp;r.result=regs->eax;r.buffer=regs->ebx;
        r.pov=read(regs->ebx+0x20);r.buttons=read(regs->ebx+0x30);
    }
    if (phase==7 || phase==8) r.actor=regs->edi;
    if (phase==8) { r.renderX=regs->eax;r.renderY=regs->ecx;r.renderAnimation=regs->esi; }
    if (phase==9) {
        const auto args=read(reinterpret_cast<uintptr_t>(regs)+40);
        r.actor=read(args);r.renderX=read(args+4);r.renderY=read(args+8);
    }
    if (phase==10 || phase==11) {
        r.actor=phase==10 ? regs->eax : regs->ebx;
        // 観測専用: 予約動作/優先度。標準の状態更新前と入力による予約後を結ぶ。
        r.deviceIndex=read(0x55543c)&0xffff;
        r.result=read(0x55543e)&0xffff;
    }
    MemoryBarrier(); InterlockedExchange(reinterpret_cast<volatile LONG*>(&header->count),n+1);
}
extern "C" __attribute__((force_align_arg_pointer,noinline)) void stockCapture(uint32_t phase,const Registers* regs) {
    // オンラインは入力専用スレッドとゲームスレッドから呼ばれる。
    // count公開前のrecord重複・loop/sampleの競合を防ぐ（ゲーム状態は変更しない）。
    CaptureLock lock;
    const auto mode=read(0x54eee8);
    if (phase==0) {
        ++loop;
        if (!bootDone && toolMode) {
            if (mode==1) { bootDone=true;header->bootDone=1; }
        } else if (!bootDone) {
            cccaster::game_memory::startup_system_info::Restore();
            if (mode==1) {
                cccaster::game_memory::startup_file_read::Restore();
                cccaster::game_memory::startup::SetBootFade(false);
                const uint8_t original[]{0x75,0x16};
                if (!patch(0x4a1d42,original,2)) ExitProcess(ERROR_WRITE_FAULT);
                bootDone=true;header->bootDone=1;
                HookLog("[StockInput] battle: all startup patches restored; product DLL absent");
            } else {
                cccaster::game_memory::startup_direct_entry::TryTraining(mode);
                if (mode!=20 && (read(0x74d598)==1 || read(0x74d598)==99))
                    *reinterpret_cast<uint32_t*>(0x74d598)=101;
            }
        }
    }
    if ((phase==7 || phase==8) && regs->edi!=0x555134) return;
    if ((phase==10 && regs->eax!=0x555134) || (phase==11 && regs->ebx!=0x555134)) return;
    if (phase==9 && read(read(reinterpret_cast<uintptr_t>(regs)+40))!=0x555134) return;
    if (phase==5) {
        if (regs->ebp+1!=read(0x74da18)) return; // 標準設定のP1コントローラだけ
        if (toolMode && bootDone && mode==1 && !*reinterpret_cast<const uint8_t*>(0x55d20b) && header->command==3) {
            // 外部コントローラ操作の観測専用。取得済みのバッファには書き込まない。
            if (static_cast<int32_t>(regs->eax)<0) {header->status=7;return;}
            const auto pov=read(regs->ebx+0x20);
            const uint32_t kind=pov==27000 ? 1 : pov==9000 ? 2 :
                (*reinterpret_cast<const uint8_t*>(regs->ebx+0x30)&0x80) ? 3 : 0;
            if (kind && !externalPrevious) {++sample;stimulus=kind;capture(6,regs);header->completed=sample;}
            externalPrevious=kind;
        }
        if (bootDone && mode==1 && !*reinterpret_cast<const uint8_t*>(0x55d20b) && header->command==1) {
            if (static_cast<int32_t>(regs->eax)<0) { header->status=7;return; }
            if (!startLoop) startLoop=loop+60;
            uint32_t pov=0xffffffff,buttons=0;
            bool beginning=false;
            if (loop>=startLoop) {
                const auto elapsed=loop-startLoop;
                if (elapsed>=30*60) { header->completed=30; header->command=2; }
                else {
                    sample=elapsed/60+1;stimulus=(sample-1)%3+1;beginning=elapsed%60==0;
                    if (elapsed%60<4) {
                        if (stimulus==1) pov=27000;
                        else if (stimulus==2) pov=9000;
                        else buttons=0x80;
                    }
                    header->completed=sample-1;
                }
            }
            // DIJOYSTATE2の実デバイス返却先。標準の4106D0以降を全て通す。
            *reinterpret_cast<uint32_t*>(regs->ebx+0x20)=pov;
            std::memset(reinterpret_cast<void*>(regs->ebx+0x30),0,128);
            *reinterpret_cast<uint8_t*>(regs->ebx+0x30)=uint8_t(buttons);
            if (beginning) capture(6,regs);
        }
    }
    capture(phase,regs);
    if (!toolMode && !bootDone && phase==1 && (mode==2 || mode==3 || mode==20 || mode==25)) {
        const auto input=read(0x76e6ac);
        if (input) {
            const bool stage=read(0x74d8ec)==5 && read(0x74d910)==5;
            const bool chooseStage=stage && read(0x74fd98)!=1;
            const bool pulse=loop%30==0;
            *reinterpret_cast<uint32_t*>(input+0x18)=chooseStage && pulse ? 2 : 0;
            *reinterpret_cast<uint32_t*>(input+0x24)=!chooseStage && pulse ? 0x400 : 0;
            *reinterpret_cast<uint32_t*>(input+0x2c)=0;
            *reinterpret_cast<uint32_t*>(input+0x38)=!stage && pulse ? 0x400 : 0;
        }
    }
}
// 製品自身がGetDeviceStateから受け取る同じDIJOYSTATE2境界。
// 製品DLL・対象GUIDからの呼出しだけを対象にし、標準ゲーム側の取得には注入しない。
static HRESULT WINAPI toolDeviceState(IDirectInputDevice8A* device,DWORD size,void* buffer) {
    const auto caller=reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const auto hr=originalDeviceState(device,size,buffer);
    if (caller<productBegin || caller>=productEnd || size!=sizeof(DIJOYSTATE2)) return hr;
    if (device!=measuredDevice) {
        DIDEVICEINSTANCEA info{};info.dwSize=sizeof(info);
        if (FAILED(device->GetDeviceInfo(&info)) || !IsEqualGUID(info.guidInstance,measuredGuid)) return hr;
        measuredDevice=device;
    }
    Registers regs{};regs.eax=uint32_t(hr);regs.ebx=reinterpret_cast<uintptr_t>(buffer);
    regs.ebp=read(0x74da18)-1;
    stockCapture(5,&regs);
    return hr;
}
#define GATE(name,phase,trampoline) extern "C" __attribute__((naked)) void name() { \
    asm volatile("pushfl\n\tpushal\n\tmov %esp,%ebx\n\tsub $528,%esp\n\tand $-16,%esp\n\tfxsave (%esp)\n\tpush %ebx\n\tpush $" #phase "\n\tcall _stockCapture\n\tadd $8,%esp\n\tfxrstor (%esp)\n\tmov %ebx,%esp\n\tpopal\n\tpopfl\n\tjmp *_" #trampoline); }
GATE(stockLoopGate,0,stockLoop)
GATE(stockConvertGate,1,stockConvert)
GATE(stockPresentGate,4,stockPresent)
GATE(stockReceiveGate,5,stockReceive)
GATE(stockLogicGate,7,stockLogic)
GATE(stockReadGate,8,stockRead)
GATE(stockSubmitGate,9,stockSubmit)
GATE(stockAdvanceGate,10,stockAdvance)
GATE(stockDecisionGate,11,stockDecision)
static DWORD WINAPI initialize(void*) {
    InitializeCriticalSection(&captureLock);
    char name[96];std::snprintf(name,sizeof(name),"Local\\CCCasterStockInput_%lu",GetCurrentProcessId());
    const auto mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,MapSize,name);
    if (!mapping) return 1;
    header=static_cast<Header*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,MapSize));
    if (!header) return 2;
    std::memset(header,0,MapSize);records=reinterpret_cast<Record*>(reinterpret_cast<char*>(header)+4096);
    LARGE_INTEGER frequency;QueryPerformanceFrequency(&frequency);
    *header={0x53494343,2,0,Capacity,frequency.QuadPart,0,sizeof(Record),0,0,0,0};
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    cccaster::game_build::PeIdentity identity;
    if (base!=0x400000 || !cccaster::game_build::ReadHeaders({reinterpret_cast<const uint8_t*>(base),4096},identity) ||
        !cccaster::game_build::SupportsRuntime(cccaster::game_build::IdentifyHeaders(identity))) {header->status=4;return 4;}
    toolMode=std::getenv("CCCASTER_STOCK_PROBE_TOOL")!=nullptr;
    header->reserved=toolMode ? 1 : 0;
    if (toolMode) {
        productBegin=reinterpret_cast<uintptr_t>(GetModuleHandleA("libcccaster_hook.dll"));
        if (!productBegin) {header->status=8;return 8;}
        const auto* dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(productBegin);
        productEnd=productBegin+reinterpret_cast<const IMAGE_NT_HEADERS*>(productBegin+dos->e_lfanew)->OptionalHeader.SizeOfImage;
        unsigned d[11];const auto* guid=std::getenv("CCCASTER_STOCK_PROBE_GUID");
        if (!guid || std::sscanf(guid,"%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
            &d[0],&d[1],&d[2],&d[3],&d[4],&d[5],&d[6],&d[7],&d[8],&d[9],&d[10])!=11) {header->status=8;return 8;}
        measuredGuid={d[0],uint16_t(d[1]),uint16_t(d[2]),{}};
        for(unsigned i=0;i<8;++i) measuredGuid.Data4[i]=uint8_t(d[i+3]);
        if (FAILED(DirectInput8Create(GetModuleHandleA(nullptr),0x800,IID_IDirectInput8A,
            reinterpret_cast<void**>(&probeInput),nullptr)) ||
            FAILED(probeInput->CreateDevice(measuredGuid,&probeDevice,nullptr))) {header->status=8;return 8;}
    } else {
    char gate[96];cccaster::diagnostics::startup::GateName(gate,sizeof(gate),GetCurrentProcessId());
    CreateEventA(nullptr,TRUE,FALSE,gate);
    constexpr uint8_t dialogOriginal[]{0x75,0x16,0x0f,0xb7,0x45,0x10,0x83,0xe8,0x01,0x74,0x11,
        0x83,0xe8,0x01,0x75,0x08,0x50,0x56,0xff,0x15,0x1c,0xb3,0x51,0x00,0x33,0xc0,0xeb,0x0e};
    if (std::memcmp(reinterpret_cast<void*>(0x4a1d42),dialogOriginal,sizeof(dialogOriginal))) {header->status=4;return 4;}
    const uint8_t closeDialog[]{0xeb,0x0e};
    if (!patch(0x4a1d42,closeDialog,2)) {header->status=6;return 6;}
    cccaster::game_memory::startup_system_info::Initialize(1);
    cccaster::game_memory::startup_file_read::Initialize(1);
    cccaster::game_memory::startup::SetBootFade(true);
    }
    struct Hook { uintptr_t address;void* gate;void** trampoline;uint8_t bytes[7];unsigned size; };
    const Hook hooks[]{
        {0x40e390,reinterpret_cast<void*>(stockLoopGate),&stockLoop,{0x51,0x8b,0x0d,0xb0,0xe6,0x76},6},
        {0x41f0c0,reinterpret_cast<void*>(stockConvertGate),&stockConvert,{0x81,0xec,0x90,0,0,0},6},
        {0x4bdd03,reinterpret_cast<void*>(stockPresentGate),&stockPresent,{0x8b,0x47,0x04,0x8b,0x08},5},
        {0x4108ef,reinterpret_cast<void*>(stockReceiveGate),&stockReceive,{0x83,0x7f,0x34,0,0x75,0x11},6},
        {0x46d90e,reinterpret_cast<void*>(stockLogicGate),&stockLogic,{0x8b,0xc5,0xe8,0xfb,0xfc,0xff,0xff},7},
        {0x41af48,reinterpret_cast<void*>(stockReadGate),&stockRead,{0x89,0x44,0x24,0x24,0x8a,0x87},6},
        {0x41a390,reinterpret_cast<void*>(stockSubmitGate),&stockSubmit,{0x81,0xec,0xe8,0,0,0},6},
        {0x4618c0,reinterpret_cast<void*>(stockAdvanceGate),&stockAdvance,{0x51,0x53,0x55,0x56,0x57},5},
        {0x46ddf0,reinterpret_cast<void*>(stockDecisionGate),&stockDecision,{0x8b,0x44,0x24,0x18,0x83,0xc0,0x01},7}};
    for (const auto& h:hooks) if ((!toolMode || h.address!=0x4108ef) && std::memcmp(reinterpret_cast<void*>(h.address),h.bytes,h.size)) {header->status=4;return 4;}
    if (MH_Initialize()!=MH_OK) {header->status=5;return 5;}
    for (const auto& h:hooks) if ((!toolMode || h.address!=0x4108ef) && MH_CreateHook(reinterpret_cast<void*>(h.address),h.gate,h.trampoline)!=MH_OK) {header->status=5;return 5;}
    if (toolMode) {
        auto** vtable=*reinterpret_cast<void***>(probeDevice);
        if (MH_CreateHook(vtable[9],reinterpret_cast<void*>(toolDeviceState),reinterpret_cast<void**>(&originalDeviceState))!=MH_OK) {header->status=5;return 5;}
        char message[192];std::snprintf(message,sizeof(message),"[ToolInput] GetDeviceState=%p product=%08X-%08X",vtable[9],unsigned(productBegin),unsigned(productEnd));HookLog(message);
    }
    if (MH_EnableHook(MH_ALL_HOOKS)!=MH_OK) {header->status=5;return 5;}
    header->status=1;return 0;
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) {DisableThreadLibraryCalls(instance);const auto worker=CreateThread(nullptr,0,initialize,nullptr,0,nullptr);if(worker)CloseHandle(worker);}
    return TRUE;
}
