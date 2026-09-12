// 自分で起動した独立ゲームコピーに共通観測DLLをロードする32bit補助。
#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <iterator>
#include <cstdio>
#include "shared_contracts/GameBuild.hpp"
int wmain(int argc, wchar_t **argv) {
    if (argc != 4) return 1;
    DWORD pid = wcstoul(argv[1],nullptr,10);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_CREATE_THREAD|
        PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ|SYNCHRONIZE,FALSE,pid);
    if (!process) return 2;
    wchar_t executable[32768]; DWORD size = 32768;
    if (!QueryFullProcessImageNameW(process,0,executable,&size) ||
        _wcsicmp(executable,argv[3])) return 3;
    HANDLE file = CreateFileW(executable,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if (file == INVALID_HANDLE_VALUE) return 4;
    DWORD bytes = GetFileSize(file,nullptr), read = 0;
    std::vector<uint8_t> data(bytes);
    if (!ReadFile(file,data.data(),bytes,&read,nullptr) || read != bytes) return 5;
    CloseHandle(file);
    if (cccaster::game_build::IdentifyFile(data) != cccaster::game_build::Edition::Carnival140) return 6;
    const size_t length = (wcslen(argv[2])+1)*sizeof(wchar_t);
    void *memory = VirtualAllocEx(process,nullptr,length,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if (!memory || !WriteProcessMemory(process,memory,argv[2],length,nullptr)) return 7;
    auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(process,nullptr,0,load,memory,0,nullptr);
    if (!thread || WaitForSingleObject(thread,10000) != WAIT_OBJECT_0) return 8;
    DWORD result = 0; GetExitCodeThread(thread,&result);
    CloseHandle(thread); VirtualFreeEx(process,memory,0,MEM_RELEASE); CloseHandle(process);
    std::printf("probe loaded pid=%lu module=%08lx\n",pid,result);
    return result ? 0 : 9;
}
