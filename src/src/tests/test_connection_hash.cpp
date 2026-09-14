#include "cli_launcher/network_wrapper/ConnectionHash.hpp"
#include <winsock2.h>
#include <algorithm>
#include <iostream>
#include <random>
#include <stdexcept>

namespace cccaster::main_app::network_wrapper {
struct ConnectionHashTestAccess {
    static bool Decode(const std::string& s, ConnectionHash::DecodedAddress& out, uint64_t now) {
        return ConnectionHash::DecodeAt(s, out, now);
    }
    static std::string Base62(const std::vector<uint8_t>& data) { return ConnectionHash::Base62Encode(data); }
    static bool Unbase62(const std::string& s, std::vector<uint8_t>& data) { return ConnectionHash::Base62Decode(s, data); }
    static uint16_t Crc(const std::vector<uint8_t>& data) { return ConnectionHash::Checksum(data); }
    static std::string At(uint64_t timestamp, bool legacy) {
        auto bytes = ConnectionHash::BuildPlainPayload("203.0.113.7", "2001:db8::1234", 7500,
            0x01020304, timestamp, "192.168.10.2");
        ConnectionHash::XorCipher(bytes, ConnectionHash::GenerateKeyStream(timestamp / 21600, bytes.size()));
        if (legacy) return "ABCD" + ConnectionHash::Base32Encode(bytes);
        const auto crc = Crc(bytes); bytes.push_back(crc >> 8); bytes.push_back(crc & 255);
        return "1" + Base62(bytes);
    }
};
}
using Hash = cccaster::main_app::network_wrapper::ConnectionHash;
using Access = cccaster::main_app::network_wrapper::ConnectionHashTestAccess;
static int checks = 0;
static void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}

int main() {
    WSADATA wsa{}; if (WSAStartup(MAKEWORD(2,2), &wsa)) return 2;
    try {
        const std::string alphabet = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
        Check(Access::Crc({'1','2','3','4','5','6','7','8','9'}) == 0x29b1, "CRC known answer");
        Check(Access::Base62({0,0,1}) == "001", "leading zeros");
        Check(Access::Base62({255,255}) == "H31", "Base62 known answer");
        std::mt19937 random(73);
        for (int i = 0; i < 500; ++i) {
            std::vector<uint8_t> input(1 + i % 37), output;
            for (auto& byte : input) byte = static_cast<uint8_t>(random());
            if (i % 2 == 0) input.front() = 0;
            Check(Access::Unbase62(Access::Base62(input), output) && input == output, "Base62 byte round trip");
        }
        // 独立したPython実装で計算した固定ベクトル。時刻に依存せず旧コードを検証する。
        constexpr uint64_t now = 1800000000;
        const std::string old = "ABCDOHB7C44JRHI72UX3VWHQYDRLUBIQRQMJZHO6CJPYRTYYR5MSSK3UZAUM";
        const std::string compact = "18QDDd9RKKbRvh3L2vpqLvwc3wEYrq7f3hNUphCHVZIVTldLiG2";
        Check(Access::At(now, true) == old, "legacy encoder fixture");
        Check(Access::At(now, false) == compact, "compact encoder fixture");
        for (auto code : {old, compact}) {
            Hash::DecodedAddress out;
            Check(Access::Decode(code, out, now), "fixed fixture decode");
            Check(out.ipv4 == "203.0.113.7" && out.ipv6 == "2001:db8::1234" &&
                  out.localIpv4 == "192.168.10.2" && out.port == 7500 && out.sessionToken == 0x01020304,
                  "preserve every field");
            Check(out.publicKey == (code == old ? "ABCD" : ""), "legacy identifier only");
            Check(Access::Decode(code, out, now + 21600), "six hours inclusive");
            Check(!Access::Decode(code, out, now + 21601) && out.isExpired, "six hours expired");
        }
        std::string lower = old;
        for (char& c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        Hash::DecodedAddress out;
        Check(Access::Decode(lower, out, now), "legacy lowercase compatibility");
        Check(Access::Decode(old.substr(0,4) + "-" + old.substr(4), out, now), "legacy separator compatibility");
        for (bool legacy : {false, true}) {
            const uint64_t boundary = (now / 21600 + 1) * 21600;
            for (uint64_t offset : {1ULL, 8ULL, 60ULL, 200ULL})
                Check(Access::Decode(Access::At(boundary - offset, legacy), out, boundary + 1), "time window boundary");
        }
        for (size_t pos = 1; pos < compact.size(); ++pos) {
            for (char c : alphabet) {
                if (compact[pos] == c) continue;
                auto damaged = compact; damaged[pos] = c;
                Check(!Access::Decode(damaged, out, now), "single character typo fixture");
            }
        }
        for (auto bad : std::vector<std::string>{"", "1", "1!", "10", "0ABCD", "S-" + compact, compact + "0",
                         compact.substr(0,compact.size()-1), std::string(10000, '1')}) {
            out.ipv4 = "stale"; out.isExpired = true;
            Check(!Access::Decode(bad, out, now) && out.ipv4.empty(), "bad input clears result");
        }
        for (unsigned flags = 1; flags < 8; ++flags) {
            const std::string v4 = (flags & 1) ? "203.0.113.7" : "";
            const std::string v6 = (flags & 2) ? "2001:db8::1234" : "";
            const std::string local = (flags & 4) ? "192.168.10.2" : "";
            for (uint16_t port : {uint16_t(1), uint16_t(7500), uint16_t(65535)}) {
                const auto code = Hash::Encode(v4, v6, port, local);
                Check(code.front() == '1' && code.find_first_not_of(alphabet) == std::string::npos, "compact alphanumeric");
                Check(Hash::Decode(code, out) && out.ipv4 == v4 && out.ipv6 == v6 && out.localIpv4 == local &&
                      out.port == port && out.sessionToken != 0 && out.publicKey.empty(), "public API round trip");
                if (flags == 1) Check(code.size() <= 24, "IPv4 length");
                if (flags == 5) Check(code.size() <= 30, "IPv4 plus LAN length");
                if (flags == 7) Check(code.size() <= 51, "dual stack plus LAN length");
                Check(Hash::DecodeSpectator(code, out) && out.port == port, "spectator plain");
                Check(Hash::DecodeSpectator("S-" + code, out) && out.port == port, "spectator legacy");
                Check(Hash::DecodeSpectator("s-" + code, out), "spectator lowercase prefix");
                Check(!Hash::DecodeSpectator("S-S-" + code, out), "spectator invalid prefix");
            }
        }
        std::cout << checks << " connection-code checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << "Check " << checks << " failed: " << e.what() << '\n'; WSACleanup(); return 1;
    }
    WSACleanup(); return 0;
}
