#include "core_dll/ui/ScoreBroadcast.hpp"
#include <windows.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>

using namespace cccaster::domain;
std::string Read(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
bool WaitText(const std::filesystem::path &path, const std::string &needle) {
    for (int n = 0; n < 200; ++n) {
        if (Read(path).find(needle) != std::string::npos) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
#define CHECK(x) do { if (!(x)) { std::cerr << "失敗: " << #x << '\n'; std::exit(1); } } while (false)
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"cccaster-score-test-" + std::to_wstring(GetCurrentProcessId()));
    const auto prefix = directory / (L"cccaster-score-" + std::to_wstring(GetCurrentProcessId()));
    auto json = prefix; json += L".json";
    auto text = prefix; text += L".txt";
    std::filesystem::create_directories(directory);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    wchar_t command[] = L"cmd.exe /c exit 0";
    CHECK(CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &process));
    CHECK(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
    const auto stale = directory / (L"cccaster-score-" + std::to_wstring(process.dwProcessId));
    for (const auto *suffix : {L".json", L".txt", L".json.tmp", L".txt.tmp"}) {
        auto path = stale; path += suffix; std::ofstream(path) << "stale";
    }
    const auto unrelated = directory / L"notes.txt";
    std::ofstream(unrelated) << "keep";
    CHECK(ui::score_broadcast::Initialize(directory));
    CHECK(WaitText(json, "\"active\": false"));
    for (const auto *suffix : {L".json", L".txt", L".json.tmp", L".txt.tmp"}) {
        auto path = stale; path += suffix; CHECK(!std::filesystem::exists(path));
    }
    CHECK(Read(unrelated) == "keep");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    session::SessionScoreSnapshot snapshot;
    snapshot.active = true;
    snapshot.p1Wins = 3;
    snapshot.p2Wins = 2;
    snapshot.unresolved = 1;
    snapshot.revision = 6;
    ui::score_broadcast::Publish(snapshot);
    CHECK(WaitText(json, "\"revision\": 6"));
    CHECK(WaitText(text, "P1 3 - 2 P2"));
    CHECK(ui::score_broadcast::Healthy());
    const auto modified = std::filesystem::last_write_time(json);
    ui::score_broadcast::Publish(snapshot);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(std::filesystem::last_write_time(json) == modified);
    ui::score_broadcast::Clear();
    ui::score_broadcast::Shutdown();
    CHECK(Read(json).find("\"active\": false") != std::string::npos);
    CHECK(Read(json).find("\"p1_wins\": 0") != std::string::npos);
    CHECK(Read(text).empty());
    const auto previousSession = Read(json);
    CHECK(ui::score_broadcast::Initialize(directory));
    CHECK(WaitText(json, "\"active\": false"));
    ui::score_broadcast::Shutdown();
    CHECK(Read(json) != previousSession); // 再初期化でsession_idを刷新
    std::filesystem::remove(json);
    std::filesystem::remove(text);
    std::filesystem::remove(unrelated);
    std::filesystem::remove(directory);
    std::cout << "配信出力: 更新・重複抑止・終了クリア・再初期化合格\n";
}
