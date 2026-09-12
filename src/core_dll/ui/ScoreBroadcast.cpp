#include "core_dll/ui/ScoreBroadcast.hpp"
#include <windows.h>
#include <atomic>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <regex>

namespace cccaster::domain::ui::score_broadcast {
namespace {
struct Worker {
    std::mutex mutex;
    session::SessionScoreSnapshot pending{};
    bool stopping = false;
    HANDLE event = nullptr;
    std::thread thread;
    std::filesystem::path base;
    uint64_t sessionId = 0;
    std::atomic<bool> healthy{false};
};
// プロセス終了のDllMainでjoin/destructしない。明示unloadはShutdownが回収する。
Worker *worker = nullptr;

// 強制終了時も次回起動で回収する。PIDを確認できない場合は保全する。
void PruneStoppedOutputs(const std::filesystem::path &directory) {
    const std::wregex pattern(L"^cccaster-score-([0-9]{1,10})\\.(json|txt)(\\.tmp)?$");
    std::error_code error;
    std::filesystem::directory_iterator it(directory, error), end;
    while (!error && it != end) {
        const auto entry = *it;
        it.increment(error);
        std::error_code statusError;
        if (!entry.is_regular_file(statusError) || entry.is_symlink(statusError)) continue;
        std::wsmatch match;
        const auto name = entry.path().filename().wstring();
        if (!std::regex_match(name, match, pattern)) continue;
        const auto value = std::stoull(match[1].str());
        if (!value || value > MAXDWORD || value == GetCurrentProcessId()) continue;
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(value));
        const bool stopped = process ? WaitForSingleObject(process, 0) == WAIT_OBJECT_0
                                     : GetLastError() == ERROR_INVALID_PARAMETER;
        if (process) CloseHandle(process);
        if (stopped) std::filesystem::remove(entry.path(), statusError);
    }
}

bool ReplaceFile(const std::filesystem::path &path, const std::string &contents) {
    auto temporary = path;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.close();
        if (!output) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

void Write(Worker &w, const session::SessionScoreSnapshot &s) {
    std::ostringstream json;
    json << "{\n  \"schema_version\": 1,\n  \"process_id\": " << GetCurrentProcessId()
         << ",\n  \"session_id\": \"" << w.sessionId << "\",\n  \"updated_unix\": " << std::time(nullptr)
         << ",\n  \"active\": " << (s.active ? "true" : "false")
         << ",\n  \"revision\": " << s.revision
         << ",\n  \"last_match_generation\": " << s.lastMatchGeneration
         << ",\n  \"p1_wins\": " << s.p1Wins << ",\n  \"p2_wins\": " << s.p2Wins
         << ",\n  \"unresolved\": " << s.unresolved
         << ",\n  \"scope\": \"confirmed_matches\"\n}\n";
    auto jsonPath = w.base; jsonPath += L".json";
    auto textPath = w.base; textPath += L".txt";
    const std::string text = s.active ? "P1 " + std::to_string(s.p1Wins) + " - " +
        std::to_string(s.p2Wins) + " P2\n" : "";
    const bool jsonOk = ReplaceFile(jsonPath, json.str());
    const bool textOk = ReplaceFile(textPath, text);
    w.healthy.store(jsonOk && textOk, std::memory_order_release);
}

void Run(Worker *w) {
    PruneStoppedOutputs(w->base.parent_path());
    for (;;) {
        if (WaitForSingleObject(w->event, INFINITE) != WAIT_OBJECT_0) break;
        session::SessionScoreSnapshot copy;
        bool stopping;
        {
            std::lock_guard<std::mutex> lock(w->mutex);
            copy = w->pending;
            stopping = w->stopping;
        }
        Write(*w, copy);
        if (stopping) break;
    }
}
} // namespace

bool Initialize(const std::filesystem::path &directory) {
    if (worker) return false;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    auto *w = new Worker;
    w->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!w->event) { delete w; return false; }
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    w->sessionId = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    w->base = directory / (L"cccaster-score-" + std::to_wstring(GetCurrentProcessId()));
    try { w->thread = std::thread(Run, w); }
    catch (...) { CloseHandle(w->event); delete w; return false; }
    worker = w;
    SetEvent(w->event); // 前回値をまず空にする。ゲームスレッドはファイルを触らない。
    return true;
}

void Publish(const session::SessionScoreSnapshot &score) {
    if (!worker) return;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        if (worker->stopping || worker->pending == score) return;
        worker->pending = score;
    }
    SetEvent(worker->event);
}

void Clear() {
    if (!worker) return;
    {
        std::lock_guard<std::mutex> lock(worker->mutex);
        worker->pending = {};
        worker->stopping = true;
    }
    SetEvent(worker->event);
}

void Shutdown() {
    if (!worker) return;
    Clear();
    worker->thread.join();
    CloseHandle(worker->event);
    delete worker;
    worker = nullptr;
}

bool Healthy() { return worker && worker->healthy.load(std::memory_order_acquire); }
} // namespace cccaster::domain::ui::score_broadcast
