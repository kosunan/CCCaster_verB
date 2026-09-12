#pragma once

#include <string>
#include <cstdint>

namespace cccaster::main_app::network_wrapper {

// Negotiation結果構造体
struct NegotiationResult {
    bool success = false;
    std::string peerIp;
    uint16_t peerPort = 0;
    uint16_t localPort = 0;
    bool isIpv6 = false;
};

class SessionNegotiator {
  public:
    // Retrieves the Global IP address using ipify
    static std::string GetGlobalIp(bool isIpv6);

    // IPv4/IPv6の両方のグローバルIPを同時取得
    static void GetGlobalIpDual(std::string &outIpv4, std::string &outIpv6);

    // ホスト用: IPv4+IPv6+ポートからハッシュ文字列を生成
    static std::string GenerateConnectionHash(uint16_t port);

    // Parses a user input string into IP and Port, returning true if Valid
    static bool ParseAddressAndPort(const std::string &inputStr, bool isIpv6, std::string &outIp,
                                    uint16_t &outPort, bool &outIsHost);

    // Performs the UDP punching and ping handshake before handing off to the DLL
    // Returns NegotiationResult with peer connection info
    // skipHostDisplay: true の場合、HOST内のグローバルIP取得・画面クリア・クリップボードコピーをスキップ
    //                  （ハッシュモードでは既にGenerateConnectionHash()で完了しているため）
    NegotiationResult RunNegotiation(bool isIpv6, bool isHost, const std::string &targetIp, uint16_t port,
                                     bool isHeadless = false, bool skipHostDisplay = false);

    // ハッシュ文字列からアドレスを解決して接続試行（IPv4優先、失敗時IPv6）
    NegotiationResult RunNegotiationFromHash(const std::string &hash);

    // テキストをクリップボードにコピー
    void CopyToClipboard(const std::string &text);

  private:
};

} // namespace cccaster::main_app::network_wrapper
