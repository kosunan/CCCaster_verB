#pragma once
// ============================================================================
// PacketRouter.hpp — UDP受信パケットのルーティング
//
// 【設計】
//   CC10統一ヘッダを検証し、全パケットを NetplaySession に転送する。
//   パケット種別の解釈は SyncCodec が行う。
// ============================================================================
#include <cstdint>
#include <vector>
#include <string>

namespace cccaster::core::network {

class PacketRouter {
  public:
    /// CC10統一ヘッダを検証し、NetplaySession に転送する。
    static void OnPacket(const std::vector<uint8_t> &data, const std::string &fromIp, uint16_t fromPort);
};

} // namespace cccaster::core::network
