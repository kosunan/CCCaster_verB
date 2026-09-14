#include "core_dll/hook/DriverLockProbe.hpp"
#include <cstdio>
#include <cstdlib>

void HookLog(const char *) {}
namespace cccaster::platform {
int64_t RealMonotonicTicks() {
    LARGE_INTEGER now{}, hz{};
    QueryPerformanceCounter(&now); QueryPerformanceFrequency(&hz);
    return now.QuadPart / hz.QuadPart * 60000000 + now.QuadPart % hz.QuadPart * 60000000 / hz.QuadPart;
}
}
using namespace cccaster::diagnostics::driver_lock;
struct Context { unsigned kind; void *lock; HANDLE ready; };
DWORD WINAPI Owner(void *p) {
    auto &ctx=*static_cast<Context *>(p);
    if(ctx.kind==1) EnterCriticalSection(static_cast<CRITICAL_SECTION *>(ctx.lock));
    else AcquireSRWLockExclusive(static_cast<SRWLOCK *>(ctx.lock));
    SetEvent(ctx.ready);
    Sleep(50);
    if(ctx.kind==1) LeaveCriticalSection(static_cast<CRITICAL_SECTION *>(ctx.lock));
    else ReleaseSRWLockExclusive(static_cast<SRWLOCK *>(ctx.lock));
    return 0;
}
bool Check(unsigned kind, void *lock) {
    Context ctx{kind,lock,CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    DWORD owner=0;
    HANDLE thread=CreateThread(nullptr,0,Owner,&ctx,0,&owner);
    if(!thread || WaitForSingleObject(ctx.ready,2000)!=WAIT_OBJECT_0) return false;
    if(kind==1) {EnterCriticalSection(static_cast<CRITICAL_SECTION *>(lock)); LeaveCriticalSection(static_cast<CRITICAL_SECTION *>(lock));}
    if(kind==2) {AcquireSRWLockExclusive(static_cast<SRWLOCK *>(lock)); ReleaseSRWLockExclusive(static_cast<SRWLOCK *>(lock));}
    if(kind==3) {AcquireSRWLockShared(static_cast<SRWLOCK *>(lock)); ReleaseSRWLockShared(static_cast<SRWLOCK *>(lock));}
    const bool ended=WaitForSingleObject(thread,2000)==WAIT_OBJECT_0;
    CloseHandle(thread);CloseHandle(ctx.ready);
    bool found=false;
    for(auto &slot:slots) if(slot.ready==1 && slot.event.lock==reinterpret_cast<uintptr_t>(lock) && slot.event.kind==kind && slot.event.tid==GetCurrentThreadId()) {
        found=slot.event.end-slot.event.begin>=6000 && (kind!=1 || slot.event.owner==owner);
    }
    return ended && found;
}
int main() {
    _putenv("CCCASTER_DRIVER_LOCK_PROBE=1");
    if(MH_Initialize()!=MH_OK) return 1;
    Install();
    if(!enabled || !originalEnter || !originalExclusive || !originalShared) return 2;
    CRITICAL_SECTION cs{}; InitializeCriticalSection(&cs);
    SRWLOCK exclusive=SRWLOCK_INIT, shared=SRWLOCK_INIT;
    if(!Check(1,&cs) || !Check(2,&exclusive) || !Check(3,&shared)) return 3;
    SetLastError(1234); originalEnter(&cs); const auto expected=GetLastError(); LeaveCriticalSection(&cs);
    SetLastError(1234); EnterCriticalSection(&cs); const auto actual=GetLastError();
    EnterCriticalSection(&cs); // 再帰取得の意味を変えない
    if(cs.RecursionCount!=2 || expected!=actual) return 4;
    LeaveCriticalSection(&cs);LeaveCriticalSection(&cs);DeleteCriticalSection(&cs);
    Flush(0);
    Event event{};event.end=6000;
    for(unsigned i=0;i<5000;++i) Publish(event);
    if(dropped!=904) return 5; // 満杯でも待たず、欠落数を残す
    enabled=false;
    MH_DisableHook(MH_ALL_HOOKS);MH_Uninitialize();
    std::puts("CS/SRW競合・所有者・再帰・LastError・満杯時の欠落検知: 成功");
    return 0;
}
