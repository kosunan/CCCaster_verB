#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <cstdio>
#include "shared_contracts/SessionExitReason.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace cccaster::public_api {

// Windows Shared Memory Name (Local prefix restricts to current user session)
constexpr const char *IPC_SHARED_MEM_NAME = "Local\\CCCasterV10_SharedState";
constexpr uint32_t IPC_VERSION_MAGIC = 0xCC100002;

/**
 * @brief ゲーム起動時にDLLへ指示するモードの列挙型です。
 */
enum class IpcGameMode : uint32_t { Versus = 0, Training = 1, Spectator = 2, VersusCPU = 3, Replay = 4 };

/**
 * @brief セッション終了時の業務エラー（切断理由）を表す列挙型です。
 */
enum class SessionErrorType : uint32_t {
    None = 0,
    SyncTimeout = 1,      // 初期同期フェーズでのタイムアウト
    PeerDisconnected = 2, // 対戦中(Phase 3以降)の通信途絶
    AbortedByUser = 3,    // ユーザーによる中断 (F12など)
    PeerClosed = 4       // 相手のゲーム終了通知を受信
};

// Strict 1-byte alignment to prevent Padding differences between 32-bit (MBAA) and 64-bit processes
#pragma pack(push, 1)
struct SharedState {
    uint32_t magicVersion; // Magic number to verify version match

    // Application State
    uint32_t targetGameMode; // 0: Versus, 1: Training, 2: Spectate

    // Network Info
    bool isHost;
    bool isIpv6;
    uint16_t port;
    uint16_t localPort; // CLI が実際にバインドしたポート (Host=port, Client=0)
    char targetIp[64];  // Null-terminated string

    // Negotiation完了後の接続先情報 (FastBoot中にMainControllerが設定)
    char peerIp[64];   // 確定した相手のIP
    uint16_t peerPort; // 確定した相手のPort

    // Game/Netplay Settings
    uint8_t delayFrames;
    uint8_t maxRollbackFrames;
    char playerName[32]; // Null-terminated string

    // Controller Configuration
    int32_t p1DeviceIndex; // -1 for default, 0+ for specific device
    int32_t p2DeviceIndex; // -1 for default, 0+ for specific device

    // Synchronization Flags
    bool dllInitialized;
    bool syncCompleted; // true: DLL側NetplaySession同期完了
    bool gameShutdownRequest;
    bool headlessMode; // true: AIテストモード (ランダム入力注入)

    // Error & Termination Reporting
    uint32_t lastErrorCode; // 0 = None, 1 = Disconnect, 2 = Desync, etc.

    // Real-time Performance Metrics (Direct writing by DLL has ~0 overhead)
    uint32_t currentPingMs;
    uint32_t currentJitterMs;
    uint32_t totalRollbackFrames;
    uint32_t localExitReason; // 終了前に保存。送信と終了はランチャーが担当。
    uint32_t peerExitReason;
};
#pragma pack(pop)

#ifdef _WIN32
/**
 * @brief Inline IPC Manager avoiding the need for a separate .cpp file.
 * Safely wraps CreateFileMapping / MapViewOfFile for easy consumption by both EXE and DLL.
 */
class IpcManager {
  public:
    // ランチャーで生成した名前をCreateProcessの環境継承で子ゲームへ渡す。
    // プロセスごとに固定し、同一PCの別対戦と共有領域を混同しない。
    static const char *MappingName() {
        static const std::string name = [] {
            char value[128]{};
            const DWORD n = GetEnvironmentVariableA("CCCASTER_IPC_NAME", value, sizeof(value));
            return n > 0 && n < sizeof(value) ? std::string(value) : std::string(IPC_SHARED_MEM_NAME);
        }();
        return name.c_str();
    }
    // ---- For EXE (Creator) ----
    // Creates the shared memory and returns a handle. The handle must be kept open
    // for the lifetime of the game process so the memory isn't destroyed by the OS.
    static HANDLE CreateAndWrite(const SharedState &state) {
        char name[128]{};
        std::snprintf(name, sizeof(name), "%s_%lu", IPC_SHARED_MEM_NAME, GetCurrentProcessId());
        if (!SetEnvironmentVariableA("CCCASTER_IPC_NAME", name))
            return NULL;
        HANDLE hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, // Use paging file
                                             NULL,                 // Default security
                                             PAGE_READWRITE,       // Read/write access
                                             0,                    // Maximum object size (high-order DWORD)
                                             sizeof(SharedState),  // Maximum object size (low-order DWORD)
                                             MappingName());       // 子ゲームだけが同じ名前を継承する

        if (hMapFile == NULL) {
            return NULL;
        }

        SharedState *pBuf = (SharedState *)MapViewOfFile(hMapFile,            // Handle to map object
                                                         FILE_MAP_ALL_ACCESS, // Read/write permission
                                                         0, 0, sizeof(SharedState));

        if (pBuf == NULL) {
            CloseHandle(hMapFile);
            return NULL;
        }

        // Copy data into shared memory
        std::memcpy(pBuf, &state, sizeof(SharedState));

        UnmapViewOfFile(pBuf);
        // Return handle to keep it alive
        return hMapFile;
    }

    // Used by EXE to update specific flags (like ShutdownRequest) or read dllInitialized
    template<class Modifier>
    static bool UpdateOrReadState(Modifier modifierFunc) {
        HANDLE hMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, MappingName());
        if (hMapFile == NULL)
            return false;

        SharedState *pBuf =
            (SharedState *)MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState));
        if (pBuf == NULL) {
            CloseHandle(hMapFile);
            return false;
        }

        if (pBuf->magicVersion == IPC_VERSION_MAGIC) {
            modifierFunc(*pBuf);
        }

        UnmapViewOfFile(pBuf);
        CloseHandle(hMapFile);
        return true;
    }

    // ---- For DLL (Reader/Updater) ----
    // Opens the existing shared memory created by EXE and reads it.
    static bool OpenAndRead(SharedState &outState) {
        HANDLE hMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, // Read/write access
                                           FALSE,               // Do not inherit the name
                                           MappingName());      // ランチャーから継承した領域

        if (hMapFile == NULL) {
            return false;
        }

        SharedState *pBuf = (SharedState *)MapViewOfFile(hMapFile,            // Handle to map object
                                                         FILE_MAP_ALL_ACCESS, // Read/write permission
                                                         0, 0, sizeof(SharedState));

        if (pBuf == NULL) {
            CloseHandle(hMapFile);
            return false;
        }

        bool success = false;
        if (pBuf->magicVersion == IPC_VERSION_MAGIC) {
            std::memcpy(&outState, pBuf, sizeof(SharedState));
            success = true;
        }

        UnmapViewOfFile(pBuf);
        CloseHandle(hMapFile);
        return success;
    }
};
#else
/**
 * @brief 非 Windows 版 IpcManager — 全メソッドが no-op。
 *
 * IPC の相手は CLI ランチャー（Windows 専用 EXE）であり、Linux でこれが動くことは
 * 現時点で無い。harness は SceneRunner をそのままリンクするため、
 * 呼び出し側に `#ifdef` を撒くのではなく、ここで受け止めて何もしない。
 *
 * 「共有メモリが無いので書けなかった」と「書いたが誰も読んでいない」は
 * 呼び出し側にとって区別する必要がないため、戻り値は false で統一する。
 * Linux 側で本当に IPC が必要になったら shm_open/mmap でここを実装する。
 */
class IpcManager {
  public:
    static void *CreateAndWrite(const SharedState &) {
        return nullptr;
    }
    template<class Modifier>
    static bool UpdateOrReadState(Modifier) {
        return false;
    }
    static bool OpenAndRead(SharedState &) {
        return false;
    }
};
#endif

// DLLは操作理由の保存だけを行う。送信はDLL終了後も生きるCLI／GUI workerへ委譲。
inline bool RequestLocalGameExit(SessionExitReason reason) {
    bool requested = false;
    IpcManager::UpdateOrReadState([&](SharedState &s) {
        if (s.targetGameMode != static_cast<uint32_t>(IpcGameMode::Versus)) return;
        if (!s.gameShutdownRequest) s.localExitReason = static_cast<uint32_t>(reason);
        s.gameShutdownRequest = true;
        requested = true;
    });
    return requested;
}

} // namespace cccaster::public_api
