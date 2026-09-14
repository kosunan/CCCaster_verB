#include "cli_launcher/network_wrapper/DualRouteNegotiator.hpp"
#include "shared_contracts/StartupNegotiation.hpp"
#include <conio.h>
#include <mmsystem.h>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

namespace cccaster::main_app::network_wrapper {
namespace {
using Clock=std::chrono::steady_clock;
using Socket=cccaster::network::UdpSocket;
using namespace std::chrono_literals;
struct Received { int family; std::vector<uint8_t> data; std::string ip; uint16_t port; Clock::time_point at; };
struct Inbox { std::mutex mutex; std::deque<Received> packets; };
struct Candidate {
    int family;
    std::string ip;
    uint16_t port;
    bool punched=false;
    route::Quality quality;
    std::map<uint32_t,Clock::time_point> pending;
    Clock::time_point began=Clock::now(), next=Clock::now();
    bool assisted=false;
};
bool V6Address(const std::string& text, asio::ip::address_v6& address) {
    asio::error_code error; address=asio::ip::make_address_v6(text,error);
    return !error && !address.is_unspecified() && !address.is_multicast() &&
           !address.is_link_local() && !address.is_v4_mapped();
}
void Report(int family,const std::string& status) {
    std::cout << "[IP_RESULT] " << (family?"IPv6 ":"IPv4 ") << status << '\n' << std::flush;
}
}

SelectedRoute SelectRoute(const RouteRequest& request) {
    struct TimerResolution { TimerResolution(){timeBeginPeriod(1);} ~TimerResolution(){timeEndPeriod(1);} } timerResolution;
    auto inbox=std::make_shared<Inbox>();
    std::unique_ptr<Socket> sockets[2];
    // IPv6専用ソケットとIPv4ソケットを同じ番号で所有する。
    for(int family=0;family<2;++family) {
        sockets[family]=std::make_unique<Socket>(request.host?request.port:0,family!=0,true);
        if(!sockets[family]->IsValid()) { sockets[family].reset(); Report(family,"bind_failed"); continue; }
        sockets[family]->OnReceive([inbox,family](const auto& data,const auto& ip,uint16_t port) {
            std::lock_guard<std::mutex> lock(inbox->mutex);
            if(inbox->packets.size()<256) inbox->packets.push_back({family,data,ip,port,Clock::now()});
        });
        Report(family,request.host?"waiting":"checking_direct");
    }
    if(!sockets[0]&&!sockets[1]) return {};
    auto now=Clock::now(), began=now, phaseBegan=now, peerBegan=Clock::time_point{};
    uint64_t nonce=uint64_t(now.time_since_epoch().count())^(uint64_t(GetCurrentProcessId())<<32);
    if(!nonce) nonce=1;
    uint64_t peerNonce=0;
    uint8_t peerAttempted=0;
    std::map<std::string,Clock::time_point> rejected;
    uint32_t sequence=1, token=request.token;
    int selected=-1;
    bool punching=false, finishedDirect=false, legacySent=false;
    const bool forcePunch=std::getenv("CCCASTER_TEST_FORCE_RELAY")!=nullptr;
    std::vector<Candidate> candidates;
    auto add=[&](int family,const std::string& text,uint16_t port,bool punched) -> int {
        if(!sockets[family]||!port||!Socket::IsValidIpAddress(text,family!=0)) return -1;
        const auto ip=asio::ip::make_address(text).to_string();
        for(size_t i=0;i<candidates.size();++i)
            if(candidates[i].family==family&&candidates[i].ip==ip&&candidates[i].port==port) return int(i);
        if(candidates.size()>=16) return -1;
        candidates.push_back({family,ip,port,punched,{}, {},Clock::now(),Clock::now()});
        return int(candidates.size()-1);
    };
    if(!request.host) for(const auto& ip:request.addresses) if(!ip.empty()) add(ip.find(':')!=std::string::npos,ip,request.port,false);
    std::string localV6=request.localIpv6;
    if(!request.host && localV6.empty() && sockets[1]) {
        // 接続先への経路に使うIPv6を取得する。connectのみでパケットは送らない。
        for(const auto& c:candidates) if(c.family==1) {
            asio::io_context io; asio::ip::udp::socket routeSocket(io); asio::error_code ec;
            routeSocket.open(asio::ip::udp::v6(),ec);
            if(!ec) routeSocket.connect({asio::ip::make_address(c.ip,ec),c.port},ec);
            if(!ec) localV6=routeSocket.local_endpoint(ec).address().to_string();
            if(!ec&&!localV6.empty()) break;
        }
    }
    asio::ip::address_v6 advertised;
    const bool hasV6=sockets[1]&&V6Address(localV6,advertised);
    auto available=[&]() {
        uint8_t mask=0; for(const auto& c:candidates) if(c.quality.Usable()) mask|=uint8_t(1<<c.family); return mask;
    };
    auto send=[&](int index,route::Type type,uint32_t seq,uint32_t processingUs=0) {
        const auto& c=candidates[index];
        route::Packet packet; packet.type=type; packet.punched=punching; packet.token=token;
        packet.nonce=nonce; packet.echoNonce=peerNonce; packet.sequence=seq; packet.available=available();
        packet.processingUs=processingUs;
        for(const auto& candidate:candidates) if(candidate.quality.sent) packet.attempted|=uint8_t(1<<candidate.family);
        if(hasV6) { packet.v6=advertised.to_bytes(); packet.v6Port=sockets[1]->GetPort(); }
        sockets[c.family]->Send(c.ip,c.port,route::Encode(packet));
    };
    auto reportResults=[&]() {
        for(int family=0;family<2;++family) {
            if(!sockets[family]) continue;
            const Candidate* best=nullptr;
            for(const auto& c:candidates) if(c.family==family&&c.quality.Usable()&&(!best||c.quality.Score()<best->quality.Score())) best=&c;
            if(best) {
                Report(family,best->punched?"ok_punch":"ok_direct");
                std::cout << "[IP_QUALITY] " << (family?"IPv6":"IPv4")
                          << " rtt=" << best->quality.Percentile(.5) << " jitter=" << best->quality.Jitter()
                          << " loss=" << best->quality.Loss() << " samples=" << best->quality.rtt.size() << '\n' << std::flush;
            } else {
                const bool attempted=std::any_of(candidates.begin(),candidates.end(),[&](const auto& c){return c.family==family&&c.quality.sent;});
                Report(family,attempted?(punching?"punch_failed":"direct_failed"):
                    (peerAttempted&(1<<family))?(punching?"peer_punch_failed":"peer_direct_failed"):
                    (punching?"punch_unavailable":"no_candidate"));
            }
        }
    };
    auto finish=[&](int index,bool legacy=false) {
        auto c=candidates[index]; reportResults();
        if(legacy) Report(c.family,"legacy_connected");
        else Report(c.family,c.punched?"selected_punch":"selected_direct");
        std::cout << "[CONNECT_ROUTE] " << (c.punched?"hole_punch":"direct") << '\n' << std::flush;
        // 選択したソケットを起動交渉へ移譲し、OSの割当ポートとNAT状態を保持する。
        return SelectedRoute{std::move(sockets[c.family]),c.ip,c.port,c.family!=0,c.punched,nonce};
    };
    auto beginPunch=[&]() {
        if(punching) return;
        reportResults(); punching=true; finishedDirect=true; phaseBegan=Clock::now(); peerBegan={}; peerAttempted=0;
        for(auto& c:candidates) { c.quality={}; c.pending.clear(); c.began=c.next=Clock::now(); c.punched=true; }
        for(int family=0;family<2;++family) if(sockets[family]) Report(family,family?"exchanging":"checking_punch");
        std::cout << "[CONNECT_STAGE] punch\n" << std::flush;
    };
    std::unique_ptr<LegacyRelay> relay;
    auto startRelay=[&]() {
        if(relay||request.relays.empty()||!sockets[0]) return;
        std::string target;
        for(const auto& ip:request.addresses) if(!ip.empty()&&ip.find(':')==std::string::npos) {target=ip+":"+std::to_string(request.port);break;}
        if(!request.host&&target.empty()) return;
        relay=std::make_unique<LegacyRelay>(request.host,sockets[0]->GetPort(),target,request.relays,
            [&](const auto& ip,uint16_t port,const auto& data){sockets[0]->Send(ip,port,data);},
            [&](const auto& ip,uint16_t port){
                beginPunch(); const auto index=add(0,ip,port,true);
                if(index>=0) candidates[index].assisted=true;
            },
            [&](const char* event) {
                if(std::string(event)=="match") beginPunch();
                std::cout << "[CONNECT_STAGE] " << event << '\n' << std::flush;
            });
    };
    // 募集登録は待受中に維持する。UDPパンチ送信は参加側が直接確認を終えた後のMatchInfoで始まる。
    if(request.host) startRelay();
    std::cout << "[CONNECT_STAGE] " << (request.host?"waiting":"direct") << '\n' << std::flush;
    while(true) {
        now=Clock::now();
        if((request.cancelled&&request.cancelled()) || (!request.headless&&_kbhit()&&_getch()==27)) {
            for(int f=0;f<2;++f) if(sockets[f]) Report(f,"cancelled");
            return {};
        }
        if(relay) relay->Tick();
        std::deque<Received> packets;
        { std::lock_guard<std::mutex> lock(inbox->mutex); packets.swap(inbox->packets); }
        for(const auto& received:packets) {
            const auto endpoint=received.ip+":"+std::to_string(received.port);
            int index=-1;
            for(size_t i=0;i<candidates.size();++i) if(candidates[i].family==received.family&&candidates[i].ip==received.ip&&candidates[i].port==received.port) index=int(i);
            route::Packet packet;
            if(!route::Decode(received.data,packet)) {
                cccaster::public_api::startup::Probe old;
                if(!cccaster::public_api::startup::Decode(received.data,old)) continue;
                if(rejected.count(endpoint)&&now-rejected[endpoint]<std::chrono::seconds(20)) continue;
                if(forcePunch && (index<0||(!received.family&&!candidates[index].assisted))) continue;
                if(selected>=0 && index==selected) return finish(index);
                if(peerNonce || (!request.host&&(!legacySent||index<0))) continue;
                if(index<0) index=add(received.family,received.ip,received.port,punching);
                if(index>=0) return finish(index,true);
                continue;
            }
            if(request.token&&packet.token!=request.token&&!(request.host&&packet.token==0)) {
                if(rejected.size()<16) rejected[endpoint]=now;
                continue;
            }
            if((peerNonce&&packet.nonce!=peerNonce) ||
               packet.nonce==nonce || (packet.echoNonce&&packet.echoNonce!=nonce)) continue;
            if(!request.host&&index<0) continue;
            if(forcePunch && !punching) continue;
            if(forcePunch && !received.family && (index<0||!candidates[index].assisted)) continue;
            if(index<0) index=add(received.family,received.ip,received.port,punching);
            if(index<0) continue;
            if(!peerNonce) {peerNonce=packet.nonce; if(!request.token) token=packet.token; peerBegan=now;}
            if(packet.punched && !punching) beginPunch();
            peerAttempted|=packet.attempted;
            if(peerBegan==Clock::time_point{}) peerBegan=now;
            if(packet.v6Port && sockets[1]) {
                auto v6=asio::ip::address_v6(packet.v6);
                asio::ip::address_v6 checked;
                const bool localPeer=received.ip=="127.0.0.1"||received.ip=="::1";
                if(V6Address(v6.to_string(),checked)&&(!v6.is_loopback()||localPeer)) {
                    // 直接接続段階は参加側から募集側へ確認する。パンチ段階では双方から送る。
                    if(!request.host||punching) add(1,v6.to_string(),packet.v6Port,punching);
                }
            }
            if(packet.type==route::Type::Ping) send(index,route::Type::Pong,packet.sequence,
                uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-received.at).count()));
            else if(packet.type==route::Type::Pong && packet.echoNonce==nonce) {
                auto& c=candidates[index]; auto pending=c.pending.find(packet.sequence);
                if(pending!=c.pending.end()) {
                    const double elapsed=std::chrono::duration<double,std::milli>(received.at-pending->second).count();
                    if(packet.processingUs/1000.0>elapsed) continue;
                    c.quality.rtt.push_back(elapsed-packet.processingUs/1000.0);
                    c.pending.erase(pending);
                }
            } else if(packet.type==route::Type::Select && !request.host && packet.echoNonce==nonce) {
                if(!candidates[index].quality.Usable()) continue;
                send(index,route::Type::Accept,packet.sequence);
                return finish(index);
            } else if(packet.type==route::Type::Accept && request.host && index==selected && packet.echoNonce==nonce) return finish(index);
        }
        for(size_t i=0;i<candidates.size();++i) {
            auto& c=candidates[i];
            if(forcePunch&&!punching) continue;
            if(punching&&!c.family&&!c.assisted) continue;
            if(now<c.next) continue;
            c.next=now+50ms;
            // 20回の有限標本。最後の送信から最低500ms待って判定する。
            if(c.quality.sent<20) {
                const auto seq=sequence++; c.pending[seq]=now; ++c.quality.sent; send(int(i),route::Type::Ping,seq);
            }
            if(!request.host&&!peerNonce && now-began>=1000ms) {
                cccaster::public_api::startup::Probe old; old.extended=true; old.nonce=nonce;
                old.timestamp=uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
                sockets[c.family]->Send(c.ip,c.port,cccaster::public_api::startup::Encode(old)); legacySent=true;
            }
            if(int(i)==selected) send(selected,route::Type::Select,0);
        }
        if(!request.host&&!finishedDirect&&now-began>=2000ms) {
            finishedDirect=true; reportResults();
            if(!available()) { beginPunch(); startRelay(); }
        }
        if(request.host&&peerBegan!=Clock::time_point{}&&selected<0&&now-peerBegan>=2000ms) {
            std::vector<route::Quality> quality; std::vector<bool> families;
            for(const auto& c:candidates) {quality.push_back(c.quality);families.push_back(c.family!=0);}
            selected=route::Choose(quality,families,request.preference);
            if(selected>=0) {reportResults(); send(selected,route::Type::Select,0); phaseBegan=now;}
        }
        if(!request.host&&now-began>=16000ms) {
            reportResults(); std::cout << "[CONNECT_STAGE] timeout\n" << std::flush; return {};
        }
        // 無応答・キャンセルした参加者で募集を消費しない。
        if(request.host && (peerNonce||punching) && now-phaseBegan>=12000ms && now-(peerBegan==Clock::time_point{}?phaseBegan:peerBegan)>=12000ms) {
            reportResults(); candidates.clear(); peerNonce=0; peerBegan={}; selected=-1;
            punching=finishedDirect=false; token=request.token; peerAttempted=0; phaseBegan=now;
            for(int f=0;f<2;++f) if(sockets[f]) Report(f,"waiting");
            std::cout << "[CONNECT_STAGE] waiting\n" << std::flush;
        }
        std::this_thread::sleep_for(5ms);
    }
}
}
