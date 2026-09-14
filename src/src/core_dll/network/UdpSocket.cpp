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

namespace cccaster::network {

// ゲーム用QPCフックから独立した時計。CLIは標準時計、DLLはRealMonotonicUs。
struct TransportClock {
    using rep = int64_t;
    using period = std::micro;
    using duration = std::chrono::microseconds;
    using time_point = std::chrono::time_point<TransportClock>;
    static constexpr bool is_steady = true;
    static time_point now() {
        return time_point(duration(NetworkSimulator::Instance().NowUs()));
    }
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
    bool valid = false;
    bool receiving = false;
    std::thread ioThread;
    asio::executor_work_guard<asio::io_context::executor_type> workGuard;

    // 遅延シミュレーション用: 非同期タイマーの寿命を保持するリスト
    std::list<std::shared_ptr<NetworkTimer>> pendingTimers;

    Impl(uint16_t port, bool isIpv6, bool ipv6Only)
        : socket(ioContext), recvBuffer(4096), workGuard(asio::make_work_guard(ioContext)) {
        asio::error_code ec;
        asio::ip::udp::endpoint ep(isIpv6 ? asio::ip::udp::v6() : asio::ip::udp::v4(), port);
        socket.open(ep.protocol(), ec);
        if (!ec) {
            // Options to make localhost testing on the same port possible, and dual stack friendly
            socket.set_option(asio::socket_base::reuse_address(true), ec);
            if (isIpv6) {
                socket.set_option(asio::ip::v6_only(ipv6Only), ec);
            }

            // port 0 allows OS to pick, non-zero tries to bind specific. Check if bind succeeds.
            socket.bind(ep, ec);
            // native sendtoを待機させない。async受信／送信は引き続きASIOが管理する。
            if (!ec) socket.native_non_blocking(true, ec);
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
        if (!socket.is_open())
            return;

        socket.async_receive_from(
            asio::buffer(recvBuffer), remoteEndpoint,
            [this](const asio::error_code &error, std::size_t bytes_transferred) {
                if (!error && bytes_transferred > 0) {
                    if (onReceiveCallback) {
                        std::vector<uint8_t> data(recvBuffer.begin(), recvBuffer.begin() + bytes_transferred);
                        std::string ip = remoteEndpoint.address().to_string();
                        uint16_t port = remoteEndpoint.port();

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

UdpSocket::UdpSocket(uint16_t bindPort, bool isIpv6, bool ipv6Only)
    : _port(bindPort), _impl(std::make_unique<Impl>(bindPort, isIpv6, ipv6Only)) {}

bool UdpSocket::IsValid() const {
    return _impl && _impl->valid;
}

uint16_t UdpSocket::GetPort() const {
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
        _impl->socket.close(ec);
        _impl->workGuard.reset();
        _impl->ioContext.stop();
        if (_impl->ioThread.joinable()) {
            _impl->ioThread.join();
        }
        _impl.reset();
    }
}

bool UdpSocket::ConfigurePeer(const std::string &targetIp, uint16_t targetPort) {
    if (!_impl || !_impl->valid) return false;
    asio::error_code ec;
    auto address = asio::ip::make_address(targetIp, ec);
    if (ec || !targetPort) return false;
    _impl->peerEndpoint = asio::ip::udp::endpoint(address, targetPort);
    _impl->peerIp = targetIp;
    _impl->peerConfigured = true;
    return true;
}

bool UdpSocket::SendPeer(const std::vector<uint8_t> &data, SendProbe probe) {
    if (!_impl || !_impl->peerConfigured || data.size() > std::numeric_limits<int>::max()) return false;
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
    if (probe.report) probe.report(probe, begin, end, success, true);
    return success;
}

void UdpSocket::Send(const std::string &targetIp, uint16_t targetPort, const std::vector<uint8_t> &data,
                     SendProbe probe) {
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
                    if (probe.report) probe.report(probe, begin, end, !error && bytes == bufferPtr->size(), false);
                    // Buffer lifetime guaranteed until completion
                });
        });
    } else {
        // Silently skip invalid IPs during polling
    }
}

void UdpSocket::OnReceive(ReceiveCallback callback) {
    if (_impl && _impl->socket.is_open()) {
        // 候補確認→起動交渉でも同じソケットを維持する。受信スレッドで
        // callbackを交換し、同じバッファへの二重async_receiveを作らない。
        asio::post(_impl->ioContext, [this, callback=std::move(callback)]() mutable {
            _impl->onReceiveCallback=std::move(callback);
            if(!_impl->receiving) { _impl->receiving=true; _impl->DoReceive(); }
        });
    }
}

// 廃止された手動ポーリングメソッド
// void UdpSocket::Poll() {
// }

} // namespace cccaster::network
