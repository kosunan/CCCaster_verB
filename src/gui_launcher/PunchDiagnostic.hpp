#pragma once
#include <asio.hpp>
#include <array>
#include <chrono>
#include <random>
#include <string>
#include <algorithm>

// 診断専用ソケット。ゲーム通信・設定には触れない。TickはGUIスレッドから呼ぶ。
namespace cccaster::diagnostic {
using Udp = asio::ip::udp;
inline unsigned Read16(const unsigned char* p) { return (unsigned(p[0]) << 8) | p[1]; }
inline bool Mapped(const unsigned char* p, size_t n, const std::array<unsigned char,20>& request, Udp::endpoint& out) {
    if (n < 20 || Read16(p) != 0x101 || Read16(p+2) != n-20 || (Read16(p+2)&3) ||
        !std::equal(p+4,p+20,request.begin()+4)) return false;
    bool found = false;
    for (size_t i=20; i<n;) {
        if (n-i < 4) return false;
        unsigned type=Read16(p+i), len=Read16(p+i+2);
        if (len > n-i-4) return false;
        if (type==0x20 && len==8 && p[i+4]==0 && p[i+5]==1) {
            asio::ip::address_v4::bytes_type ip{};
            for (int j=0;j<4;++j) ip[j]=p[i+8+j]^request[4+j];
            auto port=Read16(p+i+6)^0x2112;
            if (!port) return false;
            out=Udp::endpoint(asio::ip::address_v4(ip),static_cast<unsigned short>(port)); found=true;
        }
        i+=4+((len+3)&~3u);
        if (i>n) return false;
    }
    return found;
}
inline bool ParseCode(const std::string& s, Udp::endpoint& ep, std::string& token) {
    if (s.size()>100 || s.rfind("P1-",0)!=0) return false;
    auto colon=s.find(':',3), dash=s.find('-',colon==std::string::npos?3:colon);
    if (colon==std::string::npos || dash==std::string::npos) return false;
    auto ps=s.substr(colon+1,dash-colon-1); token=s.substr(dash+1);
    if (ps.empty() || ps.size()>5 || !std::all_of(ps.begin(),ps.end(),[](char c){return c>='0'&&c<='9';}) ||
        token.size()!=32 || !std::all_of(token.begin(),token.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) return false;
    asio::error_code ec; auto ip=asio::ip::make_address_v4(s.substr(3,colon-3),ec);
    unsigned port=static_cast<unsigned>(std::stoul(ps));
    if(ec || !port || port>65535 || ip.is_unspecified() || ip.is_multicast() || ip.to_uint()==0xffffffff) return false;
    ep=Udp::endpoint(ip,static_cast<unsigned short>(port)); return true;
}
class PunchDiagnostic {
    using Clock=std::chrono::steady_clock;
    asio::io_context io;
    Udp::resolver resolver{io};
    Udp::socket socket{io};
    Udp::endpoint server, peer;
    std::array<unsigned char,20> request{};
    std::string token, peerToken;
    Clock::time_point deadline{}, nextSend{}, nextKeep{};
    unsigned generation=0;
    bool resolved=false, gotProbe=false, gotAck=false;
    void Send(const void* p,size_t n,const Udp::endpoint& ep) {
        asio::error_code ec; socket.send_to(asio::buffer(p,n),ep,0,ec);
        if(ec && ec!=asio::error::would_block && ec!=asio::error::try_again) error=ec.message();
    }
    void Probe(char kind) { auto p=std::string("CCP1")+kind+peerToken+token; Send(p.data(),p.size(),peer); }
public:
    enum class State { Idle, Stun, Ready, Testing, Success, StunFailed, Failed, Expired, Cancelled, SocketFailed };
    State state=State::Idle;
    std::string code,error;
    bool Active() const { return socket.is_open(); }
    int Remaining() const { return (std::max)(0,int(std::chrono::duration_cast<std::chrono::seconds>(deadline-Clock::now()).count()+1)); }
    void Stop(State result=State::Cancelled) {
        ++generation; resolver.cancel(); asio::error_code ec; socket.close(ec); state=result; code.clear();
    }
    void Start(unsigned short port,const std::string& host="stun.l.google.com",const std::string& service="19302") {
        Stop(); error.clear(); resolved=gotProbe=gotAck=false; peerToken.clear();
        asio::error_code ec; socket.open(Udp::v4(),ec);
        if(!ec) socket.bind(Udp::endpoint(Udp::v4(),port),ec);
        if(!ec) socket.non_blocking(true,ec);
        if(ec) { error=ec.message(); Stop(State::SocketFailed); return; }
        std::random_device rng; const char* hex="0123456789abcdef"; token.clear();
        for(int i=0;i<32;++i) token+=hex[rng()&15];
        request={0,1,0,0,0x21,0x12,0xa4,0x42};
        for(int i=8;i<20;++i) request[i]=static_cast<unsigned char>(rng());
        state=State::Stun; deadline=Clock::now()+std::chrono::seconds(6); nextSend=Clock::now();
        auto g=generation;
        resolver.async_resolve(Udp::v4(),host,service,[this,g](asio::error_code e,Udp::resolver::results_type r){
            if(g!=generation || state!=State::Stun) return;
            if(e || r.empty()) {error=e.message(); Stop(State::StunFailed);return;}
            server=r.begin()->endpoint(); resolved=true;
        });
    }
    bool Test(const std::string& input) {
        if(state!=State::Ready) return false;
        if(!ParseCode(input,peer,peerToken) || peerToken==token) { error="Invalid peer code / 相手の診断コードを確認してください"; return false; }
        error.clear(); gotProbe=gotAck=false; state=State::Testing;
        deadline=Clock::now()+std::chrono::seconds(6); nextSend=Clock::now(); return true;
    }
    void Tick() {
        io.restart(); io.poll(); if(!Active()) return;
        auto now=Clock::now();
        if(now>=deadline) {
            Stop(state==State::Stun?State::StunFailed:state==State::Testing?State::Failed:state==State::Success?State::Success:State::Expired); return;
        }
        if(state==State::Stun && resolved && now>=nextSend) {
            Send(request.data(),request.size(),server); nextSend=now+std::chrono::seconds(1);
        }
        if((state==State::Ready || state==State::Testing || state==State::Success) && now>=nextKeep) {
            Send(request.data(),request.size(),server); nextKeep=now+std::chrono::seconds(10);
        }
        if((state==State::Testing || state==State::Success) && now>=nextSend) {
            Probe('Q'); nextSend=now+std::chrono::milliseconds(250);
        }
        for(int i=0;i<32;++i) {
            unsigned char data[512]; Udp::endpoint from; asio::error_code ec;
            size_t n=socket.receive_from(asio::buffer(data),from,0,ec);
            if(ec==asio::error::would_block || ec==asio::error::try_again) break;
            if(ec) break;
            if(state==State::Stun && resolved && from==server) {
                Udp::endpoint mapped;
                if(Mapped(data,n,request,mapped)) {
                    code="P1-"+mapped.address().to_string()+":"+std::to_string(mapped.port())+"-"+token;
                    state=State::Ready; deadline=now+std::chrono::seconds(120); nextKeep=now+std::chrono::seconds(10);
                }
            } else if((state==State::Testing || state==State::Success) && from==peer && n==69) {
                std::string p(reinterpret_cast<char*>(data),n);
                if(p.substr(0,4)!="CCP1" || p.substr(5,32)!=token || p.substr(37)!=peerToken) continue;
                if(p[4]=='Q') {gotProbe=true; Probe('A');}
                if(p[4]=='A') gotAck=true;
                if(gotProbe && gotAck && state==State::Testing) state=State::Success;
                // 成功後も元の6秒締切まで応答し、相手側の確認を妨げない。
            }
        }
    }
};
}
