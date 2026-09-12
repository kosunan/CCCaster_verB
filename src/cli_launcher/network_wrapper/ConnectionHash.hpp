#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace cccaster::main_app::network_wrapper {

/// IPv4+IPv6アドレスとポートをBase32ハッシュ文字列にエンコード/デコードするユーティリティ
/// ハッシュフォーマット: [公開鍵4文字]-[Base32(暗号化ペイロード)]
/// セキュリティ: 6時間タイムウィンドウ鍵によるXOR暗号化 + ワンタイムセッショントークン + 有効期限検証
class ConnectionHash {
  public:
    /// デコード結果を格納する構造体
    struct DecodedAddress {
        std::string ipv4;          ///< グローバルIPv4アドレス文字列（空なら未含有）
        std::string ipv6;          ///< IPv6アドレス文字列（空なら未含有）
        std::string localIpv4;     ///< ローカルIPv4アドレス文字列（空なら未含有）[NEW]
        uint16_t port = 0;         ///< ポート番号
        std::string publicKey;     ///< 公開鍵プレフィックス（4文字）
        uint32_t sessionToken = 0; ///< ワンタイムセッショントークン
        bool isExpired = false;    ///< 有効期限切れフラグ
    };

    /// 有効期限: 6時間 (秒)
    static constexpr uint32_t EXPIRY_SECONDS = 6 * 60 * 60;

    /// タイムウィンドウサイズ: 6時間 (秒)
    static constexpr uint32_t TIME_WINDOW_SECONDS = 6 * 60 * 60;

    /// IPv4+IPv6+ローカルIPv4+ポートからハッシュ文字列を生成（暗号化・トークン付き）
    static std::string Encode(const std::string &ipv4, const std::string &ipv6, uint16_t port,
                              const std::string &localIpv4 = "");

    /// ハッシュ文字列からアドレス情報をデコード（復号・期限検証付き）
    static bool Decode(const std::string &hash, DecodedAddress &out);

    /// 公開鍵（マシン固有識別子）を生成
    static std::string GeneratePublicKey();

    /// ローカルIPv4アドレスを取得（最初の非ループバック/非APIPA IPv4）
    static std::string GetLocalIpv4();

  private:
    // Base32 (RFC 4648) エンコード/デコード（パディングなし）
    static std::string Base32Encode(const std::vector<uint8_t> &data);
    static std::vector<uint8_t> Base32Decode(const std::string &encoded);

    // IPv4文字列 ↔ uint32_t 変換
    static uint32_t IpToUint32(const std::string &ip);
    static std::string Uint32ToIp(uint32_t ip);

    // IPv6文字列 ↔ 16バイト配列 変換
    static bool IpToBytes16(const std::string &ip, uint8_t out[16]);
    static std::string Bytes16ToIp(const uint8_t bytes[16]);

    // 6時間タイムウィンドウインデックスからXOR鍵ストリームを生成
    static std::vector<uint8_t> GenerateKeyStream(uint64_t timeWindowIndex, size_t length);

    // ペイロードをXOR暗号化/復号（対称操作）
    static void XorCipher(std::vector<uint8_t> &data, const std::vector<uint8_t> &keyStream);

    // ランダムなワンタイムセッショントークンを生成
    static uint32_t GenerateSessionToken();

    // 現在のUTCタイムスタンプ（秒）を取得
    static uint64_t GetUtcTimestamp();

    // ペイロードを生の平文として構築（トークン＋タイムスタンプ含む）
    static std::vector<uint8_t> BuildPlainPayload(const std::string &ipv4, const std::string &ipv6,
                                                  uint16_t port, uint32_t token, uint64_t timestamp,
                                                  const std::string &localIpv4 = "");

    // 平文ペイロードを解析
    static bool ParsePlainPayload(const std::vector<uint8_t> &payload, DecodedAddress &out,
                                  uint64_t &outTimestamp);
};

} // namespace cccaster::main_app::network_wrapper
