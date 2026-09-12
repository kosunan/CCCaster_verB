#pragma once
// 実装は Windows 専用（DirectInput）。ただし harness は本ヘッダを include した上で
// 中身をスタブに差し替えるため、宣言だけは Linux でも読めるようにしておく。
#ifdef _WIN32
#include <windows.h>
#else
using HWND = void *; // Initialize() のシグネチャを両OSで一致させるためだけの別名
#endif
#include <cstdint>
#include <vector>
#include <string>

namespace cccaster::game_interface {

struct JoyDeviceInfo {
    int id;
    char name[256];
    char instanceGuid[40]; // 同名デバイスを個体として区別するDirectInput instance GUID
};

class DirectInputHook {
  public:
    // 素材初期化中に一覧だけを準備。Initialize()が排他内で完了を引き継ぐ。
    static void BeginInitialize();
    static bool Initialize(HWND hwnd);
    static void Shutdown();
    static void RefreshDevices();
    // OS通知を取りこぼす接続方式もあるため、ゲームスレッドから定期的に呼ぶ。
    // 列挙・新規デバイス準備はワーカーで行い、完成した差分だけを採用する。
    static void UpdateHotplug();
    // WndProcでは列挙せず、次のゲームフレームへ再検出を依頼する。
    static void RequestDeviceRefresh();
    static void ReloadConfigs();

    // Call every frame to read gamepad states
    static void Poll();
    static void PollUi();

    // Fetch MBAA input bitmask constructed from INI bindings
    static uint32_t GetPlayer1Input();
    static uint32_t GetPlayer2Input();
    // 通常入力とは別系統。bit0=保存、bit1=読込。用途の判定はSceneRunnerで行う。
    static uint8_t GetTrainingControls();
    static bool IsBindingPressed(int joyId, const std::string &bind);

    /// この機体で操作している人の入力を取る。
    /// @param isHost    ホスト機なら true（P1Device / P2Device のどちらを優先するか）
    /// @param soloLocal ローカルプレイヤーが1人だけか（ネットプレイなら true）。
    ///                  true のとき、優先スロットが未割当ならもう一方に読み替える。
    /// 対戦中はこちらを使うこと。GetPlayerNInput() を直接呼ぶと、
    /// 割り当て先スロットの取り違えで無反応になる（詳細は .cpp のコメント）。
    static uint32_t GetLocalPlayerInput(bool isHost, bool soloLocal);

    // Mapping flow APIs
    static std::vector<JoyDeviceInfo> GetConnectedDevices();

    // Returns 1 if Right was just pressed, -1 if Left, 0 if nothing. Also outputs the joyId.
    // Used for assigning controllers to P1 or P2 in the UI.
    static int GetActiveDeviceDirection(int &outJoyId);

    // Returns a string representation of the newly pressed input (e.g. "Button:1", "POV:0", "Axis:X+")
    // Returns empty string if no new input was detected. Used for the mapping wizard.
    static std::string GetAnyInputEdge(int joyId);

    static void StartMappingPlayer1(int inputIndex);
    static void StartMappingPlayer2(int inputIndex);

    // テストモード: コントローラ入力を無視してパケット値を使用
    static void SetTestModeEnabled(bool enabled);
    static void SetTestInputP1(uint32_t input); // (direction<<16)|buttons
    static void SetTestInputP2(uint32_t input);
};

} // namespace cccaster::game_interface
