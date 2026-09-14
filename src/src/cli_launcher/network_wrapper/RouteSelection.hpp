#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace cccaster::main_app::network_wrapper::route {
enum class Preference : uint8_t { Auto, IPv4, IPv6 };
enum class Type : uint8_t { Ping = 1, Pong, Select, Accept };
// 起動前だけの候補確認。対戦の通信版10・接続コードには変更を加えない。
struct Packet {
    Type type = Type::Ping;
    bool punched = false;
    uint8_t available = 0, attempted = 0;
    uint32_t token = 0, sequence = 0, processingUs = 0;
    uint64_t nonce = 0, echoNonce = 0;
    uint16_t v6Port = 0;
    std::array<uint8_t,16> v6{};
};
inline std::vector<uint8_t> Encode(const Packet& p) {
    std::vector<uint8_t> b{'C','C','B','R','T','E',0,1};
    auto put = [&](uint64_t n, int count) { while(count--) { b.push_back(uint8_t(n)); n >>= 8; } };
    put(uint8_t(p.type),1); put(p.punched,1); put(p.available,1); put(p.attempted,1);
    put(p.token,4); put(p.nonce,8); put(p.echoNonce,8); put(p.sequence,4); put(p.v6Port,2);
    b.insert(b.end(),p.v6.begin(),p.v6.end()); put(p.processingUs,4); return b;
}
inline bool Decode(const std::vector<uint8_t>& b, Packet& p) {
    const std::array<uint8_t,8> magic{'C','C','B','R','T','E',0,1};
    if(b.size()!=58 || !std::equal(magic.begin(),magic.end(),b.begin()) ||
       b[8]<1 || b[8]>4 || b[9]>1 || b[10]>3 || b[11]>3) return false;
    size_t offset=12;
    auto get = [&](int count) { uint64_t n=0; for(int i=0;i<count;++i) n|=uint64_t(b[offset++])<<(8*i); return n; };
    p.type=Type(b[8]); p.punched=b[9]!=0; p.available=b[10]; p.attempted=b[11];
    p.token=uint32_t(get(4)); p.nonce=get(8); p.echoNonce=get(8); p.sequence=uint32_t(get(4)); p.v6Port=uint16_t(get(2));
    std::copy_n(b.begin()+offset,16,p.v6.begin()); offset+=16; p.processingUs=uint32_t(get(4)); return p.nonce!=0;
}
struct Quality {
    unsigned sent = 0;
    std::vector<double> rtt;
    bool Usable() const { return !rtt.empty(); }
    double Loss() const { return sent ? 100.0*(sent-std::min<size_t>(sent,rtt.size()))/sent : 100.0; }
    double Percentile(double fraction) const {
        if(rtt.empty()) return 0;
        auto sorted=rtt; std::sort(sorted.begin(),sorted.end());
        return sorted[static_cast<size_t>((sorted.size()-1)*fraction)];
    }
    double Jitter() const { return Percentile(.9)-Percentile(.1); }
    // 損失1%を10ms相当とし、平均だけでは隠れる遅延の尾と測定不足を加味する。
    double Score() const { return Usable() ? Loss()*10 + Percentile(.9) + Jitter() + (rtt.size()<3 ? 100 : 0)
                                            : std::numeric_limits<double>::infinity(); }
};
inline int Choose(const std::vector<Quality>& quality, const std::vector<bool>& ipv6, Preference preference) {
    int best=-1;
    for(size_t i=0;i<quality.size();++i) {
        if(!quality[i].Usable()) continue;
        const bool preferred=preference!=Preference::Auto && ipv6[i]==(preference==Preference::IPv6);
        const bool bestPreferred=best>=0 && preference!=Preference::Auto && ipv6[best]==(preference==Preference::IPv6);
        if(best<0 || (preferred && !bestPreferred) || (preferred==bestPreferred &&
           (quality[i].Score()<quality[best].Score()-1 ||
            (std::abs(quality[i].Score()-quality[best].Score())<=1 && ipv6[i] && !ipv6[best])))) best=int(i);
    }
    return best;
}
}
