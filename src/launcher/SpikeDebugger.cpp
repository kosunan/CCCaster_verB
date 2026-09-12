#include "launcher/SpikeDebugger.hpp"
#include "shared_contracts/SpikeDebugGate.hpp"
#include <fstream>
#include <string>
#include <cstdint>
#include <iostream>
#include <cstdlib>
#include <algorithm>
namespace cccaster::main_app {
namespace {
int64_t Qpc() { LARGE_INTEGER n{}; QueryPerformanceCounter(&n); return n.QuadPart; }
std::string Hex(const void* data, size_t n) {
    const char* digits = "0123456789abcdef";
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string out; out.reserve(n * 2);
    for (size_t i=0; i<n; ++i) { out += digits[bytes[i] >> 4]; out += digits[bytes[i] & 15]; }
    return out;
}
}
bool SpikeDebugger::Start(DWORD pid, DWORD tid) {
    worker_ = std::thread(&SpikeDebugger::Run, this, pid, tid);
    for (unsigned i=0; i<5000 && !ready_.load(); ++i) Sleep(1);
    if (ready_.load() != 1) { Stop(); return false; }
    return true;
}
void SpikeDebugger::Stop() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
}
void SpikeDebugger::Run(DWORD pid, DWORD tid) {
    const std::string path = "spike_debug_" + std::to_string(pid) + "_" + std::to_string(Qpc()) + ".tsv";
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    HANDLE process = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION | SYNCHRONIZE, FALSE, pid);
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    char gateName[96]; cccaster::diagnostics::SpikeDebugGateName(gateName, sizeof(gateName), pid);
    HANDLE gate=CreateEventA(nullptr, TRUE, FALSE, gateName);
    if (!out || !process || !thread || !gate) {
        std::cerr << "[SpikeDebug] attach failed error=" << GetLastError() << "\n";
        if (thread) CloseHandle(thread);
        if (process) CloseHandle(process);
        if (gate) CloseHandle(gate);
        ready_.store(-1); return;
    }
    ready_.store(1);
    std::cerr << "[SpikeDebug] waiting for combat readiness pid=" << pid << "\n";
    bool combatReady=false;
    while (!stop_.load() && WaitForSingleObject(process,0)==WAIT_TIMEOUT) {
        if (WaitForSingleObject(gate,50)==WAIT_OBJECT_0) { combatReady=true; break; }
    }
    CloseHandle(gate);
    if (!combatReady || !DebugActiveProcess(pid)) {
        std::cerr << "[SpikeDebug] not attached pid=" << pid << " error=" << GetLastError() << "\n";
        CloseHandle(thread); CloseHandle(process); return;
    }
    if (!DebugSetProcessKillOnExit(FALSE)) {
        DebugActiveProcessStop(pid); CloseHandle(thread); CloseHandle(process);
        ready_.store(-1); return;
    }
    LARGE_INTEGER hz{}; QueryPerformanceFrequency(&hz);
    unsigned intervalUs=2000;
    char intervalText[32]{};
    if(GetEnvironmentVariableA("CCCASTER_DEBUG_INTERVAL_US",intervalText,sizeof(intervalText)) < sizeof(intervalText)) {
        char* tail=nullptr;
        const auto parsed=std::strtoul(intervalText,&tail,10);
        if(tail!=intervalText && *tail=='\0' && parsed>=200 && parsed<=10000) intervalUs=unsigned(parsed);
    }
    out << "META\t1\t" << pid << '\t' << tid << '\t' << hz.QuadPart << '\t' << intervalUs << "\t200000\n";
    out.flush();
    std::cerr << "[SpikeDebug] attached pid=" << pid << " tid=" << tid << " file=" << path
              << " intervalUs=" << intervalUs << " timingPerturbed=1\n";
    ready_.store(1);
    bool initialBreak = true, exited = false;
    HANDLE timer=CreateWaitableTimerExW(nullptr, nullptr, 0x2, TIMER_ALL_ACCESS);
    out << "TIMER\t" << Qpc() << '\t' << (timer != nullptr) << '\n';
    uint32_t samples = 0;
    const auto alertWait=reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleA("ntdll.dll"),"NtWaitForAlertByThreadId"));
    uintptr_t d3dBase=0;
    auto next = Qpc();
    while (!stop_.load() && !exited && out) {
        DEBUG_EVENT event{};
        if (WaitForDebugEvent(&event, 0)) {
            const auto begin = Qpc();
            DWORD status = DBG_CONTINUE;
            if (event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT || event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
                const bool create = event.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT;
                const auto base = reinterpret_cast<uintptr_t>(create ? event.u.CreateProcessInfo.lpBaseOfImage : event.u.LoadDll.lpBaseOfDll);
                HANDLE file = create ? event.u.CreateProcessInfo.hFile : event.u.LoadDll.hFile;
                wchar_t wideName[16384]{};
                std::string name;
                const DWORD length = file ? GetFinalPathNameByHandleW(file, wideName, 16384, FILE_NAME_NORMALIZED) : 0;
                if (length && length < 16384) {
                    const int bytes=WideCharToMultiByte(CP_UTF8, 0, wideName, length, nullptr, 0, nullptr, nullptr);
                    name.resize(bytes);
                    if (bytes) WideCharToMultiByte(CP_UTF8, 0, wideName, length, name.data(), bytes, nullptr, nullptr);
                }
                IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS32 nt{}; SIZE_T got = 0;
                uint32_t size = 0, stamp = 0;
                if (ReadProcessMemory(process, reinterpret_cast<void*>(base), &dos, sizeof(dos), &got) &&
                    dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 1048576 &&
                    ReadProcessMemory(process, reinterpret_cast<void*>(base + dos.e_lfanew), &nt, sizeof(nt), &got) &&
                    nt.Signature == IMAGE_NT_SIGNATURE) { size=nt.OptionalHeader.SizeOfImage; stamp=nt.FileHeader.TimeDateStamp; }
                // このOS版で命令とPDBを照合した診断点だけを読む。他版では所有者採取を行わない。
                if(stamp==1987618885u && name.size()>=8 && name.substr(name.size()-8)=="d3d9.dll") d3dBase=base;
                out << "MODULE\t" << begin << '\t' << base << '\t' << size << '\t' << stamp << '\t' << name << '\n';
                if (file) CloseHandle(file);
            } else if (event.dwDebugEventCode == UNLOAD_DLL_DEBUG_EVENT) {
                out << "UNLOAD\t" << begin << '\t' << reinterpret_cast<uintptr_t>(event.u.UnloadDll.lpBaseOfDll) << '\n';
                if(reinterpret_cast<uintptr_t>(event.u.UnloadDll.lpBaseOfDll)==d3dBase) d3dBase=0;
            } else if (event.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
                const auto& e=event.u.Exception;
                out << "EXCEPTION\t" << begin << '\t' << event.dwThreadId << '\t' << e.ExceptionRecord.ExceptionCode
                    << '\t' << reinterpret_cast<uintptr_t>(e.ExceptionRecord.ExceptionAddress) << '\t' << e.dwFirstChance << '\n';
                if (initialBreak && e.dwFirstChance && e.ExceptionRecord.ExceptionCode == EXCEPTION_BREAKPOINT) initialBreak=false;
                else status=DBG_EXCEPTION_NOT_HANDLED;
            } else if (event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) exited=true;
            const BOOL continued = ContinueDebugEvent(event.dwProcessId, event.dwThreadId, status);
            out << "EVENT\t" << begin << '\t' << Qpc() << '\t' << event.dwDebugEventCode << '\t' << continued << '\n';
            if (!continued) break;
            continue;
        }
        if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT) break;
        if (initialBreak || samples >= 200000 || Qpc() < next) {
            LARGE_INTEGER due{};
            due.QuadPart=-10*std::max<int64_t>(50,std::min<int64_t>(1000,(next-Qpc())*1000000/hz.QuadPart));
            if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, INFINITE);
            else Sleep(1);
            continue;
        }
        const auto begin=Qpc();
        CONTEXT c{}; c.ContextFlags=CONTEXT_CONTROL | CONTEXT_INTEGER;
        unsigned char code[64]{}; uint32_t stack[512]{};
        SIZE_T codeSize=0, stackSize=0;
        DWORD ownerTid=0, ownerError=0, ownerPrevious=DWORD(-1), cs[6]{};
        bool ownerStable=false;
        uintptr_t critical=0;
        CONTEXT ownerContext{}; unsigned char ownerCode[64]{}; uint32_t ownerStack[512]{};
        SIZE_T ownerCodeSize=0,ownerStackSize=0;
        int64_t ownerBegin=0,ownerCaptured=0,ownerEnd=0;
        const DWORD previous=SuspendThread(thread);
        if (previous == DWORD(-1)) { out << "ERROR\t" << begin << "\tsuspend\t" << GetLastError() << '\n'; break; }
        const bool valid=GetThreadContext(thread, &c) != FALSE;
        const auto captured=Qpc();
        if (valid) {
            ReadProcessMemory(process, reinterpret_cast<void*>(uintptr_t(c.Eip)), code, sizeof(code), &codeSize);
            // 待機中は呼出元まで遡るため広く読む。値だけで確定スタックとは扱わない。
            const SIZE_T wanted=alertWait && c.Eip>=alertWait && c.Eip<alertWait+16 ? sizeof(stack) : 128;
            ReadProcessMemory(process, reinterpret_cast<void*>(uintptr_t(c.Esp)), stack, wanted, &stackSize);
            uintptr_t bp=c.Ebp;
            for(unsigned depth=0;d3dBase && depth<16 && bp>=c.Esp && bp-c.Esp+12<=stackSize;++depth) {
                const auto offset=(bp-c.Esp)/4;
                if((bp-c.Esp)%4) break;
                if(stack[offset+1]==d3dBase+0x140935) {
                    critical=stack[offset+2]; SIZE_T got=0;
                    if(ReadProcessMemory(process,reinterpret_cast<void*>(critical),cs,sizeof(cs),&got) && got==sizeof(cs)) ownerTid=cs[3];
                    break;
                }
                if(stack[offset]<=bp) break;
                bp=stack[offset];
            }
            if(ownerTid && ownerTid!=tid) {
                HANDLE owner=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION,FALSE,ownerTid);
                if(owner && GetProcessIdOfThread(owner)==pid) {
                    ownerBegin=Qpc();ownerPrevious=SuspendThread(owner);
                    if(ownerPrevious!=DWORD(-1)) {
                        ownerContext.ContextFlags=CONTEXT_CONTROL|CONTEXT_INTEGER;
                        if(GetThreadContext(owner,&ownerContext)) {
                            ownerCaptured=Qpc();
                            DWORD observed[6]{};SIZE_T got=0;
                            ownerStable=ReadProcessMemory(process,reinterpret_cast<void*>(critical),observed,sizeof(observed),&got) &&
                                got==sizeof(observed) && observed[3]==ownerTid && observed[2]>0;
                            ReadProcessMemory(process,reinterpret_cast<void*>(uintptr_t(ownerContext.Eip)),ownerCode,sizeof(ownerCode),&ownerCodeSize);
                            ReadProcessMemory(process,reinterpret_cast<void*>(uintptr_t(ownerContext.Esp)),ownerStack,sizeof(ownerStack),&ownerStackSize);
                        } else ownerError=GetLastError();
                        if(ResumeThread(owner)==DWORD(-1)) ownerError=GetLastError();
                    } else ownerError=GetLastError();
                    ownerEnd=Qpc();
                } else ownerError=GetLastError();
                if(owner) CloseHandle(owner);
            }
        }
        const DWORD resumed=ResumeThread(thread); // 必ず自分が追加した停止1回を戻してから整形。
        const auto end=Qpc();
        if(critical) out << "OWNER\t" << captured << '\t' << critical << '\t' << ownerTid << '\t' << cs[1] << '\t' << cs[2]
            << '\t' << ownerBegin << '\t' << ownerCaptured << '\t' << ownerEnd << '\t' << ownerPrevious << '\t' << ownerError
            << '\t' << ownerContext.Eip << '\t' << ownerContext.Esp << '\t' << ownerContext.Ebp
            << '\t' << Hex(ownerCode,ownerCodeSize) << '\t' << Hex(ownerStack,ownerStackSize) << '\t' << ownerStable << '\n';
        if (resumed == DWORD(-1)) { out << "ERROR\t" << end << "\tresume\t" << GetLastError() << '\n'; break; }
        if (valid) out << "SAMPLE\t" << begin << '\t' << captured << '\t' << end << '\t' << previous
            << '\t' << c.Eip << '\t' << c.Esp << '\t' << c.Ebp << '\t' << c.Eax << '\t' << c.Ebx
            << '\t' << c.Ecx << '\t' << c.Edx << '\t' << c.Esi << '\t' << c.Edi << '\t' << c.EFlags
            << '\t' << Hex(code, codeSize) << '\t' << Hex(stack, stackSize) << '\n';
        else out << "ERROR\t" << end << "\tcontext\n";
        ++samples;
        if (samples % 128 == 0) out.flush();
        if (samples == 200000) { out << "LIMIT\t" << end << "\tsampling_stopped\n"; out.flush(); }
        next=end + hz.QuadPart * intervalUs / 1000000;
    }
    if (!exited) out << "DETACH\t" << Qpc() << '\t' << DebugActiveProcessStop(pid) << '\n';
    out << "END\t" << Qpc() << '\t' << samples << '\n'; out.flush();
    if (timer) CloseHandle(timer);
    CloseHandle(thread); CloseHandle(process);
}
}
