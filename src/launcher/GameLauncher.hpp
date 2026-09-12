#pragma once

#include <string>
#include <windows.h>
#include <cstdint>
#include "launcher/SpikeDebugger.hpp"

namespace cccaster::main_app {

/**
 * @brief ゲーム(MBAA.exe)の起動とコアDLLの注入を担当する純粋なランチャークラス
 *
 * 【設計方針と役割 (Role & Scope)】
 * 本モジュールは、対象ゲームに対する以下の3つの責務のみを持ちます。
 * 1. ゲームプロセスのSuspend状態での起動 (`CreateProcessA` with `CREATE_SUSPENDED`)
 * 2. エントリポイントのロック（無限ループ化）による初期化前の一時停止状態の担保
 * 3. `cccaster_hook.dll` のインジェクト (`CreateRemoteThread` + `LoadLibraryA`)
 *
 * 【干渉範囲 (Interference Boundaries)】
 * - ゲーム自体の進行状態（メモリ監視）、UI自動遷移、および非アクティブ動作の
 *   パッチなどは本クラスでは行いません。それらのゲーム固有ロジックはすべて
 *   DLL側（`GameHooks::Initialize` 等）へ移譲・カプセル化されています。
 * - 本クラスはDLLインジェクト後、エントリポイントのロックを解除して即座に処理を
 *   ホスト側（`MainController`等）へ返します。
 * - プロセスやスレッドのハンドルは保持されるため、呼び出し元で終了待機が可能です。
 */
class GameLauncher {
  public:
    GameLauncher();
    ~GameLauncher();

    /**
     * @brief ゲームをSuspend状態で起動し、DLLを注入後にResumeする一連のシーケンスを実行する
     * @param exePath 起動するMBAA.exeの絶対パス
     * @return true: 起動、DLLパッチ適用、および実行再開のフローがすべて成功した
     * @return false: APIエラー(ファイル未検出、権限不足、またはコンテキスト取得失敗)が発生した
     */
    /**
     * @brief ゲームプロセスを起動し、目標画面に到達するまで監視・フック注射を行う
     * @param exePath 起動対象のゲーム実行ファイルへのパス
     * @return true 目標画面に到達成功
     * @return false 起動失敗、またはタイムアウトなどの異常終了
     */
    bool BootAndMonitor(const std::string &exePath);
    bool StartSpikeDebugIfRequested();

    /**
     * @brief 起動したゲームプロセスのハンドルを取得します
     * @return プロセスハンドル (監視・待機用)
     */
    HANDLE GetProcessHandle() const {
        return _pi.hProcess;
    }

  private:
    PROCESS_INFORMATION _pi;
    SpikeDebugger _spikeDebugger;
    DWORD _originalEntryPoint;
    WORD _originalEntryPointCode;

    // Core execution steps
    bool LaunchSuspended(const std::string &exePath);
    bool ApplyInitialPatches();
    bool MonitorBootSequence();

    // Memory Utilities
    void WriteMemory(uintptr_t addr, const void *buffer, size_t size);
    void ReadMemory(uintptr_t addr, void *buffer, size_t size);
};

} // namespace cccaster::main_app
