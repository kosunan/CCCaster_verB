#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>

int GetProcessIdByName(const std::string& processName) {
    PROCESSENTRY32 processEntry;
    processEntry.dwSize = sizeof(PROCESSENTRY32);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (Process32First(snapshot, &processEntry)) {
        do {
            if (processName == processEntry.szExeFile) {
                CloseHandle(snapshot);
                return processEntry.th32ProcessID;
            }
        } while (Process32Next(snapshot, &processEntry));
    }
    CloseHandle(snapshot);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: injector.exe <dll_path>\n";
        return 1;
    }

    std::string dllPathStr = argv[1];
    char fullPath[MAX_PATH];
    if (GetFullPathNameA(dllPathStr.c_str(), MAX_PATH, fullPath, nullptr) == 0) {
        std::cerr << "Failed to get full local path.\n";
        return 1;
    }
    std::string absoluteDllPath = fullPath;
    
    int pid = GetProcessIdByName("MBAA.exe");
    if (pid == 0) {
        std::cerr << "MBAA.exe not found.\n";
        return 1;
    }
    
    std::cout << "Target PID: " << pid << "\n";
    std::cout << "Injecting: " << absoluteDllPath << "\n";
    
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) {
        std::cerr << "OpenProcess failed.\n";
        return 1;
    }
    
    void* loc = VirtualAllocEx(hProcess, 0, absoluteDllPath.length() + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!loc) {
        std::cerr << "VirtualAllocEx failed.\n";
        CloseHandle(hProcess);
        return 1;
    }
    
    WriteProcessMemory(hProcess, loc, absoluteDllPath.c_str(), absoluteDllPath.length() + 1, 0);
    
    HANDLE hThread = CreateRemoteThread(hProcess, 0, 0, (LPTHREAD_START_ROUTINE)LoadLibraryA, loc, 0, 0);
    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeThread(hThread, &exitCode);
        CloseHandle(hThread);
        
        if (exitCode != 0) {
            std::cout << "Injection successful! HMODULE: " << std::hex << exitCode << std::dec << "\n";
        } else {
            std::cerr << "Injection failed. LoadLibraryA returned NULL. Check DLL dependencies.\n";
        }
    } else {
        std::cerr << "CreateRemoteThread failed. Error: " << GetLastError() << "\n";
    }
    
    CloseHandle(hProcess);
    return 0;
}
