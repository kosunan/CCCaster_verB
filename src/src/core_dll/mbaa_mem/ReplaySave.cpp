#include "core_dll/mbaa_mem/RealGameMemory.hpp"
#include "core_dll/mbaa_mem/MbaaAddresses.hpp"
#include "core_dll/engine/ReplayFileName.hpp"
#include "core_dll/engine/ReplayFileFormat.hpp"
#include "core_dll/common/DebugLog.hpp"
#include <windows.h>
#include <array>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace cccaster::game_interface {
std::string RealGameMemory::ReplayFilePath() const {
    // 標準Replayオブジェクト+0x94のMSVC文字列。ロード/保存処理と0x407C10で配置を照合。
    const auto *value = reinterpret_cast<const unsigned char *>(0x77BF34 + 0x94);
    if (IsBadReadPtr(value, 28)) return {};
    uint32_t length{}, capacity{};
    std::memcpy(&length, value + 20, 4); std::memcpy(&capacity, value + 24, 4);
    if (!length || length > 1024 || length > capacity) return {};
    const char *data = reinterpret_cast<const char *>(value + 4);
    if (capacity >= 16) std::memcpy(&data, value + 4, sizeof(data));
    if (!data || IsBadReadPtr(data, length + 1) || data[length]) return {};
    return std::string(data, length);
}

bool RealGameMemory::SaveReplay(const char *p1, const char *p2, int winner) {
    using cccaster::domain::session::DebugLog;
    if (GameMode() != CC_GAME_MODE_RETRY) return false;
    // 旧版saveReplayが呼ぶ標準シリアライザ。stdcall(Replay*, filename)、ret 8。
    // 保存UIは通らず、訂正済みの全ラウンドと入力/RNGをゲーム自身に書かせる。
    const unsigned char entry[] = {0x6a, 0xff, 0x68, 0x28, 0x50, 0x51, 0x00};
    if (std::memcmp(reinterpret_cast<void *>(0x446C90), entry, sizeof(entry))) {
        DebugLog("[AutoReplay] FAILED serializer signature"); return false;
    }
    const auto begin = *reinterpret_cast<const uintptr_t *>(0x77BF98);
    const auto end = *reinterpret_cast<const uintptr_t *>(0x77BF9C);
    if (!begin || end <= begin || (end - begin) % 0x140 || end - begin > 0x140 * 10000u ||
        IsBadReadPtr(reinterpret_cast<void *>(begin), end - begin)) {
        DebugLog("[AutoReplay] FAILED round table"); return false;
    }
    char gamePath[MAX_PATH]{};
    const auto length = GetModuleFileNameA(nullptr, gamePath, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    std::string directory(gamePath);
    directory.resize(directory.find_last_of("\\/") + 1);
    directory += "ReplayVS";
    if (!CreateDirectoryA(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        DebugLog("[AutoReplay] FAILED directory error=%lu", GetLastError()); return false;
    }
    SYSTEMTIME now{}; GetLocalTime(&now);
    char timestamp[40];
    std::snprintf(timestamp, sizeof(timestamp), "%04u%02u%02u_%02u%02u%02u_%03u",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    const auto stem = cccaster::domain::session::ReplayFileStem(timestamp, p1, p2, winner);
    static unsigned serial = 0;
    std::string temporary;
    HANDLE reservation = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        temporary = directory + "\\cccaster_" + std::to_string(GetCurrentProcessId()) + "_" +
            std::to_string(++serial) + ".part";
        reservation = CreateFileA(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reservation != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (reservation == INVALID_HANDLE_VALUE) {
        DebugLog("[AutoReplay] FAILED reserve error=%lu", GetLastError()); return false;
    }
    CloseHandle(reservation);
    const auto save = reinterpret_cast<int (__stdcall *)(void *, const char *)>(0x446C90);
    const int result = save(reinterpret_cast<void *>(0x77BF34), temporary.c_str());
    // 元関数は書込み失敗でも1を返し得るため、戻り値だけで成功にしない。
    HANDLE file = CreateFileA(temporary.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    std::array<unsigned char, 0x60> header{};
    DWORD read = 0; LARGE_INTEGER size{};
    bool valid = file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size) &&
        ReadFile(file, header.data(), header.size(), &read, nullptr) && read == header.size() &&
        !std::memcmp(header.data(), "MBAAReplayFile", 14) && size.QuadPart > 0x60;
    uint32_t rounds = 0; std::memcpy(&rounds, header.data() + 0x5c, 4);
    valid = valid && rounds == (end - begin) / 0x140;
    // 手動保存メニューを通らないため、標準一覧の日時欄も明示的に埋める。
    if (valid) {
        const uint16_t date[] = {now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond};
        LARGE_INTEGER dateOffset{}; dateOffset.QuadPart = 24;
        DWORD written = 0;
        valid = SetFilePointerEx(file, dateOffset, nullptr, FILE_BEGIN) &&
            WriteFile(file, date, sizeof(date), &written, nullptr) && written == sizeof(date) && FlushFileBuffers(file);
    }
    if (valid && size.QuadPart <= 256 * 1024 * 1024) {
        std::vector<unsigned char> contents(static_cast<size_t>(size.QuadPart));
        LARGE_INTEGER start{};
        valid = SetFilePointerEx(file, start, nullptr, FILE_BEGIN) &&
            ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &read, nullptr) &&
            read == contents.size() && cccaster::domain::session::ValidReplayFile(contents, rounds);
    } else valid = false;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!valid) {
        DebugLog("[AutoReplay] FAILED output result=%d bytes=%lld rounds=%u partial=%s",
            result, size.QuadPart, rounds, temporary.c_str());
        return false; // 未完成データは.partのまま保持し、リプレイ一覧へ出さない。
    }
    for (unsigned collision = 0; collision < 1000; ++collision) {
        const auto path = directory + "\\" + stem + (collision ? "_" + std::to_string(collision + 1) : "") + ".rep";
        if (MoveFileA(temporary.c_str(), path.c_str())) {
            DebugLog("[AutoReplay] SAVED winner=%d rounds=%u bytes=%lld path=%s", winner, rounds, size.QuadPart, path.c_str());
            return true;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
    }
    DebugLog("[AutoReplay] FAILED publish error=%lu partial=%s", GetLastError(), temporary.c_str());
    return false;
}
}
