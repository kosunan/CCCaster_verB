#include "cli_launcher/network_wrapper/SessionNegotiator.hpp"
#include "cli_launcher/GuiSession.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <iostream>

int main(int argc,char** argv) {
    if(argc<5) return 2;
    const bool host=std::string(argv[1])=="host";
    const auto port=static_cast<uint16_t>(std::stoi(argv[3]));
    const int seconds=std::stoi(argv[4]);
    using namespace cccaster::main_app;
    gui::cancelEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!gui::cancelEvent) return 3;
    std::mutex mutex; std::condition_variable wake; bool done=false;
    std::thread deadline([&]{std::unique_lock<std::mutex> lock(mutex); if(!wake.wait_for(lock,std::chrono::seconds(seconds),[&]{return done;})) SetEvent(gui::cancelEvent);});
    std::cout << std::unitbuf;
    const auto begin=std::chrono::steady_clock::now();
    network_wrapper::SessionNegotiator negotiator;
    const auto result=negotiator.RunNegotiation(false,host,host?"":argv[2],port,true,true);
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();
    std::cout << "RESULT success=" << result.success << " local=" << result.localPort << " peerPort=" << result.peerPort << " elapsedMs=" << elapsed << '\n';
    {std::lock_guard<std::mutex> lock(mutex);done=true;} wake.notify_one(); deadline.join();
    CloseHandle(gui::cancelEvent);gui::cancelEvent=nullptr;
    return result.success?0:1;
}
