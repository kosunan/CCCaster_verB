#include "tests/test_support.hpp"
#include "core_dll/network/UdpSocket.hpp"
#include "core_dll/network/NetworkSimulator.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <algorithm>

using cccaster::network::UdpSocket;
int main() {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::vector<uint8_t>> received;
    std::vector<uint16_t> ports;
    // callbacksの参照先より先にソケットを破棄・joinする。
    UdpSocket receiver(0), sender(0);
    CC_CHECK(receiver.IsValid() && sender.IsValid());
    receiver.OnReceive([&](const auto &data, const auto &, uint16_t port) {
        std::lock_guard lock(mutex);
        received.push_back(data);
        ports.push_back(port);
        changed.notify_one();
    });
    auto waitFor = [&](size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(2), [&] { return received.size() >= count; });
    };
    CC_CASE("未設定・不正宛先を拒否する");
    CC_CHECK(!sender.SendPeer({1}));
    CC_CHECK(!sender.ConfigurePeer("bad address", receiver.GetPort()));
    CC_CHECK(!sender.ConfigurePeer("127.0.0.1", 0));
    CC_CHECK(sender.ConfigurePeer("127.0.0.1", receiver.GetPort()));
    CC_CASE("直接送信直後のバッファ上書きでも連続UDPの内容・順序・送信元を保つ");
    std::vector<uint8_t> buffer(384);
    for (unsigned i = 0; i < 32; ++i) {
        buffer.assign(64 + i * 3, static_cast<uint8_t>(i));
        CC_CHECK(sender.SendPeer(buffer));
        std::fill(buffer.begin(), buffer.end(), uint8_t{0xff});
    }
    CC_CHECK(waitFor(32));
    {
        std::lock_guard lock(mutex);
        CC_CHECK_EQ(received.size(), 32u);
        for (unsigned i = 0; i < received.size(); ++i) {
            CC_CHECK(received[i] == std::vector<uint8_t>(64 + i * 3, static_cast<uint8_t>(i)));
            CC_CHECK_EQ(ports[i], sender.GetPort());
        }
    }
    CC_CASE("UDP上限超過は待たずに失敗し、後続の正常送信は可能");
    CC_CHECK(!sender.SendPeer(std::vector<uint8_t>(65536)));
    CC_CHECK(sender.SendPeer({42}));
    CC_CHECK(waitFor(33));
    CC_CASE("従来ASIO経路もコピー寿命と送信元を維持");
    buffer = {43, 44};
    sender.Send("127.0.0.1", receiver.GetPort(), buffer);
    buffer.assign(512, 0xff);
    CC_CHECK(waitFor(34));
    {
        std::lock_guard lock(mutex);
        CC_CHECK(received.size() == 34 && received.back() == std::vector<uint8_t>({43, 44}));
    }
    CC_CASE("SendPeerでも送信側の損失注入を迂回しない");
    auto &sim = cccaster::network::NetworkSimulator::Instance();
    sim.Enable(0, 0, 100);
    const auto before = sim.DroppedCount();
    CC_CHECK(sender.SendPeer({45}));
    CC_CHECK_EQ(sim.DroppedCount(), before + 1);
    CC_CASE("送信側遅延はコピーを保持して非同期で完了する");
    sim.Enable(10, 10, 0);
    buffer = {46, 47};
    CC_CHECK(sender.SendPeer(buffer));
    buffer.assign(512, 0xff);
    CC_CHECK(waitFor(35));
    {
        std::lock_guard lock(mutex);
        CC_CHECK(received.size() == 35 && received.back() == std::vector<uint8_t>({46, 47}));
    }
    CC_CHECK(sim.DelayedCount() >= 1);
    return cccaster::test::Summarize("udp_send");
}
