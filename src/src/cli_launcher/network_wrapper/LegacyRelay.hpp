#pragma once
#include <asio.hpp>
#ifdef _WIN32
#include <mstcpip.h>
#endif
#include <array>
#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace cccaster::main_app::network_wrapper {
// 旧SmartSocket/server.py互換の接続支援。対戦データを中継しない。
// Tickと全コールバックは呼出スレッド。UDPは本接続のソケットから送信する。
class LegacyRelay {
    using Clock = std::chrono::steady_clock;
    using Tcp = asio::ip::tcp;
public:
    struct Server { std::string host; uint16_t port = 3939; };
    using Send = std::function<void(const std::string&, uint16_t, const std::vector<uint8_t>&)>;
    using Peer = std::function<void(const std::string&, uint16_t)>;
    using Event = std::function<void(const char*)>;
    static bool ParseServer(const std::string& value, Server& out) {
        auto colon = value.rfind(':');
        if (colon == std::string::npos || colon == 0 || value.size()>260) return false;
        const auto host=value.substr(0,colon), port=value.substr(colon+1);
        if(port.empty() || port.size()>5) return false;
        for(char c:host) if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='-')) return false;
        unsigned number=0;
        for(char c:port) { if(c<'0'||c>'9') return false; number=number*10+unsigned(c-'0'); }
        if(!number||number>65535) return false;
        out={host,static_cast<uint16_t>(number)}; return true;
    }
    static uint32_t ReadId(const char* p) {
        return uint32_t(uint8_t(p[0])) | uint32_t(uint8_t(p[1]))<<8 |
               uint32_t(uint8_t(p[2]))<<16 | uint32_t(uint8_t(p[3]))<<24;
    }
private:
    struct Match { Clock::time_point deadline, nextSend; bool hasPeer=false; };
    struct Channel {
        Tcp::resolver resolver;
        Tcp::socket tcp;
        Server server;
        std::string ip, input, registration;
        std::array<char,512> buffer{};
        std::map<uint32_t,Match> matches;
        Clock::time_point deadline{}, retry{};
        unsigned generation=0;
        bool connecting=false, live=false;
        Channel(asio::io_context& io,const Server& s):resolver(io),tcp(io),server(s) {}
    };
    asio::io_context io;
    std::vector<std::unique_ptr<Channel>> channels;
    bool host;
    uint16_t hostingPort;
    std::string target;
    Send send;
    Peer peer;
    Event event;
    bool notifiedUnavailable=false;
    void Fail(Channel& c) {
        ++c.generation; c.resolver.cancel(); asio::error_code ec; c.tcp.close(ec);
        c.connecting=c.live=false; c.matches.clear(); c.input.clear();
        c.retry=Clock::now()+std::chrono::seconds(30);
    }
    bool Parse(Channel& c) {
        for(;;) {
            if(c.input.empty()) return true;
            const std::string matchHeader="MatchInfo", tunHeader="TunInfo";
            const bool match=matchHeader.compare(0,(std::min)(c.input.size(),matchHeader.size()),c.input,0,(std::min)(c.input.size(),matchHeader.size()))==0;
            const bool tun=tunHeader.compare(0,(std::min)(c.input.size(),tunHeader.size()),c.input,0,(std::min)(c.input.size(),tunHeader.size()))==0;
            if(!match&&!tun) return false;
            const size_t header=match?9:7;
            if(c.input.size()<header+4) return true;
            const auto id=ReadId(c.input.data()+header);
            if(!id) return false;
            if(match) {
                if(c.matches.size()>=8 && !c.matches.count(id)) return false;
                // 同一通知の再送は試行期限を延長しない。
                if(!c.matches.count(id)) {
                    c.matches.emplace(id,Match{Clock::now()+std::chrono::seconds(8),Clock::now(),false});
                    event("match");
                }
                c.input.erase(0,header+4);
            } else {
                const auto end=c.input.find('\0',header+4);
                if(end==std::string::npos) return c.input.size()<header+4+22;
                if(end-header-4>21) return false;
                auto found=c.matches.find(id);
                Server endpoint;
                if(!ParseServer(c.input.substr(header+4,end-header-4),endpoint)) return false;
                asio::error_code ec; auto ip=asio::ip::make_address_v4(endpoint.host,ec);
                if(ec||ip.is_unspecified()||ip.is_multicast()||ip.to_uint()==0xffffffff) return false;
                if(found!=c.matches.end()&&Clock::now()<found->second.deadline&&!found->second.hasPeer) {
                    found->second.hasPeer=true;
                    found->second.deadline=Clock::now()+std::chrono::seconds(8);
                    peer(ip.to_string(),endpoint.port); event("punch");
                }
                c.input.erase(0,end+1);
            }
        }
    }
    void Read(Channel& c,unsigned g) {
        c.tcp.async_read_some(asio::buffer(c.buffer),[this,&c,g](asio::error_code ec,size_t n){
            if(g!=c.generation) return;
            if(ec) {Fail(c);return;}
            c.input.append(c.buffer.data(),n);
            if(c.input.size()>4096||!Parse(c)) {Fail(c);return;}
            Read(c,g);
        });
    }
    void Connect(Channel& c) {
        c.connecting=true;
        c.deadline=Clock::now()+std::chrono::seconds(3);
        const auto g=++c.generation;
        c.resolver.async_resolve(Tcp::v4(),c.server.host,std::to_string(c.server.port),[this,&c,g](asio::error_code ec,Tcp::resolver::results_type addresses){
            if(g!=c.generation) return;
            if(ec) {Fail(c);return;}
            asio::async_connect(c.tcp,addresses,[this,&c,g](asio::error_code e,const Tcp::endpoint& endpoint){
                if(g!=c.generation) return;
                if(e) {Fail(c);return;}
                c.ip=endpoint.address().to_string();
                asio::error_code ignored; c.tcp.set_option(asio::socket_base::keep_alive(true),ignored);
#ifdef _WIN32
                // 募集掲示板での長い待機中もTCPを維持し、無応答の切断を検出する。
                // 旧サーバーは任意のアプリ層keepaliveを受け付けないのでOS側で行う。
                tcp_keepalive keepalive{1,30000,10000}; DWORD returned=0;
                if(WSAIoctl(c.tcp.native_handle(),SIO_KEEPALIVE_VALS,&keepalive,sizeof(keepalive),
                            nullptr,0,&returned,nullptr,nullptr)==SOCKET_ERROR) {Fail(c);return;}
#endif
                if(host) c.registration=std::string{'U',char(hostingPort&255),char(hostingPort>>8)};
                else c.registration="U"+target;
                asio::async_write(c.tcp,asio::buffer(c.registration),[this,&c,g](asio::error_code error,size_t){
                    if(g!=c.generation) return;
                    if(error) {Fail(c);return;}
                    c.live=true; c.connecting=false; notifiedUnavailable=false;
                    event("relay_ready"); Read(c,g);
                });
            });
        });
    }
public:
    LegacyRelay(bool isHost,uint16_t port,std::string targetAddress,const std::vector<Server>& servers,Send sendUdp,Peer onPeer,Event onEvent)
        :host(isHost),hostingPort(port),target(std::move(targetAddress)),send(std::move(sendUdp)),peer(std::move(onPeer)),event(std::move(onEvent)) {
        for(const auto& server:servers) if(channels.size()<8) channels.emplace_back(new Channel(io,server));
        for(auto& c:channels) Connect(*c);
    }
    ~LegacyRelay() {
        for(auto& c:channels) Fail(*c);
        // キャンセル完了ハンドラがChannelのバッファを参照し終わってから破棄する。
        io.restart(); io.run();
    }
    bool Available() const {
        for(const auto& c:channels) if(c->live) return true;
        return false;
    }
    void Tick() {
        io.restart(); io.poll(); const auto now=Clock::now();
        bool pending=false;
        for(auto& ptr:channels) {
            auto& c=*ptr;
            if(c.connecting&&now>=c.deadline) Fail(c);
            if(host&&!c.live&&!c.connecting&&now>=c.retry) Connect(c);
            pending=pending||c.connecting||c.live;
            for(auto it=c.matches.begin();it!=c.matches.end();) {
                if(now>=it->second.deadline) {it=c.matches.erase(it);event("attempt_expired");continue;}
                if(now>=it->second.nextSend) {
                    const auto id=it->first;
                    send(c.ip,c.server.port,{uint8_t(host?0:1),uint8_t(id),uint8_t(id>>8),uint8_t(id>>16),uint8_t(id>>24)});
                    it->second.nextSend=now+std::chrono::milliseconds(100);
                }
                ++it;
            }
        }
        if(!pending&&!notifiedUnavailable) {notifiedUnavailable=true;event("relay_unavailable");}
    }
};
}
