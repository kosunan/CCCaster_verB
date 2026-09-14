#include "cli_launcher/network_wrapper/SessionNegotiator.hpp"
#include <iostream>
int main(int argc,char** argv) {
    // role port ipv4 ipv6 preference localIpv6 timeoutMs token
    if(argc!=9) return 2;
    using namespace cccaster::main_app::network_wrapper;
    RouteRequest request; request.host=std::string(argv[1])=="host";
    request.port=static_cast<uint16_t>(std::stoi(argv[2]));
    for(auto arg:{argv[3],argv[4]}) if(std::string(arg)!="-") request.addresses.emplace_back(arg);
    request.preference=static_cast<route::Preference>(std::stoi(argv[5]));
    if(std::string(argv[6])!="-") request.localIpv6=argv[6];
    const auto begin=std::chrono::steady_clock::now();
    const auto deadline=begin+std::chrono::milliseconds(std::stoi(argv[7]));
    request.token=uint32_t(std::stoul(argv[8]));
    request.cancelled=[&]{return std::chrono::steady_clock::now()>=deadline;};
    std::cout << std::unitbuf;
    SessionNegotiator negotiator;
    const auto result=negotiator.RunAutomatic(request);
    std::cout << "RESULT success=" << result.success << " ipv6=" << result.isIpv6 << " local=" << result.localPort
              << " peer=" << result.peerIp << " peerPort=" << result.peerPort << '\n';
    return result.success?0:1;
}
