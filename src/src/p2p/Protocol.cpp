#include <winsock2.h>
#include <ws2tcpip.h>
#include "p2p/Protocol.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
namespace cccaster::p2p {
namespace {
constexpr char Alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
uint16_t U16(std::span<const uint8_t> b, size_t i) { return uint16_t(b[i]) << 8 | b[i + 1]; }
void Put16(Bytes &b, uint16_t v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
bool Valid(Candidate &c) {
    if (!c.port)
        return false;
    char normalized[64]{};
    uint8_t raw[16]{};
    if (c.type == "v6") {
        if (InetPtonA(AF_INET6, c.ip.c_str(), raw) != 1 || (raw[0] & 0xe0) != 0x20)
            return false;
        InetNtopA(AF_INET6, raw, normalized, sizeof(normalized));
    } else {
        if (c.type != "v4pub" && c.type != "v4lan")
            return false;
        if (InetPtonA(AF_INET, c.ip.c_str(), raw) != 1 || raw[0] == 0 || raw[0] == 127 || raw[0] >= 224 ||
            raw[0] == 169 && raw[1] == 254)
            return false;
        bool local = raw[0] == 10 || (raw[0] == 172 && raw[1] >= 16 && raw[1] <= 31) ||
                     (raw[0] == 192 && raw[1] == 168);
        if ((c.type == "v4lan") != local)
            return false;
        InetNtopA(AF_INET, raw, normalized, sizeof(normalized));
    }
    c.ip = normalized;
    return true;
}
} // namespace
int64_t Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
Json Candidates(const std::vector<Candidate> &values) {
    auto j = Json::array();
    for (const auto &c : values)
        j.push_back({{"t", c.type}, {"a", c.ip}, {"p", c.port}});
    return j;
}
bool ReadCandidates(const Json &json, std::vector<Candidate> &out) {
    out.clear();
    if (!json.is_array() || json.empty() || json.size() > 8)
        return false;
    try {
        for (const auto &j : json) {
            if (!j.is_object() || !j.at("p").is_number_integer())
                return false;
            uint64_t port = j.at("p").get<uint64_t>();
            if (port < 1 || port > 65535)
                return false;
            Candidate c{j.at("t").get<std::string>(), j.at("a").get<std::string>(), uint16_t(port)};
            if (!Valid(c))
                return false;
            if (std::none_of(out.begin(), out.end(),
                             [&](const auto &v) { return c.ip == v.ip && c.port == v.port; }))
                out.push_back(c);
        }
    } catch (const std::exception &) {
        out.clear();
        return false;
    }
    return true;
}
bool ReadMessage(const Keys &keys, const std::string &topic, const std::string &sealed, Json &out,
                 bool fresh) {
    try {
        std::string plain;
        if (!keys.Open(topic, sealed, plain))
            return false;
        out = Json::parse(plain, nullptr, false);
        if (!out.is_object() || !out.at("v").is_number_integer() || out.at("v") != 1 ||
            !out.at("ts").is_number_integer())
            return false;
        auto ts = out.at("ts").get<int64_t>();
        if (fresh && (ts < Now() - 60 || ts > Now() + 60))
            return false;
        auto type = out.at("type").get<std::string>();
        if (type != "status" && type != "request" && type != "answer")
            return false;
        if (type != "status" && Unhex(out.at("session").get<std::string>()).size() != 8)
            return false;
        if (type == "status") {
            auto state = out.at("state").get<std::string>();
            if (state != "waiting" && state != "busy" && state != "closed")
                return false;
        }
        if (type == "answer" && !out.at("accept").is_boolean())
            return false;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
bool ReadSpectatorStatus(const Keys &keys, const std::string &sealed, SpectatorStatus &out) {
    Json j;
    if (!ReadMessage(keys, keys.status, sealed, j, false) || j["type"] != "status")
        return false;
    try {
        SpectatorStatus result;
        result.timestamp = j.at("ts").get<int64_t>();
        if (result.timestamp < Now() - 3600 || result.timestamp > Now() + 60)
            return false;
        result.room = j.at("room").get<std::string>();
        if (Unhex(result.room).size() != 8 || !j.at("revision").is_number_unsigned())
            return false;
        result.revision = j.at("revision").get<uint64_t>();
        if (!result.revision)
            return false;
        result.state = j.at("state").get<std::string>();
        const auto &watch = j.at("spectators");
        if (!watch.at("allowed").is_boolean())
            return false;
        result.allowed = watch.at("allowed").get<bool>();
        if (result.allowed && result.state == "busy" &&
            !ReadCandidates(watch.at("candidates"), result.candidates))
            return false;
        out = std::move(result);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}
bool Admission::Admit(const std::string &session, int64_t now) {
    if (Unhex(session).size() != 8 || seen.count(session) || answers >= 20)
        return false;
    while (!times.empty() && times.front() <= now - 60)
        times.pop_front();
    if (times.size() >= 6)
        return false;
    seen.insert(session);
    times.push_back(now);
    ++answers;
    return true;
}
std::string DirectCode(const std::string &code, const std::string &session, bool host,
                       const std::vector<Candidate> &candidates) {
    auto sid = Unhex(session);
    if (NormalizeCode(code) != code || sid.size() != 8 || candidates.empty())
        return {};
    Bytes raw{1, uint8_t(host)};
    raw.insert(raw.end(), code.begin(), code.end());
    raw.insert(raw.end(), sid.begin(), sid.end());
    // 各種類1候補。短い手動コードにし、残りは通常の候補交換で扱う。
    std::set<std::string> used;
    for (auto c : candidates) {
        if (!Valid(c) || !used.insert(c.type).second)
            continue;
        raw.push_back(c.V6() ? 6 : c.type == "v4lan" ? 1 : 4);
        uint8_t ip[16]{};
        InetPtonA(c.V6() ? AF_INET6 : AF_INET, c.ip.c_str(), ip);
        raw.insert(raw.end(), ip, ip + (c.V6() ? 16 : 4));
        Put16(raw, c.port);
    }
    std::string out = "P1-";
    unsigned acc = 0, bits = 0;
    for (auto c : raw) {
        acc = (acc << 8) | c;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out += Alphabet[(acc >> bits) & 31];
        }
    }
    if (bits)
        out += Alphabet[(acc << (5 - bits)) & 31];
    return out;
}
bool ReadDirectCode(const std::string &text, std::string &code, std::string &session, bool &host,
                    std::vector<Candidate> &candidates) {
    if (text.size() > 400 || text.rfind("P1-", 0) != 0)
        return false;
    Bytes raw;
    unsigned acc = 0, bits = 0;
    for (size_t i = 3; i < text.size(); ++i) {
        char c = text[i];
        if (c == '-' || c == ' ' || c == '\r' || c == '\n')
            continue;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        if (c == 'O')
            c = '0';
        if (c == 'I' || c == 'L')
            c = '1';
        auto p = std::strchr(Alphabet, c);
        if (!c || !p)
            return false;
        acc = (acc << 5) | unsigned(p - Alphabet);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            raw.push_back(uint8_t(acc >> bits));
        }
    }
    if (raw.size() < 23 || raw[0] != 1 || raw[1] > 1 || (bits && (acc & ((1U << bits) - 1))))
        return false;
    host = raw[1] != 0;
    code.assign(raw.begin() + 2, raw.begin() + 8);
    if (NormalizeCode(code) != code)
        return false;
    session = Hex(std::span(raw).subspan(8, 8));
    auto j = Json::array();
    for (size_t i = 16; i < raw.size();) {
        auto type = raw[i++];
        size_t size = type == 6 ? 16 : 4;
        if (type != 6 && type != 4 && type != 1 || i + size + 2 > raw.size())
            return false;
        char ip[64]{};
        if (!InetNtopA(type == 6 ? AF_INET6 : AF_INET, raw.data() + i, ip, sizeof(ip)))
            return false;
        i += size;
        auto port = U16(raw, i);
        i += 2;
        j.push_back({{"t", type == 6 ? "v6" : type == 4 ? "v4pub" : "v4lan"}, {"a", ip}, {"p", port}});
    }
    return ReadCandidates(j, candidates);
}
Bytes EncodeControl(const Key &key, const Control &p) {
    Bytes out{'C', 'B', 'P', 1, uint8_t(uint8_t(p.type) | (p.host ? 0x80 : 0))};
    out.insert(out.end(), p.session.begin(), p.session.end());
    for (int i = 7; i >= 0; --i)
        out.push_back(uint8_t(p.sequence >> (i * 8)));
    if (p.route.size() > 160)
        return {};
    out.insert(out.end(), p.route.begin(), p.route.end());
    auto tag = Mac(key, out);
    out.insert(out.end(), tag.begin(), tag.begin() + 16);
    return out;
}
bool DecodeControl(const Key &key, std::span<const uint8_t> b, Control &p) {
    if (b.size() < 37 || b.size() > 197 || b[0] != 'C' || b[1] != 'B' || b[2] != 'P' || b[3] != 1 ||
        (b[4] & 0x7f) < 1 || (b[4] & 0x7f) > 5)
        return false;
    auto tag = Mac(key, b.first(b.size() - 16));
    if (!Equal(std::span(tag).first(16), b.last(16)))
        return false;
    p.type = ControlType(b[4] & 0x7f);
    p.host = (b[4] & 0x80) != 0;
    std::copy_n(b.begin() + 5, 8, p.session.begin());
    p.sequence = 0;
    for (size_t i = 13; i < 21; ++i)
        p.sequence = (p.sequence << 8) | b[i];
    p.route.assign(b.begin() + 21, b.end() - 16);
    return true;
}
Bytes StunRequest(std::span<const uint8_t> transaction) {
    if (transaction.size() != 12)
        return {};
    Bytes b{0, 1, 0, 0, 0x21, 0x12, 0xa4, 0x42};
    b.insert(b.end(), transaction.begin(), transaction.end());
    return b;
}
bool StunResponse(std::span<const uint8_t> b, std::span<const uint8_t> tx, Candidate &mapped) {
    if (tx.size() != 12 || b.size() < 20 || U16(b, 0) != 0x101 || U16(b, 2) != b.size() - 20 ||
        b[4] != 0x21 || b[5] != 0x12 || b[6] != 0xa4 || b[7] != 0x42 || !Equal(b.subspan(8, 12), tx))
        return false;
    for (size_t i = 20; i + 4 <= b.size();) {
        auto type = U16(b, i), len = U16(b, i + 2);
        i += 4;
        if (i + len > b.size())
            return false;
        if (type == 0x20 && len == 8 && b[i + 1] == 1) {
            uint8_t ip[4];
            for (int j = 0; j < 4; ++j)
                ip[j] = b[i + 4 + j] ^ b[4 + j];
            char address[64]{};
            InetNtopA(AF_INET, ip, address, sizeof(address));
            mapped = {"v4pub", address, uint16_t(U16(b, i + 2) ^ 0x2112)};
            return Valid(mapped);
        }
        i += (len + 3) & ~size_t(3);
    }
    return false;
}
} // namespace cccaster::p2p
