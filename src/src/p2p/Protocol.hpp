#pragma once
#include "p2p/Crypto.hpp"
#include <nlohmann/json.hpp>
#include <set>
#include <deque>
namespace cccaster::p2p {
using Json = nlohmann::json;
struct Candidate {
    std::string type, ip;
    uint16_t port = 0;
    bool V6() const { return type == "v6"; }
};
Json Candidates(const std::vector<Candidate> &values);
bool ReadCandidates(const Json &json, std::vector<Candidate> &out);
int64_t Now();
bool ReadMessage(const Keys &keys, const std::string &topic, const std::string &sealed, Json &out,
                 bool fresh = true);
struct SpectatorStatus {
    std::string room, state;
    uint64_t revision = 0;
    int64_t timestamp = 0;
    bool allowed = false;
    std::vector<Candidate> candidates;
};
bool ReadSpectatorStatus(const Keys &keys, const std::string &sealed, SpectatorStatus &out);
std::string DirectCode(const std::string &code, const std::string &session, bool host,
                       const std::vector<Candidate> &candidates);
bool ReadDirectCode(const std::string &text, std::string &code, std::string &session, bool &host,
                    std::vector<Candidate> &candidates);
struct Admission {
    std::set<std::string> seen;
    std::deque<int64_t> times;
    unsigned answers = 0;
    bool Admit(const std::string &session, int64_t now);
};
enum class ControlType : uint8_t { Punch = 1, Ack = 2, Select = 3, Selected = 4, Keepalive = 5 };
struct Control {
    ControlType type = ControlType::Punch;
    bool host = false;
    std::array<uint8_t, 8> session{};
    uint64_t sequence = 0;
    std::string route;
};
Bytes EncodeControl(const Key &key, const Control &packet);
bool DecodeControl(const Key &key, std::span<const uint8_t> bytes, Control &packet);
Bytes StunRequest(std::span<const uint8_t> transaction);
bool StunResponse(std::span<const uint8_t> data, std::span<const uint8_t> transaction, Candidate &mapped);
} // namespace cccaster::p2p
