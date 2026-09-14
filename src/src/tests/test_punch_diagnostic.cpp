#include "gui_launcher/PunchDiagnostic.hpp"
#include "test_support.hpp"
#include <thread>
using namespace cccaster::diagnostic;
int main(int argc, char** argv) {
    if (argc==2 && std::string(argv[1])=="--public-stun") {
        PunchDiagnostic probe; probe.Start(0);
        auto began=std::chrono::steady_clock::now();
        while(probe.state==PunchDiagnostic::State::Stun) {
            probe.Tick(); std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::printf("public_stun_ready=%d elapsed_ms=%lld (peer connectivity untested)\n",
            probe.state==PunchDiagnostic::State::Ready,
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-began).count()));
        return probe.state==PunchDiagnostic::State::Ready ? 0 : 1;
    }
    CC_CASE("コード検証");
    Udp::endpoint endpoint; std::string token;
    const std::string suffix(32,'a');
    CC_CHECK(ParseCode("P1-127.0.0.1:1234-"+suffix,endpoint,token));
    CC_CHECK(!ParseCode("P1-127.0.0.1:65536-"+suffix,endpoint,token));
    CC_CHECK(!ParseCode("P1-127.0.0.1:0-"+suffix,endpoint,token));
    CC_CHECK(!ParseCode("P1-224.0.0.1:1234-"+suffix,endpoint,token));
    CC_CHECK(!ParseCode("P1-127.0.0.1:1234-short",endpoint,token));
    asio::io_context io;
    Udp::socket stun(io,Udp::endpoint(Udp::v4(),0)); stun.non_blocking(true);
    const auto service=std::to_string(stun.local_endpoint().port());
    Udp::socket blackhole(io,Udp::endpoint(Udp::v4(),0));
    PunchDiagnostic a,b,timeout,cancelled,occupied,stunTimeout;
    stunTimeout.Start(0,"127.0.0.1",std::to_string(blackhole.local_endpoint().port()));
    a.Start(0,"127.0.0.1",service); b.Start(0,"127.0.0.1",service);
    timeout.Start(0,"127.0.0.1",service);
    cancelled.Start(0,"127.0.0.1",service); cancelled.Stop();
    occupied.Start(stun.local_endpoint().port(),"127.0.0.1",service);
    CC_CHECK(occupied.state==PunchDiagnostic::State::SocketFailed);
    bool tested=false, parserTested=false;
    auto began=std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now()-began<std::chrono::milliseconds(6500)) {
        a.Tick(); b.Tick(); timeout.Tick(); cancelled.Tick(); stunTimeout.Tick();
        // 3番目の診断にもSTUN応答を返すが、相手は応答しない。
        for(int j=0;j<8;++j) {
            unsigned char p[512]{}; Udp::endpoint from; asio::error_code ec;
            auto n=stun.receive_from(asio::buffer(p),from,0,ec); if(ec) break;
            if(n!=20) continue;
            std::array<unsigned char,20> req{}; std::copy(p,p+20,req.begin());
            p[0]=1;p[1]=1;p[2]=0;p[3]=12;
            p[20]=0;p[21]=0x20;p[22]=0;p[23]=8;p[24]=0;p[25]=1;
            auto port=from.port()^0x2112;p[26]=port>>8;p[27]=port&255;
            auto bytes=from.address().to_v4().to_bytes();
            for(int k=0;k<4;++k) p[28+k]=bytes[k]^req[4+k];
            if(!parserTested) {
                CC_CASE("STUN長さとtransaction ID");
                CC_CHECK(Mapped(p,32,req,endpoint)); CC_CHECK(endpoint==from);
                CC_CHECK(!Mapped(p,31,req,endpoint)); p[8]^=1;
                CC_CHECK(!Mapped(p,32,req,endpoint)); p[8]^=1;
                p[23]=12; CC_CHECK(!Mapped(p,32,req,endpoint)); p[23]=8;
                parserTested=true;
            }
            stun.send_to(asio::buffer(p,32),from,0,ec);
        }
        if(!tested && a.state==PunchDiagnostic::State::Ready && b.state==PunchDiagnostic::State::Ready && timeout.state==PunchDiagnostic::State::Ready) {
            CC_CASE("実UDP双方向診断と自己コード拒否");
            CC_CHECK(!a.Test(a.code));
            auto ac=a.code,bc=b.code;
            CC_CHECK(a.Test(bc));CC_CHECK(b.Test(ac));
            CC_CHECK(timeout.Test("P1-127.0.0.1:"+service+"-"+suffix));tested=true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CC_CHECK(tested); CC_CHECK(parserTested);
    CC_CHECK(a.state==PunchDiagnostic::State::Success);
    CC_CHECK(b.state==PunchDiagnostic::State::Success);
    CC_CHECK(timeout.state==PunchDiagnostic::State::Failed);
    CC_CHECK(!timeout.Active()); CC_CHECK(timeout.code.empty());
    CC_CHECK(cancelled.state==PunchDiagnostic::State::Cancelled);
    CC_CHECK(!cancelled.Active());
    CC_CHECK(stunTimeout.state==PunchDiagnostic::State::StunFailed);
    CC_CHECK(!stunTimeout.Active());
    return cccaster::test::Summarize("punch_diagnostic");
}
