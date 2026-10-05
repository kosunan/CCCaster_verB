#include "core_dll/network/UdpSocket.hpp"
#include "core_dll/network/NetworkSimulator.hpp"

// ASIOがシステム(MinGWやvcpkg等)にインストールされている想定
// 無ければ単純なWinsockに差し替えることも可能なようにPimplで隠蔽しています
#define ASIO_STANDALONE
#include <asio.hpp>

#include <list>
#include <memory>
#include <cstdio>
#include <limits>
#include <future>
#include <atomic>
#ifdef _WIN32
#include "p2p/Protocol.hpp"
#endif

namespace cccaster::network {

// ゲーム用QPCフックから独立した時計。CLIは標準時計、DLLはRealMonotonicUs。
struct TransportClock {
    using rep = int64_t;
    using period = std::micro;
    using duration = std::chrono::microseconds;
    using time_point = std::chrono::time_point<TransportClock>;
    static constexpr bool is_steady = true;
    static time_point now() { return time_point(duration(NetworkSimulator::Instance().NowUs())); }
};
using NetworkTimer = asio::basic_waitable_timer<TransportClock>;

struct UdpSocket::Impl {
    asio::io_context ioContext;
    asio::ip::udp::socket socket;
    asio::ip::udp::endpoint remoteEndpoint; // 受信時の送信元を保持する用
    asio::ip::udp::endpoint peerEndpoint;
#ifdef _WIN32
    SOCKET sendHandle = INVALID_SOCKET;
#else
    int sendHandle = -1;
#endif
    std::string peerIp;
    bool peerConfigured = false;
    std::vector<uint8_t> recvBuffer;
    ReceiveCallback onReceiveCallback;
    std::atomic<bool> valid{false};
    bool receiving = false;
    bool paused = false;
#ifdef _WIN32
    bool transferable = false;
    NetworkTimer receivePoll{ioContext};
    bool authenticated = false;
    p2p::Key mac{};
    p2p::Control control{};
    uint64_t receivedSequence = 0;
    int64_t lastReceive = 0;
    NetworkTimer keepalive{ioContext};
    std::function<void()> disconnected;

    void SendControl(p2p::ControlType type, const std::string &route = {}) {
        control.type = type;
        control.route = route;
        control.sequence = std::max(control.sequence + 1, uint64_t(GetTickCount64()) * 1024);
        auto bytes = p2p::EncodeControl(mac, control);
        ::sendto(sendHandle, (const char *)bytes.data(), int(bytes.size()), 0, peerEndpoint.data(),
                 int(peerEndpoint.size()));
    }
    bool ControlPacket(const std::vector<uint8_t> &data, const std::string &ip, uint16_t port) {
        if (!authenticated || data.size() < 3 || data[0] != 'C' || data[1] != 'B' || data[2] != 'P')
            return false;
        p2p::Control packet;
        if (ip != peerIp || port != peerEndpoint.port() || !p2p::DecodeControl(mac, data, packet) ||
            packet.session != control.session || packet.host == control.host)
            return true;
        if (packet.sequence <= receivedSequence)
            return true;
        receivedSequence = packet.sequence;
        lastReceive = NetworkSimulator::Instance().NowUs();
        if (packet.type == p2p::ControlType::Select)
            SendControl(p2p::ControlType::Selected, packet.route);
        else if (packet.type == p2p::ControlType::Punch)
            SendControl(p2p::ControlType::Ack);
        return true;
    }
    void Keepalive() {
        keepalive.expires_after(std::chrono::seconds(15));
        keepalive.async_wait([this](const asio::error_code &ec) {
            if (ec || paused || !authenticated)
                return;
            SendControl(p2p::ControlType::Keepalive);
            if (NetworkSimulator::Instance().NowUs() - lastReceive >= 45000000) {
                // ゲーム固有の切断監視とは別に、P2P制御の45秒監視を終了する。
                valid = false;
                asio::error_code error;
                socket.cancel(error);
                receivePoll.cancel(error);
                if (disconnected)
                    disconnected();
                return;
            }
            Keepalive();
        });
    }
    explicit Impl(std::span<const uint8_t> protocolInfo)
        : socket(ioContext), recvBuffer(4096), workGuard(asio::make_work_guard(ioContext)) {
        if (protocolInfo.size() != sizeof(WSAPROTOCOL_INFOW))
            return;
        WSAPROTOCOL_INFOW info{};
        std::memcpy(&info, protocolInfo.data(), sizeof(info));
        auto handle = WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
                                 WSA_FLAG_OVERLAPPED);
        if (handle == INVALID_SOCKET)
            return;
        asio::error_code ec;
        socket.assign(info.iAddressFamily == AF_INET6 ? asio::ip::udp::v6() : asio::ip::udp::v4(), handle,
                      ec);
        if (ec) {
            closesocket(handle);
            return;
        }
        socket.native_non_blocking(true, ec);
        if (ec)
            return;
        sendHandle = handle;
        valid = true;
        ioThread = std::thread([this] { ioContext.run(); });
    }
#endif
    std::thread ioThread;
    asio::executor_work_guard<asio::io_context::executor_type> workGuard;

    // 遅延シミュレーション用: 非同期タイマーの寿命を保持するリスト
    std::list<std::shared_ptr<NetworkTimer>> pendingTimers;

    Impl(uint16_t port, bool isIpv6, bool ipv6Only, bool exclusive)
        : socket(ioContext), recvBuffer(4096), workGuard(asio::make_work_guard(ioContext)) {
        asio::error_code ec;
        asio::ip::udp::endpoint ep(isIpv6 ? asio::ip::udp::v6() : asio::ip::udp::v4(), port);
#ifdef _WIN32
        if (exclusive) {
            // ゲームに渡す前にはIOCPへ関連付けない。最初の関連付けをDLL側が所有する。
            transferable = true;
            sendHandle = WSASocketW(isIpv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
                                    WSA_FLAG_OVERLAPPED);
            if (sendHandle == INVALID_SOCKET)
                return;
            BOOL yes = TRUE;
            if (setsockopt(sendHandle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&yes, sizeof(yes)) != 0)
                return;
            if (isIpv6) {
                DWORD only = ipv6Only ? 1 : 0;
                if (setsockopt(sendHandle, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&only, sizeof(only)) != 0)
                    return;
            }
            if (::bind(sendHandle, ep.data(), int(ep.size())) != 0)
                return;
            u_long nonblock = 1;
            if (ioctlsocket(sendHandle, FIONBIO, &nonblock) != 0)
                return;
            valid = true;
            ioThread = std::thread([this] { ioContext.run(); });
            return;
        }
#endif
        socket.open(ep.protocol(), ec);
        if (!ec) {
            // Options to make localhost testing on the same port possible, and dual stack friendly
            if (!exclusive)
                socket.set_option(asio::socket_base::reuse_address(true), ec);
#ifdef _WIN32
            else {
                BOOL yes = TRUE;
                if (setsockopt(socket.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char *)&yes,
                               sizeof(yes)) != 0)
                    return;
            }
#endif
            if (isIpv6) {
                socket.set_option(asio::ip::v6_only(ipv6Only), ec);
            }

            // port 0 allows OS to pick, non-zero tries to bind specific. Check if bind succeeds.
            socket.bind(ep, ec);
            // native sendtoを待機させない。async受信／送信は引き続きASIOが管理する。
            if (!ec)
                socket.native_non_blocking(true, ec);
            if (!ec) {
                sendHandle = socket.native_handle();
                valid = true;
                ioThread = std::thread([this]() { ioContext.run(); });
            }
        }
    }

    // 完了済みタイマーをクリーンアップ
    void CleanupTimers() {
        pendingTimers.remove_if([](const std::shared_ptr<NetworkTimer> &t) {
            (void)t;                   // expired timers are cleaned up after callback
            return t.use_count() == 1; // only us holding it = callback done
        });
    }

    void DoReceive() {
#ifdef _WIN32
        if (transferable) {
            if (paused || !valid)
                return;
            for (int i = 0; i < 64; ++i) {
                asio::ip::udp::endpoint endpoint;
                int size = int(endpoint.capacity());
                auto n = recvfrom(sendHandle, (char *)recvBuffer.data(), int(recvBuffer.size()), 0,
                                  endpoint.data(), &size);
                if (n < 0)
                    break;
                endpoint.resize(size);
                std::vector<uint8_t> data(recvBuffer.begin(), recvBuffer.begin() + n);
                auto ip = endpoint.address().to_string();
                auto port = endpoint.port();
                if (!ControlPacket(data, ip, port) && onReceiveCallback)
                    onReceiveCallback(data, ip, port);
            }
            receivePoll.expires_after(std::chrono::milliseconds(5));
            receivePoll.async_wait([this](const asio::error_code &ec) {
                if (!ec)
                    DoReceive();
            });
            return;
        }
#endif
        if (!socket.is_open() || paused)
            return;

        socket.async_receive_from(
            asio::buffer(recvBuffer), remoteEndpoint,
            [this](const asio::error_code &error, std::size_t bytes_transferred) {
                if (!error && bytes_transferred > 0) {
                    if (onReceiveCallback) {
                        std::vector<uint8_t> data(recvBuffer.begin(), recvBuffer.begin() + bytes_transferred);
                        std::string ip = remoteEndpoint.address().to_string();
                        uint16_t port = remoteEndpoint.port();
#ifdef _WIN32
                        if (ControlPacket(data, ip, port)) {
                            DoReceive();
                            return;
                        }
#endif

                        auto &sim = NetworkSimulator::Instance();
                        if (sim.IsEnabled()) {
                            // パケットロス判定
                            if (sim.ShouldDrop()) {
                                // ドロップ — コールバックを呼ばない
                            } else {
                                uint32_t delayMs = sim.GetRandomDelayMs();
                                if (delayMs > 0) {
                                    // 遅延付き受信: steady_timer で遅延後にコールバック
                                    const int64_t began = sim.NowUs();
                                    auto timer = std::make_shared<NetworkTimer>(
                                        ioContext, std::chrono::milliseconds(delayMs));
                                    auto cb = onReceiveCallback; // コピーキャプチャ
                                    pendingTimers.push_back(timer);
                                    timer->async_wait([timer, data, ip, port, cb, this, began,
                                                       delayMs](const asio::error_code &ec) {
                                        if (!ec && cb) {
                                            NetworkSimulator::Instance().ObserveDelay(
                                                delayMs, NetworkSimulator::Instance().NowUs() - began);
                                            cb(data, ip, port);
                                        }
                                        CleanupTimers();
                                    });
                                } else {
                                    onReceiveCallback(data, ip, port);
                                }
                            }
                        } else {
                            // シミュレーション無効: 通常処理
                            onReceiveCallback(data, ip, port);
                        }
                    }
                }
                // エラー時（ポートクローズ等）以外は次に備えて再帰的に待受
                if (error != asio::error::operation_aborted) {
                    DoReceive();
                }
            });
    }
};

UdpSocket::UdpSocket(uint16_t bindPort, bool isIpv6, bool ipv6Only, bool exclusive)
    : _port(bindPort), _impl(std::make_unique<Impl>(bindPort, isIpv6, ipv6Only, exclusive)) {}

#ifdef _WIN32
UdpSocket::UdpSocket(std::span<const uint8_t> info) : _port(0), _impl(std::make_unique<Impl>(info)) {}
std::vector<uint8_t> UdpSocket::DuplicateForProcess(uint32_t processId) {
    if (!IsValid())
        return {};
    Pause();
    WSAPROTOCOL_INFOW info{};
    if (WSADuplicateSocketW(_impl->sendHandle, processId, &info) != 0) {
        Resume();
        return {};
    }
    const auto *begin = reinterpret_cast<const uint8_t *>(&info);
    return {begin, begin + sizeof(info)};
}
void UdpSocket::Pause() {
    if (!_impl || !_impl->ioThread.joinable())
        return;
    std::promise<void> done;
    auto future = done.get_future();
    asio::post(_impl->ioContext, [this, &done] {
        _impl->paused = true;
        _impl->receiving = false;
        asio::error_code ec;
        _impl->keepalive.cancel(ec);
        _impl->receivePoll.cancel(ec);
        _impl->socket.cancel(ec);
        for (auto &timer : _impl->pendingTimers)
            timer->cancel(ec);
        done.set_value();
    });
    future.wait();
}
void UdpSocket::Resume() {
    asio::post(_impl->ioContext, [this] {
        if (_impl->transferable) {
            u_long mode = 1;
            if (ioctlsocket(_impl->sendHandle, FIONBIO, &mode) != 0)
                return;
        }
        _impl->paused = false;
        if (!_impl->receiving) {
            _impl->receiving = true;
            _impl->DoReceive();
        }
        if (_impl->authenticated) {
            _impl->lastReceive = NetworkSimulator::Instance().NowUs();
            _impl->Keepalive();
        }
    });
}
void UdpSocket::EnableP2p(const std::array<uint8_t, 32> &mac, const std::array<uint8_t, 8> &session,
                          const std::string &ip, uint16_t port, bool host,
                          std::function<void()> disconnected) {
    asio::post(_impl->ioContext,
               [this, mac, session, ip, port, host, disconnected = std::move(disconnected)] {
                   if (!ConfigurePeer(ip, port))
                       return;
                   _impl->mac = mac;
                   _impl->control.session = session;
                   _impl->control.host = host;
                   _impl->authenticated = true;
                   _impl->disconnected = disconnected;
                   _impl->lastReceive = NetworkSimulator::Instance().NowUs();
                   _impl->Keepalive();
               });
}
#endif

bool UdpSocket::IsValid() const { return _impl && _impl->valid; }

uint16_t UdpSocket::GetPort() const {
#ifdef _WIN32
    if (_impl && _impl->transferable && _impl->sendHandle != INVALID_SOCKET) {
        asio::ip::udp::endpoint endpoint;
        int size = int(endpoint.capacity());
        if (getsockname(_impl->sendHandle, endpoint.data(), &size) == 0) {
            endpoint.resize(size);
            return endpoint.port();
        }
    }
#endif
    if (_impl && _impl->socket.is_open()) {
        asio::error_code ec;
        auto ep = _impl->socket.local_endpoint(ec);
        if (!ec) {
            return ep.port();
        }
    }
    return _port;
}

bool UdpSocket::IsValidIpAddress(const std::string &ip, bool isIpv6) {
    asio::error_code ec;
    auto addr = asio::ip::make_address(ip, ec);
    if (ec)
        return false;
    if (isIpv6 && !addr.is_v6())
        return false;
    if (!isIpv6 && !addr.is_v4())
        return false;
    return true;
}

UdpSocket::~UdpSocket() {
    if (_impl) {
        // コールバック呼び出し等をキャンセル
        asio::error_code ec;
        // 複製先が存在するとcloseだけでは保留中I/Oが終わらないため、先に取り消す。
        _impl->socket.cancel(ec);
        _impl->socket.close(ec);
        _impl->workGuard.reset();
        _impl->ioContext.stop();
        if (_impl->ioThread.joinable()) {
            _impl->ioThread.join();
        }
#ifdef _WIN32
        if (_impl->transferable && _impl->sendHandle != INVALID_SOCKET)
            closesocket(_impl->sendHandle);
#endif
        _impl.reset();
    }
}

bool UdpSocket::ConfigurePeer(const std::string &targetIp, uint16_t targetPort) {
    if (!_impl || !_impl->valid)
        return false;
    asio::error_code ec;
    auto address = asio::ip::make_address(targetIp, ec);
    if (ec || !targetPort)
        return false;
    _impl->peerEndpoint = asio::ip::udp::endpoint(address, targetPort);
    _impl->peerIp = targetIp;
    _impl->peerConfigured = true;
    return true;
}

bool UdpSocket::SendPeer(const std::vector<uint8_t> &data, SendProbe probe) {
    if (!_impl || !_impl->peerConfigured || data.size() > std::numeric_limits<int>::max())
        return false;
    if (NetworkSimulator::Instance().IsSendEnabled()) {
        // 送信側の遅延・損失注入も保つ。通常の実対戦試験は受信側1回の注入。
        Send(_impl->peerIp, _impl->peerEndpoint.port(), data, probe);
        return true;
    }
    const auto begin = probe.clock ? probe.clock() : 0;
    // ASIOの共有socket内部を別スレッドから操作せず、不変のhandle/宛先だけを使う。
    const auto sent = ::sendto(_impl->sendHandle, reinterpret_cast<const char *>(data.data()),
                               static_cast<int>(data.size()), 0, _impl->peerEndpoint.data(),
                               static_cast<int>(_impl->peerEndpoint.size()));
    const auto end = probe.clock ? probe.clock() : 0;
    const bool success = sent >= 0 && static_cast<size_t>(sent) == data.size();
    if (probe.report)
        probe.report(probe, begin, end, success, true);
    return success;
}

void UdpSocket::Send(const std::string &targetIp, uint16_t targetPort, const std::vector<uint8_t> &data,
                     SendProbe probe) {
#ifdef _WIN32
    if (_impl && _impl->transferable) {
        if (!_impl->valid)
            return;
        asio::error_code error;
        auto ip = asio::ip::make_address(targetIp, error);
        if (error)
            return;
        const asio::ip::udp::endpoint endpoint(ip, targetPort);
        ::sendto(_impl->sendHandle, (const char *)data.data(), int(data.size()), 0, endpoint.data(),
                 int(endpoint.size()));
        return;
    }
#endif
    if (!_impl || !_impl->socket.is_open())
        return;

    auto &sim = NetworkSimulator::Instance();
    if (sim.IsSendEnabled()) {
        // パケットロス判定
        if (sim.ShouldDrop()) {
            return; // 送信しない
        }
    }

    asio::error_code ec;
    auto addr = asio::ip::make_address(targetIp, ec);
    if (!ec) {
        auto endpoint = asio::ip::udp::endpoint(addr, targetPort);
        auto bufferPtr = std::make_shared<std::vector<uint8_t>>(data);

        if (sim.IsSendEnabled()) {
            uint32_t delayMs = sim.GetRandomDelayMs();
            if (delayMs > 0) {
                // 遅延付き送信: steady_timer で遅延後に送信
                asio::post(_impl->ioContext, [this, endpoint, bufferPtr, delayMs]() {
                    const int64_t began = NetworkSimulator::Instance().NowUs();
                    auto timer =
                        std::make_shared<NetworkTimer>(_impl->ioContext, std::chrono::milliseconds(delayMs));
                    _impl->pendingTimers.push_back(timer);
                    timer->async_wait(
                        [this, endpoint, bufferPtr, timer, began, delayMs](const asio::error_code &waitEc) {
                            if (!waitEc && _impl->socket.is_open()) {
                                NetworkSimulator::Instance().ObserveDelay(
                                    delayMs, NetworkSimulator::Instance().NowUs() - began);
                                _impl->socket.async_send_to(
                                    asio::buffer(*bufferPtr), endpoint,
                                    [bufferPtr](const asio::error_code &, std::size_t) {});
                            }
                            _impl->CleanupTimers();
                        });
                });
                return;
            }
        }

        // 遅延なし or シミュレーション無効: 即時送信
        asio::post(_impl->ioContext, [this, endpoint, bufferPtr, probe]() {
            const auto begin = probe.clock ? probe.clock() : 0;
            _impl->socket.async_send_to(
                asio::buffer(*bufferPtr), endpoint,
                [bufferPtr, probe, begin](const asio::error_code &error, std::size_t bytes) {
                    const auto end = probe.clock ? probe.clock() : 0;
                    if (probe.report)
                        probe.report(probe, begin, end, !error && bytes == bufferPtr->size(), false);
                    // Buffer lifetime guaranteed until completion
                });
        });
    } else {
        // Silently skip invalid IPs during polling
    }
}

void UdpSocket::OnReceive(ReceiveCallback callback) {
    if (_impl && _impl->valid) {
        // 候補確認→起動交渉でも同じソケットを維持する。受信スレッドで
        // callbackを交換し、同じバッファへの二重async_receiveを作らない。
        asio::post(_impl->ioContext, [this, callback = std::move(callback)]() mutable {
            _impl->onReceiveCallback = std::move(callback);
            if (!_impl->receiving && !_impl->paused) {
                _impl->receiving = true;
                _impl->DoReceive();
            }
        });
    }
}

// 廃止された手動ポーリングメソッド
// void UdpSocket::Poll() {
// }

} // namespace cccaster::network
