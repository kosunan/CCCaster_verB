#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <ctime>

namespace cccaster::training_standby {
// 対戦IPCと独立した、GUI・練習worker・そのゲームだけの通知経路。
inline constexpr wchar_t Environment[] = L"CCCASTER_TRAINING_STANDBY";
struct State {
    uint32_t version = 1;
    uint32_t generation = 0;
    uint64_t heartbeat = 0;
    int64_t expires = 0;
    char request[33]{};
    char name[125]{};
    uint32_t reply = 0; // 1: ACCEPT, 2: DECLINE
    bool stopGame = false;
    bool Live(uint64_t ticks, int64_t now) const {
        return version == 1 && request[0] && !stopGame && expires > now &&
            ticks >= heartbeat && ticks - heartbeat < 3000;
    }
};
class Channel {
    HANDLE mapping_ = nullptr, mutex_ = nullptr;
    State* data_ = nullptr;
public:
    std::wstring name;
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel() { Close(); }
    void Close() {
        if (data_) UnmapViewOfFile(data_);
        if (mapping_) CloseHandle(mapping_);
        if (mutex_) CloseHandle(mutex_);
        data_ = nullptr; mapping_ = mutex_ = nullptr; name.clear();
    }
    bool Open(const std::wstring& value, bool create = false) {
        Close(); name = value;
        if (name.empty()) return false;
        const auto lockName = name + L"_lock";
        mutex_ = create ? CreateMutexW(nullptr, FALSE, lockName.c_str())
                        : OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, lockName.c_str());
        mapping_ = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(State), name.c_str())
                          : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (mapping_) data_ = static_cast<State*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(State)));
        if (!mutex_ || !data_) { Close(); return false; }
        if (create && !Update([](State& s) { s = State{}; })) { Close(); return false; }
        return true;
    }
    bool Create() {
        return Open(L"Local\\CCCasterTraining_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                    std::to_wstring(GetTickCount64()), true);
    }
    bool OpenEnvironment() {
        wchar_t value[256]{};
        const auto length = GetEnvironmentVariableW(Environment, value, 256);
        return length && length < 256 && Open(value);
    }
    template<class F> bool Update(F callback) {
        if (!data_ || !mutex_) return false;
        const auto result = WaitForSingleObject(mutex_, 0);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) return false;
        callback(*data_);
        ReleaseMutex(mutex_);
        return true;
    }
    bool Read(State& result) { return Update([&](State& s) { result = s; }); }
    bool Reply(uint32_t generation, uint32_t decision) {
        bool accepted = false;
        Update([&](State& s) {
            if (s.generation == generation && !s.reply && (decision == 1 || decision == 2) &&
                s.Live(GetTickCount64(), std::time(nullptr))) { s.reply = decision; accepted = true; }
        });
        return accepted;
    }
};
}
