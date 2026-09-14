#include "cli_launcher/network_wrapper/RouteSelection.hpp"
#include "tests/test_support.hpp"
using namespace cccaster::main_app::network_wrapper::route;
int main() {
    CC_CASE("候補パケットの厳密な長さ・世代・識別子");
    Packet p,decoded; p.nonce=123; p.echoNonce=456; p.token=789; p.v6[15]=1; p.v6Port=7500;
    const auto valid=Encode(p); CC_CHECK(Decode(valid,decoded));
    CC_CHECK_EQ(decoded.nonce,123); CC_CHECK_EQ(decoded.echoNonce,456);
    CC_CHECK_EQ(decoded.v6Port,7500); CC_CHECK_EQ(decoded.v6[15],1);
    for(size_t i=0;i<valid.size();++i) CC_CHECK(!Decode({valid.begin(),valid.begin()+i},decoded));
    auto invalid=valid; invalid.push_back(0); CC_CHECK(!Decode(invalid,decoded));
    for(auto offset:{0,7,8,9,10,11}) { invalid=valid; invalid[offset]=255; CC_CHECK(!Decode(invalid,decoded)); }
    p.nonce=0; CC_CHECK(!Decode(Encode(p),decoded));
    CC_CASE("自動は品質を選び、優先設定は不通時に代替する");
    Quality good{20,std::vector<double>(20,20)}, slow{20,std::vector<double>(20,80)}, none;
    CC_CHECK_EQ(Choose({good,slow},{false,true},Preference::Auto),0);
    CC_CHECK_EQ(Choose({slow,good},{false,true},Preference::Auto),1);
    CC_CHECK_EQ(Choose({good,slow},{false,true},Preference::IPv6),1);
    CC_CHECK_EQ(Choose({slow,good},{false,true},Preference::IPv4),0);
    CC_CHECK_EQ(Choose({none,good},{false,true},Preference::IPv4),1);
    CC_CHECK_EQ(Choose({good,none},{false,true},Preference::IPv6),0);
    CC_CHECK_EQ(Choose({none,none},{false,true},Preference::Auto),-1);
    CC_CHECK_EQ(Choose({good,good},{false,true},Preference::Auto),1);
    CC_CASE("最低pingが低くても損失や揺れのある経路を避ける");
    Quality loss{20,std::vector<double>(18,1)};
    CC_CHECK_EQ(Choose({loss,good},{false,true},Preference::Auto),1);
    Quality jitter{20,{1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,150,150,150,150}};
    CC_CHECK_EQ(Choose({good,jitter},{false,true},Preference::Auto),0);
    return cccaster::test::Summarize("route_selection");
}
