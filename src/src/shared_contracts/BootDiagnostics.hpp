#pragma once
#include <cstdint>
#include <cstring>
#include <string_view>

namespace cccaster::boot {
inline constexpr uint32_t Magic = 0x43434253, Abi = 1;
enum class Stage : int32_t { Preflight, GameValidation, CreateProcess, EntryLock, DllLoad, Bootstrap,
    RuntimeValidation, Ipc, Patches, Clock, Network, Graphics, Scene, Assets, Ready, EntryRelease, Running };
enum class Error : uint32_t { None, DllFile, DllFormat, BuildMismatch, GameMismatch, Windows,
    Timeout, ChildExited, Ipc, Settings, Patch, Clock, Network, Graphics, Exception, Contract };
struct Descriptor { uint32_t magic, abi, size; char build[65]; };
// 対戦IPC・通信版とは独立。無名共有メモリのhandleを対象子プロセスだけへ複製する。
struct Status {
    uint32_t magic = Magic, abi = Abi, size = sizeof(Status), processId = 0;
    volatile int32_t stage = int32_t(Stage::Bootstrap);
    Error error = Error::None;
    uint32_t win32 = 0, address = 0, patchError = 0, rollbackFailed = 0;
    char expectedBuild[65]{}, dllBuild[65]{}, patchName[48]{};
};
inline bool Compatible(const Descriptor &d, std::string_view build) {
    return d.magic == Magic && d.abi == Abi && d.size == sizeof(Descriptor) &&
        d.build[64] == 0 && build.size() == 64 && std::memcmp(d.build, build.data(), 64) == 0;
}
inline const char *Name(Stage stage) {
    constexpr const char *names[]{"preflight","game_validation","create_process","entry_lock","dll_load",
        "bootstrap","runtime_validation","ipc","patches","clock","network","graphics","scene","assets",
        "ready","entry_release","running"};
    return uint32_t(stage) < std::size(names) ? names[uint32_t(stage)] : "unknown";
}
inline const char *Name(Error error) {
    constexpr const char *names[]{"none","dll_file","dll_format","build_mismatch","game_mismatch","windows",
        "timeout","child_exited","ipc","settings","patch","clock","network","graphics","exception","contract"};
    return uint32_t(error) < std::size(names) ? names[uint32_t(error)] : "unknown";
}
inline const char *Message(Error error, bool japanese) {
    switch (error) {
    case Error::DllFile: return japanese ? "DLLを読み込めません。配置先とファイルのアクセス権を確認してください。" : "Cannot read the DLL. Check its location and file access.";
    case Error::DllFormat: case Error::BuildMismatch: case Error::Contract:
        return japanese ? "ランチャーとDLLの組合せが一致しません。同じ配布物のCLI・GUI・DLLをまとめて配置してください。" : "Launcher and DLL do not match. Install CLI, GUI and DLL from the same package.";
    case Error::GameMismatch: return japanese ? "このEXEでは必要な入力・ゲーム状態の配置を確認できません。詳細ログの対象箇所を確認してください。" : "Required input/state layout is incompatible. Check the affected site in the session log.";
    case Error::Ipc: case Error::Settings: return japanese ? "ゲームへ起動設定を渡せませんでした。設定と詳細ログを確認してください。" : "Could not pass valid startup settings to the game. Check settings and the session log.";
    case Error::Patch: return japanese ? "ゲームへのパッチ適用に失敗したため起動を中止しました。対象箇所は詳細ログに記録しています。" : "Startup stopped because a game patch failed. The affected site is recorded in the session log.";
    case Error::Clock: return japanese ? "ゲームの時計フックを準備できませんでした。詳細ログを確認してください。" : "Could not prepare game timing hooks. Check the session log.";
    case Error::Network: return japanese ? "ゲームの通信ソケットを準備できませんでした。ポートの使用状況を確認してください。" : "Could not prepare the game network socket. Check whether its port is already in use.";
    case Error::Graphics: return japanese ? "描画フックを準備できませんでした。詳細ログを確認してください。" : "Could not prepare graphics hooks. Check the session log.";
    case Error::Timeout: return japanese ? "ゲーム起動が時間切れになりました。停止した段階は詳細ログに記録しています。" : "Game startup timed out. The last startup stage is recorded in the session log.";
    case Error::ChildExited: return japanese ? "初期化中にゲームが終了しました。停止した段階は詳細ログに記録しています。" : "The game exited during initialization. Check the last stage in the session log.";
    default: return japanese ? "ゲームの起動に失敗しました。失敗した段階とエラー番号を詳細ログで確認してください。" : "Game startup failed. Check the stage and error number in the session log.";
    }
}
// GUIは完全な構造化行だけを採用。一般のfailed文言で具体的な診断を上書きしない。
inline Error ParseError(std::string_view log) {
    constexpr std::string_view prefix = "[BOOT_ERROR] code=";
    const auto begin = log.rfind(prefix);
    if (begin == log.npos || (begin && log[begin-1] != '\n')) return Error::None;
    const auto newline = log.find('\n', begin);
    if (newline == log.npos) return Error::None;
    const auto from = begin + prefix.size(), end = log.find(' ', from);
    if (end == log.npos || end >= newline) return Error::None;
    const auto code = log.substr(from, end-from);
    for (uint32_t i = 1; i <= uint32_t(Error::Contract); ++i)
        if (code == Name(Error(i))) return Error(i);
    return Error::None;
}
}
