// ConnectionHash Encode/Decode ラウンドトリップテスト
// ビルド: g++ -std=c++17 -I../../include test_connection_hash.cpp ../network_wrapper/ConnectionHash.cpp -lws2_32 -o test_hash.exe

#include "cli_launcher/network_wrapper/ConnectionHash.hpp"
#include <iostream>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#endif

using Hash = cccaster::main_app::network_wrapper::ConnectionHash;

int main() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    int passed = 0;
    int failed = 0;

    auto check = [&](const char *name, bool condition) {
        if (condition) {
            std::cout << "  [PASS] " << name << "\n";
            passed++;
        } else {
            std::cout << "  [FAIL] " << name << "\n";
            failed++;
        }
    };

    std::cout << "\n=== ConnectionHash Encode/Decode Test ===\n\n";

    // Test 1: IPv4 only
    {
        std::string hash = Hash::Encode("192.168.1.100", "", 10800);
        std::cout << "  Hash (IPv4):  " << hash << "\n";
        check("IPv4 hash not empty", !hash.empty());
        check("IPv4 hash no dash (double-click friendly)", hash.find('-') == std::string::npos);

        Hash::DecodedAddress addr;
        bool ok = Hash::Decode(hash, addr);
        check("IPv4 decode success", ok);
        check("IPv4 address match", addr.ipv4 == "192.168.1.100");
        check("IPv4 port match", addr.port == 10800);
        check("IPv4 no IPv6", addr.ipv6.empty());
        check("IPv4 not expired", !addr.isExpired);
        check("IPv4 token nonzero", addr.sessionToken != 0);
        std::cout << "  Token: 0x" << std::hex << addr.sessionToken << std::dec << "\n\n";
    }

    // Test 2: IPv6 only
    {
        std::string hash = Hash::Encode("", "2001:db8::1", 7777);
        std::cout << "  Hash (IPv6):  " << hash << "\n";

        Hash::DecodedAddress addr;
        bool ok = Hash::Decode(hash, addr);
        check("IPv6 decode success", ok);
        check("IPv6 no IPv4", addr.ipv4.empty());
        check("IPv6 address match", addr.ipv6 == "2001:db8::1");
        check("IPv6 port match", addr.port == 7777);
        check("IPv6 not expired", !addr.isExpired);
        std::cout << "\n";
    }

    // Test 3: Dual-stack (IPv4 + IPv6)
    {
        std::string hash = Hash::Encode("10.0.0.1", "fe80::1", 10800);
        std::cout << "  Hash (Dual):  " << hash << "\n";

        Hash::DecodedAddress addr;
        bool ok = Hash::Decode(hash, addr);
        check("Dual decode success", ok);
        check("Dual IPv4 match", addr.ipv4 == "10.0.0.1");
        check("Dual IPv6 match", addr.ipv6 == "fe80::1");
        check("Dual port match", addr.port == 10800);
        std::cout << "\n";
    }

    // Test 4: 同じ入力でも毎回異なるハッシュ（ワンタイムトークン）
    {
        std::string hash1 = Hash::Encode("1.2.3.4", "", 5000);
        std::string hash2 = Hash::Encode("1.2.3.4", "", 5000);
        std::cout << "  Hash1: " << hash1 << "\n";
        std::cout << "  Hash2: " << hash2 << "\n";
        check("One-time: different hashes", hash1 != hash2);

        // 両方ともデコード可能
        Hash::DecodedAddress a1, a2;
        check("One-time: hash1 decodes", Hash::Decode(hash1, a1));
        check("One-time: hash2 decodes", Hash::Decode(hash2, a2));
        check("One-time: same IP", a1.ipv4 == a2.ipv4);
        check("One-time: same port", a1.port == a2.port);
        check("One-time: different tokens", a1.sessionToken != a2.sessionToken);
        std::cout << "\n";
    }

    // Test 5: 不正なハッシュの拒否
    {
        Hash::DecodedAddress addr;
        check("Invalid: garbage rejected", !Hash::Decode("GARBAGE", addr));
        check("Invalid: empty rejected", !Hash::Decode("", addr));
        check("Invalid: too short rejected", !Hash::Decode("ABCD", addr));
        check("Invalid: wrong format rejected", !Hash::Decode("ABCD!!!!", addr));
        std::cout << "\n";
    }

    // Test 6: 公開鍵の一貫性
    {
        std::string key1 = Hash::GeneratePublicKey();
        std::string key2 = Hash::GeneratePublicKey();
        std::cout << "  Public Key: " << key1 << "\n";
        check("PublicKey: 4 chars", key1.length() == 4);
        check("PublicKey: consistent", key1 == key2);
        std::cout << "\n";
    }

    std::cout << "=== Results: " << passed << " passed, " << failed << " failed ===\n\n";

#ifdef _WIN32
    WSACleanup();
#endif
    return failed > 0 ? 1 : 0;
}
