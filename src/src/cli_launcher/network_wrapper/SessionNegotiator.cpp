#include "cli_launcher/network_wrapper/SessionNegotiator.hpp"
#include "cli_launcher/network_wrapper/ConnectionHash.hpp"
#include "cli_launcher/network_wrapper/LegacyRelay.hpp"
#include "cli_launcher/ConfigManager.hpp"
#include <fstream>
#include <sstream>
#include <filesystem>
#include "cli_launcher/GuiSession.hpp"
#include "cli_launcher/ui/ConsoleRenderer.hpp"
#include "core_dll/network/UdpSocket.hpp"
#include "shared_contracts/StartupNegotiation.hpp"
#include "shared_contracts/StartupTrace.hpp"
#include <memory>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <stdexcept>
#include <windows.h>
#include <wininet.h>
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif
#include <conio.h>

namespace cccaster::main_app::network_wrapper {

static std::vector<LegacyRelay::Server> RelayServers() {
    if (!ConfigManager::GetInt("Netplay", "RelayEnabled", 1)) return {};
    std::string text;
    if (const char* overrideList = std::getenv("CCCASTER_RELAY_SERVERS")) text = overrideList;
    else {
        wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768);
        std::ifstream file(std::filesystem::path(path).parent_path()/"relay_list.txt");
        if (file) text.assign(std::istreambuf_iterator<char>(file),{});
        else text="melty.argoneus.com:3939\nmelty-backup.argoneus.com:3939\n104.238.130.23:3939";
    }
    std::replace(text.begin(),text.end(),';','\n');
    std::istringstream lines(text); std::string line;
    std::vector<LegacyRelay::Server> servers;
    while(std::getline(lines,line) && servers.size()<8) {
        auto start=line.find_first_not_of(" \t\r");
        if(start==std::string::npos || line[start]=='#') continue;
        line=line.substr(start,line.find_last_not_of(" \t\r")-start+1);
        LegacyRelay::Server parsed;
        if(LegacyRelay::ParseServer(line,parsed)) servers.push_back(parsed);
    }
    return servers;
}

std::string SessionNegotiator::GetGlobalIp(bool isIpv6) {
    ui::ConsoleRenderer::ClearScreen();
    ui::ConsoleRenderer::PrintHeader();
    std::cout << "\n  [ INFO ] Fetching Global IP Address...\n";

    std::string ip = isIpv6 ? "::1" : "127.0.0.1";
    HINTERNET net = InternetOpenA("CCCaster", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (net) {
        DWORD timeout = 3000; // 3 seconds timeout
        InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

        std::string url = isIpv6 ? "http://api6.ipify.org" : "http://api.ipify.org";
        HINTERNET conn = InternetOpenUrlA(net, url.c_str(), NULL, 0, INTERNET_FLAG_RELOAD, 0);
        if (conn) {
            char buffer[128];
            DWORD read;
            if (InternetReadFile(conn, buffer, sizeof(buffer) - 1, &read) && read > 0) {
                buffer[read] = '\0';
                std::string result(buffer);
                result.erase(result.find_last_not_of(" \n\r\t") + 1);
                ip = result;
            }
            InternetCloseHandle(conn);
        }
        InternetCloseHandle(net);
    }
    return ip;
}

void SessionNegotiator::CopyToClipboard(const std::string &text) {
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1);
        if (hMem) {
            memcpy(GlobalLock(hMem), text.c_str(), text.size() + 1);
            GlobalUnlock(hMem);
            SetClipboardData(CF_TEXT, hMem);
        }
        CloseClipboard();
    }
}

bool SessionNegotiator::ParseAddressAndPort(const std::string &inputStr, bool isIpv6, std::string &outIp,
                                            uint16_t &outPort, bool &outIsHost) {
    outIp = isIpv6 ? "::1" : "127.0.0.1";
    outPort = 0;
    outIsHost = false;

    if (inputStr.empty())
        return false;

    try {
        bool isNumberOnly = std::all_of(inputStr.begin(), inputStr.end(), ::isdigit);

        if (isNumberOnly) {
            outIsHost = true;
            outPort = static_cast<uint16_t>(std::stoi(inputStr));
            if (outPort == 0)
                throw std::invalid_argument("Port 0 is reserved");
        } else {
            outIsHost = false;
            auto closeBracket = inputStr.find(']');
            auto colonPos = inputStr.find_last_of(':');

            auto extractPort = [&](size_t pos) {
                outIp = inputStr.substr(0, pos);
                std::string portStr = inputStr.substr(pos + 1);
                if (!portStr.empty())
                    outPort = static_cast<uint16_t>(std::stoi(portStr));
                if (outPort == 0)
                    throw std::invalid_argument("Port 0 is reserved");
            };

            if (closeBracket != std::string::npos) {
                if (closeBracket + 1 < inputStr.size() && inputStr[closeBracket + 1] == ':') {
                    extractPort(closeBracket + 1);
                    outIp = outIp.substr(1, outIp.size() - 2);
                } else {
                    outIp = inputStr.substr(1, closeBracket - 1);
                }
            } else if (colonPos != std::string::npos && !isIpv6) {
                extractPort(colonPos);
            } else {
                outIp = inputStr;
            }

            if (!cccaster::network::UdpSocket::IsValidIpAddress(outIp, isIpv6)) {
                return false;
            }
        }
    } catch (const std::exception &) {
        return false;
    }

    return true;
}

NegotiationResult SessionNegotiator::RunNegotiation(bool isIpv6, bool isHost, const std::string &targetIp,
                                                    uint16_t port, bool isHeadless, bool skipHostDisplay,
                                                    bool allowRelay, int connectTimeoutMs, SelectedRoute selected) {
    if (gui::Cancelled()) return {};
    namespace startup = cccaster::public_api::startup;
    SetEnvironmentVariableA(startup::LocalNonceEnv, nullptr);
    SetEnvironmentVariableA(startup::PeerNonceEnv, nullptr);
    const bool baseline = std::getenv("CCCASTER_STARTUP_BASELINE") != nullptr;
    const char *dropFinalSetting = std::getenv("CCCASTER_TEST_STARTUP_DROP_FINAL");
    const bool dropFinalEnabled = !baseline && dropFinalSetting && std::string(dropFinalSetting) == "1";
    bool droppedFinal = false;
    auto trace = [](const char *name) {
        if (cccaster::diagnostics::startup::Enabled())
            std::cerr << "[Startup] event=" << name << " qpcUs="
                      << cccaster::diagnostics::startup::QpcUs() << std::endl;
    };
    isHeadless = isHeadless || gui::IsWorker();
    timeBeginPeriod(1);

    // clientのportは接続先。自分も同じ番号へbindすると、host起動前に
    // localhost宛probeを自分で受信してしまう。実際の自portはIPCでDLLへ渡す。
    const bool routeSelected = selected.socket != nullptr;
    auto socketOwner = routeSelected ? std::move(selected.socket)
                                    : std::make_unique<cccaster::network::UdpSocket>(isHost ? port : 0, isIpv6);
    auto &socket = *socketOwner;
    if (!socket.IsValid()) {
        std::cout << "\x1b[31m  [ERROR] Failed to bind to Port " << port
                  << ".\n  It may already be in use. Press any key to try again...\x1b[0m";
        if (!isHeadless)
            _getch();
        timeEndPeriod(1);
        return NegotiationResult{};
    }

    if (isHost) {
        if (skipHostDisplay) {
            // ハッシュモード: IP取得・画面クリア・クリップボードコピーは
            // GenerateConnectionHash() + MainController で完了済み。
            // ポート表示のみ行う。
            std::cout << "  \x1b[1;36m[ HOST ]\x1b[0m Waiting on Port " << socket.GetPort() << "...\n";
        } else {
            // 旧 IP:port 直接接続モード: 従来通りグローバルIP取得 + クリップボードコピー
            std::cout << "  \x1b[1;36m[ HOST ]\x1b[0m Waiting on Port " << port << "...\n";
            std::string globalIp = GetGlobalIp(isIpv6);
            std::string hostString = isIpv6 ? ("[" + globalIp + "]:" + std::to_string(port))
                                            : (globalIp + ":" + std::to_string(port));
            CopyToClipboard(hostString);
            std::cout << "  \x1b[32m[ INFO ]\x1b[0m Global IP " << hostString << " copied to clipboard.\n";
        }
    } else {
        std::string displayIp = isIpv6 ? ("[" + targetIp + "]") : targetIp;
        std::cout << "  \x1b[1;36m[ CLIENT ]\x1b[0m Connecting to " << displayIp << ":" << port << "...\n";
    }
    std::cout << "  (Press ESC to cancel)\n\n" << std::flush;

    std::atomic<bool> connected(false);
    std::mutex clientMutex;
    std::string activeClientIp = targetIp;
    uint16_t activeClientPort = port;
    struct Candidate { std::string ip; uint16_t port; std::chrono::steady_clock::time_point expires; };
    std::vector<Candidate> candidates; // clientMutexで受信スレッドと共有する。
    bool punchedConnection = selected.punched;
    const bool forceRelay = !routeSelected && std::getenv("CCCASTER_TEST_FORCE_RELAY") != nullptr;

    std::atomic<uint32_t> packetsReceived(0);
    uint32_t packetsSent = 0;

    std::atomic<uint32_t> expectedSeq(0);
    std::atomic<uint32_t> lostPackets(0);
    std::atomic<double> currentPingMs(0.0);
    std::atomic<double> currentJitterMs(0.0);
    std::atomic<double> lastPingMs(0.0);
    uint16_t seqNum = 0;

    std::atomic<uint64_t> lastReceivedRemoteTime(0);
    std::atomic<uint64_t> lastReceiveLocalTime(0);
    LARGE_INTEGER nonceCounter{};
    QueryPerformanceCounter(&nonceCounter);
    startup::Agreement agreement;
    agreement.localNonce = static_cast<uint64_t>(nonceCounter.QuadPart) ^
                           (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
    if (!agreement.localNonce) agreement.localNonce = 1;
    if(selected.nonce) agreement.localNonce=selected.nonce;
    // socketのcallbackが参照する変数より先に受信スレッドを停止する。
    struct StopReceiver {
        std::unique_ptr<cccaster::network::UdpSocket> &socket;
        ~StopReceiver() { socket.reset(); }
    } stopReceiver{socketOwner};

    socket.OnReceive([&](const std::vector<uint8_t> &data, const std::string &ip, uint16_t recvPort) {
        startup::Probe probe;
        if (!startup::Decode(data, probe)) return;
        {
            std::lock_guard<std::mutex> lock(clientMutex);
            const bool direct = ip == activeClientIp && recvPort == activeClientPort;
            if(routeSelected && !direct) return;
            const bool candidate = std::any_of(candidates.begin(),candidates.end(),[&](const Candidate& c) {
                return c.ip==ip && c.port==recvPort && std::chrono::steady_clock::now()<c.expires;
            });
            if (connected && !direct) return;
            if (!connected && ((!isHost && !direct && !candidate) || (forceRelay && !candidate))) return;
            if (!agreement.Receive(probe)) return;
            if (!connected) {
                activeClientIp = ip;
                activeClientPort = recvPort;
                punchedConnection = selected.punched || candidate;
            }
            connected = true;
        }
        packetsReceived++;

        if (data.size() >= 10) {
            uint16_t remoteSeq = *reinterpret_cast<const uint16_t *>(data.data());
            uint64_t remoteTime = *reinterpret_cast<const uint64_t *>(data.data() + 2);

            lastReceivedRemoteTime = remoteTime;
            lastReceiveLocalTime = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();

            if (expectedSeq != 0 && remoteSeq > expectedSeq) {
                lostPackets += (remoteSeq - expectedSeq);
            }
            expectedSeq = remoteSeq + 1;

            if (data.size() >= 26) {
                uint64_t echoedTime = *reinterpret_cast<const uint64_t *>(data.data() + 10);
                uint64_t remoteProcessingDelay = *reinterpret_cast<const uint64_t *>(data.data() + 18);

                if (echoedTime != 0) {
                    auto nowTime = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();

                    int64_t rttMicro = static_cast<int64_t>(nowTime - echoedTime) -
                                       static_cast<int64_t>(remoteProcessingDelay);
                    if (rttMicro < 0)
                        rttMicro = 0;

                    double pingMs = static_cast<double>(rttMicro) / 1000.0;

                    if (lastPingMs == 0.0) {
                        currentPingMs = pingMs;
                        lastPingMs = pingMs;
                    } else {
                        double diff = std::abs(pingMs - lastPingMs);
                        // Outlier rejection/mitigation for sudden spikes without loss
                        if (diff > 100.0 && lostPackets == 0) {
                            currentPingMs = currentPingMs * 0.95 + pingMs * 0.05;
                        } else {
                            currentJitterMs = currentJitterMs * 0.8 + diff * 0.2;
                            currentPingMs = currentPingMs * 0.8 + pingMs * 0.2;
                        }
                        lastPingMs = pingMs;
                    }
                }
            }
        }
    });

    auto startTime = std::chrono::steady_clock::now();
    auto lastPrintTime = startTime;
    bool wasConnected = false;
    bool lockedIn = false;
    auto connectedTime = std::chrono::steady_clock::time_point::min();
    const auto relayServers = !isIpv6 && allowRelay ? RelayServers() : std::vector<LegacyRelay::Server>{};
    std::unique_ptr<LegacyRelay> relay;
    bool relayStarted=false;
    std::cout << "[CONNECT_STAGE] " << (isHost?"waiting":"direct") << '\n' << std::flush;

    while (true) {
        if (gui::Cancelled()) {
            timeEndPeriod(1);
            return {};
        }
        auto now = std::chrono::steady_clock::now();

        if(!connected && !relayStarted && !relayServers.empty() &&
           (isHost || forceRelay || now-startTime>=std::chrono::seconds(1))) {
            relayStarted=true;
            if(!isHost) std::cout << "[CONNECT_STAGE] relay\n" << std::flush;
            relay.reset(new LegacyRelay(isHost,socket.GetPort(),targetIp+":"+std::to_string(port),relayServers,
                [&](const std::string& ip,uint16_t p,const std::vector<uint8_t>& bytes){socket.Send(ip,p,bytes);},
                [&](const std::string& ip,uint16_t p){
                    std::lock_guard<std::mutex> lock(clientMutex);
                    auto expires=std::chrono::steady_clock::now()+std::chrono::seconds(8);
                    auto found=std::find_if(candidates.begin(),candidates.end(),[&](const Candidate& c){return c.ip==ip&&c.port==p;});
                    if(found==candidates.end() && candidates.size()<16) candidates.push_back({ip,p,expires});
                },
                [&](const char* stage){
                    if(connected) return;
                    if(std::string(stage)=="relay_ready") {
                        if(isHost) std::cout << "[CONNECT_STAGE] relay_waiting\n" << std::flush;
                    } else if(std::string(stage)=="attempt_expired") {
                        if(isHost) std::cout << "[CONNECT_STAGE] attempt_expired\n" << std::flush;
                    } else std::cout << "[CONNECT_STAGE] " << stage << '\n' << std::flush;
                }));
        }
        if(relay && !connected) relay->Tick();
        {
            std::lock_guard<std::mutex> lock(clientMutex);
            candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[&](const Candidate& c){return now>=c.expires;}),candidates.end());
        }
        if((!isHost || routeSelected) && !connected && now-startTime>=std::chrono::milliseconds(connectTimeoutMs)) {
            std::cout << "[CONNECT_STAGE] timeout\n[TIMEOUT] No peer response. Check the host code, hosting status and network.\n" << std::flush;
            timeEndPeriod(1); return {};
        }

        if (connected && !wasConnected) {
            wasConnected = true;
            trace("negotiation_connected");
            std::cout << "[CONNECT_ROUTE] " << (punchedConnection?"hole_punch":"direct") << '\n' << std::flush;
            std::string printIp;
            uint16_t printPort;
            {
                std::lock_guard<std::mutex> lock(clientMutex);
                printIp = activeClientIp;
                printPort = activeClientPort;
            }
            std::cout << "\n  \x1b[32m[ SUCCESS ]\x1b[0m Connection established with \x1b[1;36m[" << printIp
                      << ":" << printPort << "]\n\n\x1b[0m" << std::flush;
        }

        if (connected) {
            if (connectedTime == std::chrono::steady_clock::time_point::min()) {
                connectedTime = std::chrono::steady_clock::now();
            }
            bool ready = false, extendedPeer = false;
            {
                std::lock_guard<std::mutex> lock(clientMutex);
                ready = agreement.Ready();
                extendedPeer = agreement.peerNonce != 0;
            }
            const auto connectedSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - connectedTime).count();
            if ((!baseline && ready) || ((baseline || !extendedPeer) && connectedSeconds >= 2)) {
                std::cout << "\n\nAuto-Locking connection for automated test...\n" << std::flush;
                lockedIn = true;
                break;
            }
            if (connectedSeconds >= 30) {
                std::cout << "[CONNECT_STAGE] handshake_timeout\n" << std::flush;
                if(isHost) {
                    std::lock_guard<std::mutex> lock(clientMutex);
                    connected=false; wasConnected=false; candidates.clear();
                    agreement.peerNonce=0; agreement.echoed=false; agreement.replySent=false;
                    activeClientIp.clear(); activeClientPort=port;
                    lastReceiveLocalTime=0; lastReceivedRemoteTime=0;
                    connectedTime=std::chrono::steady_clock::time_point::min();
                    continue;
                }
                std::cout << "[TIMEOUT] Startup negotiation did not complete.\n" << std::flush;
                timeEndPeriod(1);
                return {};
            }
        }

        if (connected || !isHost || routeSelected || !candidates.empty()) {
            uint64_t timestampNow = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
            uint64_t localProcessingDelay = 0;

            if (lastReceiveLocalTime != 0) {
                localProcessingDelay = timestampNow - lastReceiveLocalTime;
            }

            startup::Probe outgoing;
            outgoing.sequence = seqNum++;
            outgoing.timestamp = timestampNow;
            outgoing.echoedTime = lastReceivedRemoteTime;
            outgoing.processingDelay = localProcessingDelay;
            outgoing.extended = !baseline;

            std::vector<std::pair<std::string,uint16_t>> destinations;
            {
                std::lock_guard<std::mutex> lock(clientMutex);
                if(connected || routeSelected || (!isHost && !forceRelay)) destinations.emplace_back(activeClientIp,activeClientPort);
                if(!connected) for(const auto& candidate:candidates)
                    if(std::find(destinations.begin(),destinations.end(),std::make_pair(candidate.ip,candidate.port))==destinations.end())
                        destinations.emplace_back(candidate.ip,candidate.port);
                outgoing.nonce = agreement.localNonce;
                outgoing.echoNonce = agreement.peerNonce;
            }

            bool dropFinal = false;
            {
                std::lock_guard<std::mutex> lock(clientMutex);
                auto afterSend = agreement;
                if (outgoing.extended) afterSend.Sent(outgoing.echoNonce);
                dropFinal = dropFinalEnabled && !droppedFinal && !agreement.Ready() && afterSend.Ready();
            }
            if (dropFinal) {
                droppedFinal = true;
                std::cerr << "[TestStartup] dropped final probe\n" << std::flush;
            } else {
                for(const auto& dest:destinations) socket.Send(dest.first,dest.second,startup::Encode(outgoing));
            }
            {
                std::lock_guard<std::mutex> lock(clientMutex);
                if (outgoing.extended) agreement.Sent(outgoing.echoNonce);
            }
            packetsSent++;
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPrintTime).count() >= 1) {
            double lossRate = 0.0;
            if (packetsReceived + lostPackets > 0) {
                lossRate = (static_cast<double>(lostPackets) / (packetsReceived + lostPackets)) * 100.0;
            }
            ui::ConsoleRenderer::PrintNetworkStatus(connected, packetsReceived, packetsSent, currentPingMs,
                                                    currentJitterMs, lossRate);
            packetsReceived = 0;
            packetsSent = 0;
            lostPackets = 0;
            lastPrintTime = now;
        }

        // headlessモード時は_kbhit()をスキップ (stdin無しでブロックする可能性があるため)
        if (!isHeadless && _kbhit()) {
            int key = _getch();
            if (key == 27) { // ESC
                timeEndPeriod(1);
                return NegotiationResult{};
            } else if (key == '\r' || key == '\n') {
                bool canLock = false;
                {
                    std::lock_guard<std::mutex> lock(clientMutex);
                    canLock = connected && (baseline || !agreement.peerNonce || agreement.Ready());
                }
                if (canLock) {
                    std::cout << "\n\nConnection Locked. Preparing to launch match...\n" << std::flush;
                    lockedIn = true;
                    // Fix 8: Audio feedback on successful connection lock-in
                    PlaySoundA("beep.wav", NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
                    break;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60FPS loop
    }

    timeEndPeriod(1);

    NegotiationResult result;
    result.success = lockedIn;
    result.isIpv6 = isIpv6;
    if (socket.IsValid()) {
        result.localPort = socket.GetPort();
    }
    if (lockedIn) {
        std::lock_guard<std::mutex> lock(clientMutex);
        result.peerIp = activeClientIp;
        result.peerPort = activeClientPort;
        if (!baseline && agreement.Ready()) {
            SetEnvironmentVariableA(startup::LocalNonceEnv, std::to_string(agreement.localNonce).c_str());
            SetEnvironmentVariableA(startup::PeerNonceEnv, std::to_string(agreement.peerNonce).c_str());
        }
        trace("negotiation_locked");
    }
    return result;
}

// ============================================================
// IPv4/IPv6 デュアル取得
// ============================================================

void SessionNegotiator::GetGlobalIpDual(std::string &outIpv4, std::string &outIpv6) {
    ui::ConsoleRenderer::ClearScreen();
    ui::ConsoleRenderer::PrintHeader();
    std::cout << "\n  [ INFO ] Fetching Global IP Addresses (IPv4 + IPv6)...\n";

    outIpv4 = "";
    outIpv6 = "";

    HINTERNET net = InternetOpenA("CCCaster", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net)
        return;

    DWORD timeout = 3000;
    InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    // IPv4 取得
    {
        HINTERNET conn = InternetOpenUrlA(net, "http://api.ipify.org", NULL, 0, INTERNET_FLAG_RELOAD, 0);
        if (conn) {
            char buffer[128];
            DWORD read;
            if (InternetReadFile(conn, buffer, sizeof(buffer) - 1, &read) && read > 0) {
                buffer[read] = '\0';
                std::string result(buffer);
                result.erase(result.find_last_not_of(" \n\r\t") + 1);
                outIpv4 = result;
            }
            InternetCloseHandle(conn);
        }
    }

    // IPv6 取得
    {
        HINTERNET conn = InternetOpenUrlA(net, "http://api6.ipify.org", NULL, 0, INTERNET_FLAG_RELOAD, 0);
        if (conn) {
            char buffer[128];
            DWORD read;
            if (InternetReadFile(conn, buffer, sizeof(buffer) - 1, &read) && read > 0) {
                buffer[read] = '\0';
                std::string result(buffer);
                result.erase(result.find_last_not_of(" \n\r\t") + 1);
                outIpv6 = result;
            }
            InternetCloseHandle(conn);
        }
    }

    InternetCloseHandle(net);

    if (!outIpv4.empty()) {
        std::cout << "  [ OK ] IPv4: " << outIpv4 << "\n";
    } else {
        std::cout << "  [ -- ] IPv4: Not available\n";
    }
    if (!outIpv6.empty()) {
        std::cout << "  [ OK ] IPv6: " << outIpv6 << "\n";
    } else {
        std::cout << "  [ -- ] IPv6: Not available\n";
    }
}

// ============================================================
// ハッシュ生成 (ホスト用)
// ============================================================

std::string SessionNegotiator::GenerateConnectionHash(uint16_t port) {
    std::string ipv4, ipv6;
    GetGlobalIpDual(ipv4, ipv6);

    // ローカルIPv4も取得してハッシュに含める（同一LAN内接続用）
    std::string localIpv4 = ConnectionHash::GetLocalIpv4();
    if (!localIpv4.empty()) {
        std::cout << "  [ OK ] Local IPv4: " << localIpv4 << "\n";
    } else {
        std::cout << "  [ -- ] Local IPv4: Not available\n";
    }

    return ConnectionHash::Encode(ipv4, ipv6, port, localIpv4);
}

// ============================================================
// ハッシュから接続 (クライアント用)
// ============================================================

NegotiationResult SessionNegotiator::RunAutomatic(RouteRequest request) {
    request.headless=request.headless||gui::IsWorker();
    if(!request.cancelled) request.cancelled=[] {return gui::Cancelled();};
    request.relays=RelayServers();
    auto selected=SelectRoute(request);
    if(!selected.socket) return {};
    const auto ip=selected.ip; const auto port=selected.port; const auto ipv6=selected.ipv6;
    return RunNegotiation(ipv6,request.host,ip,port,request.headless,true,false,12000,std::move(selected));
}

NegotiationResult SessionNegotiator::RunAutomaticHost(uint16_t port,const std::string& hash,route::Preference preference,bool headless) {
    ConnectionHash::DecodedAddress address;
    if(!ConnectionHash::Decode(hash,address)) return {};
    RouteRequest request; request.host=true; request.port=port; request.token=address.sessionToken;
    request.localIpv6=address.ipv6; request.preference=preference; request.headless=headless||gui::IsWorker();
    return RunAutomatic(std::move(request));
}

NegotiationResult SessionNegotiator::RunNegotiationFromHash(const std::string &hash) {
    ConnectionHash::DecodedAddress addr;
    if (!ConnectionHash::Decode(hash, addr)) {
        if (addr.isExpired) {
            std::cout << "\x1b[31m  [ERROR] Connection hash has EXPIRED (6-hour limit).\n"
                      << "  Ask the host to generate a new hash.\n"
                      << "  Press any key to try again...\x1b[0m";
        } else {
            std::cout << "\x1b[31m  [ERROR] Invalid or corrupted connection hash.\n"
                      << "  Press any key to try again...\x1b[0m";
        }
        if (!gui::IsWorker()) _getch();
        return NegotiationResult{};
    }

    std::cout << "\n  \x1b[1;36m[ HASH DECODED ]\x1b[0m\n";
    if (!addr.publicKey.empty()) std::cout << "  Legacy ID:      " << addr.publicKey << "\n";
    std::cout << "  Session Token:  " << std::hex << std::uppercase << addr.sessionToken << std::dec << "\n";
    if (!addr.ipv4.empty())
        std::cout << "  Global IPv4:  " << addr.ipv4 << "\n";
    if (!addr.localIpv4.empty())
        std::cout << "  Local  IPv4:  " << addr.localIpv4 << "\n";
    if (!addr.ipv6.empty())
        std::cout << "  IPv6:         " << addr.ipv6 << "\n";
    std::cout << "  Port:         " << addr.port << "\n\n";

    RouteRequest request; request.port=addr.port; request.token=addr.sessionToken;
    request.addresses={addr.ipv4,addr.ipv6,addr.localIpv4}; request.headless=gui::IsWorker();
    return RunAutomatic(std::move(request));
}

} // namespace cccaster::main_app::network_wrapper
