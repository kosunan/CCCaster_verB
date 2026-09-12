#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace cccaster::main_app::ui {

class ConsoleRenderer {
  public:
    static void EnableVirtualTerminalProcessing();
    static void ClearScreen();
    static void PrintHeader();

    // TUI非同期テキスト入力（ESCキャンセル付き）
    // @param prompt 表示するプロンプト文
    // @param prefill 事前に埋めておく文字列
    // @param allowCancel ESCによるキャンセルを許可するか
    // @return 入力された文字列。ESCキャンセル時はstd::nullopt
    static std::optional<std::string> GetTextInputWithCancel(const std::string &prompt,
                                                             const std::string &prefill = "",
                                                             bool allowCancel = true);

    // TUI描画＆キー入力待機ユーティリティ
    // @param title メニュー上部のタイトル文言
    // @param options 選択肢のリスト
    // @param allowCancel trueならESCキーでのキャンセル（戻り値 -1）を許可する
    // @return 選ばれたインデックス(0〜)。ESCが押された場合は -1
    static int DrawMenuAndGetSelection(const std::string &title, const std::vector<std::string> &options,
                                       bool allowCancel);

    // Network status drawing helpers
    static void PrintNetworkStatus(bool isConnected, uint32_t packetsReceived, uint32_t packetsSent,
                                   double currentPingMs, double currentJitterMs, double lossRate);
};

} // namespace cccaster::main_app::ui
