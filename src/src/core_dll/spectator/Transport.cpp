#include "core_dll/spectator/Transport.hpp"
#include <asio.hpp>
#include <chrono>
#include "core_dll/common/DebugLog.hpp"

namespace cccaster::spectator {
using Tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;
namespace {
bool Again(const asio::error_code &e) { return e == asio::error::would_block || e == asio::error::try_again; }
void WorkerPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}
}
void Transport::StartHost(uint16_t port) {
    if (running_.exchange(true)) return;
    status_ = Status::Connecting;
    worker_ = std::thread([this, port] { Serve(port); });
}
void Transport::StartViewer(const std::string &ip, uint16_t port) {
    if (running_.exchange(true)) return;
    status_ = Status::Connecting;
    worker_ = std::thread([this, ip, port] { Receive(ip, port); });
}
void Transport::Stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}
void Transport::Serve(uint16_t port) {
    WorkerPriority();
    try {
        asio::io_context io;
        Tcp::acceptor acceptor(io);
        // IPv4/IPv6両方。IPv6が無い環境ではIPv4へ戻す。
        asio::error_code error;
        acceptor.open(Tcp::v6(), error);
        if (!error) acceptor.set_option(asio::ip::v6_only(false), error);
        const bool ipv6 = !error;
        if (error) { acceptor.close(error); acceptor.open(Tcp::v4()); }
        acceptor.bind(Tcp::endpoint(ipv6 ? Tcp::v6() : Tcp::v4(), port));
        acceptor.listen(8); acceptor.non_blocking(true);
        port_ = acceptor.local_endpoint().port();
        Archive<32768> archive;
        struct Peer {
            Tcp::socket socket;
            std::array<uint8_t, 16> hello{};
            size_t received = 0, offset = 0;
            uint64_t cursor = 0;
            std::array<uint8_t, 4096> outgoing{};
            size_t bytes = 0;
            bool greeted = false;
            Clock::time_point progress = Clock::now();
            Clock::time_point heartbeat = Clock::now();
            explicit Peer(asio::io_context &io) : socket(io) {}
        };
        std::array<std::unique_ptr<Peer>, 8> peers;
        auto candidate = std::make_unique<Peer>(io);
        status_ = Status::Waiting;
        domain::session::DebugLog("[Spectator] LISTEN tcp=%u", port);
        Record record;
        while (running_.load(std::memory_order_acquire)) {
            // 受渡しの回収にも上限。ネット側が占有してもゲーム側は待たない。
            for (unsigned i = 0; i < 256 && queue_.Pop(record); ++i) archive.Append(record);
            for (auto &peer : peers) if (!peer) {
                acceptor.accept(candidate->socket, error);
                if (!error) {
                    candidate->socket.non_blocking(true);
                    candidate->socket.set_option(Tcp::no_delay(true));
                    candidate->socket.set_option(asio::socket_base::send_buffer_size(8192));
                    candidate->progress = Clock::now();
                    peer = std::move(candidate);
                    candidate = std::make_unique<Peer>(io);
                }
                break; // acceptは1回/tickまで
            }
            uint32_t count = 0;
            for (auto &slot : peers) {
                if (!slot) continue;
                auto &p = *slot;
                if (Clock::now() - p.progress > std::chrono::seconds(10)) { slot.reset(); continue; }
                if (p.received != Hello.size() * 4) {
                    const auto n = p.socket.read_some(asio::buffer(p.hello.data() + p.received, 16 - p.received), error);
                    if (error && !Again(error)) { slot.reset(); continue; }
                    p.received += n;
                    if (p.received != 16) continue;
                    if (std::memcmp(p.hello.data(), Hello.data(), 16)) { slot.reset(); continue; }
                }
                ++count;
                uint8_t unexpected;
                const auto extra = p.socket.receive(asio::buffer(&unexpected, 1), Tcp::socket::message_peek, error);
                if (extra || (error && !Again(error))) { slot.reset(); continue; }
                if (!p.bytes) {
                    if (!p.greeted) {
                        std::memcpy(p.outgoing.data(), Hello.data(), 16); p.bytes = 16; p.greeted = true;
                    } else {
                        if (!p.cursor) p.cursor = archive.Join();
                        if (p.cursor && p.cursor < archive.head && !archive.Get(p.cursor)) { slot.reset(); continue; }
                        // 1回4096B、約256KB/s/人を上限に再送はTCPへ任せる。
                        while (const auto *r = archive.Get(p.cursor)) {
                            if (p.bytes + r->size > p.outgoing.size()) break;
                            std::memcpy(p.outgoing.data() + p.bytes, r, r->size);
                            p.bytes += r->size; ++p.cursor;
                        }
                        if (!p.bytes) {
                            if (Clock::now() - p.heartbeat < std::chrono::seconds(1)) { p.progress = Clock::now(); continue; }
                            Record heartbeat; heartbeat.Set(Heartbeat, 0, uint32_t(0));
                            std::memcpy(p.outgoing.data(), &heartbeat, heartbeat.size); p.bytes = heartbeat.size;
                            p.heartbeat = Clock::now();
                        }
                    }
                }
                const auto n = p.socket.write_some(asio::buffer(p.outgoing.data() + p.offset, p.bytes - p.offset), error);
                if (error && !Again(error)) { slot.reset(); continue; }
                if (n) { p.progress = Clock::now(); p.offset += n; }
                if (p.offset == p.bytes) p.offset = p.bytes = 0;
            }
            viewers_ = count;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    } catch (const std::exception &e) {
        domain::session::DebugLog("[Spectator] HOST DISABLED reason=%s", e.what());
        if (status_ != Status::Overflow) status_ = Status::Failed;
    }
    running_ = false; viewers_ = 0;
}
void Transport::Receive(std::string ip, uint16_t port) {
    WorkerPriority();
    try {
        asio::io_context io;
        Tcp::socket socket(io);
        asio::error_code error;
        const auto endpoint = Tcp::endpoint(asio::ip::make_address(ip), port);
        socket.open(endpoint.protocol()); socket.non_blocking(true);
        bool connected = false;
        socket.async_connect(endpoint, [&](const asio::error_code &e) { error = e; connected = true; });
        const auto begin = Clock::now();
        while (running_ && !connected && Clock::now() - begin < std::chrono::seconds(10)) {
            io.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        if (!connected || error) throw std::runtime_error("connect");
        socket.set_option(Tcp::no_delay(true));
        size_t sent = 0, have = 0, want = 16;
        bool greeted = false, header = false, pending = false;
        Record record;
        auto last = Clock::now();
        while (running_) {
            if (sent < 16) {
                sent += socket.write_some(asio::buffer(reinterpret_cast<const uint8_t*>(Hello.data()) + sent, 16 - sent), error);
                if (error && !Again(error)) break;
            }
            if (pending) {
                if (queue_.Push(record)) { pending = false; have = 0; want = 12; header = false; }
                else { std::this_thread::sleep_for(std::chrono::milliseconds(4)); continue; }
            }
            const auto n = socket.read_some(asio::buffer(reinterpret_cast<uint8_t*>(&record) + have, want - have), error);
            if (error && !Again(error)) break;
            have += n;
            if (n) last = Clock::now();
            if (have == want) {
                if (!greeted) {
                    if (std::memcmp(&record, Hello.data(), 16)) throw std::runtime_error("version");
                    greeted = true; have = 0; want = 12; status_ = Status::Waiting;
                } else if (!header) {
                    if (!Record::ExpectedSize(record.kind) || record.size != Record::ExpectedSize(record.kind))
                        throw std::runtime_error("header");
                    header = true; want = record.size;
                } else {
                    if (!record.Valid()) throw std::runtime_error("record");
                    if (record.kind == Heartbeat) { have = 0; want = 12; header = false; }
                    else {
                        pending = true; status_ = Status::Receiving;
                        if (record.kind == Input) latest_ = record.frame;
                    }
                }
            }
            if (!n) std::this_thread::sleep_for(std::chrono::milliseconds(4));
            // メニューで入力記録が止まる間も接続は維持。OSの切断はreadで検出。
            if (Clock::now() - last > std::chrono::seconds(30)) break;
        }
        if (running_) status_ = Status::Disconnected;
    } catch (const std::exception &e) {
        domain::session::DebugLog("[Spectator] RECEIVE FAILED reason=%s", e.what());
        status_ = Status::Failed;
    }
    running_ = false;
}
}
