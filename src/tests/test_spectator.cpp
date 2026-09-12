#include "core_dll/spectator/Transport.hpp"
#include "cli_launcher/network_wrapper/SpectatorEndpoint.hpp"
#include <chrono>
#include <iostream>
#include <cstdarg>
#include <cstdlib>
#include <algorithm>
#include <vector>
void HookLog(const char *) {}
using namespace cccaster::spectator;
static void Check(bool ok) { if (!ok) { std::cerr << "観戦検査失敗\n"; std::abort(); } }
template<class F> bool Until(F f) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    while (std::chrono::steady_clock::now() < end);
    return false;
}
int main() {
    Record a, b;
    a.Set(Input, 65537, std::array<uint32_t, 2>{0, 0x90010}); Check(a.Valid());
    a.size = 19; Check(!a.Valid()); a.size = 20;
    a.payload[1] = 0x100000; Check(!a.Valid()); a.payload[1] = 0;
    auto q = std::make_unique<Queue<4>>();
    Check(!q->Pop(b));
    Check(q->PushInput(77, 12, 34)); Check(q->Pop(b));
    Check(b.size == 20 && b.kind == Input && b.frame == 77 && b.payload[0] == 12 && b.payload[1] == 34);
    for (int i = 0; i < 4; ++i) { a.frame = 65537+i; Check(q->Push(a)); }
    Check(!q->Push(a));
    for (int i = 0; i < 4; ++i) { Check(q->Pop(b)); Check(b.frame == 65537u+i); }
    Check(!q->Pop(b));
    // スロット周回/同時読書きで欠落・混入を検査。値が先に見えることを確認。
    auto concurrent = std::make_unique<Queue<128>>();
    std::thread producer([&] {
        Record r;
        for (uint32_t i = 1; i <= 200000; ++i) {
            r.Set(Input, i, std::array<uint32_t, 2>{i, ~i});
            while (!concurrent->Push(r)) std::this_thread::yield();
        }
    });
    for (uint32_t i = 1; i <= 200000; ++i) {
        while (!concurrent->Pop(b)) std::this_thread::yield();
        Check(b.frame == i && b.payload[0] == i && b.payload[1] == ~i);
    }
    producer.join();
    StartData start;
    start.p1.epoch = start.p2.epoch = 65536;
    start.p1.revision = start.p2.revision = 1;
    start.p1.confirmed = start.p2.confirmed = 1;
    start.p1.stageConfirmed = 1; start.p1.stage = 1;
    a.Set(Start, 131073, start); Check(a.Valid());
    auto invalid = start; invalid.damageLevel = 5; b.Set(Start, 131073, invalid); Check(!b.Valid());
    b.Set(Selection, 65536, start); Check(b.Valid());
    auto archive = std::make_unique<Archive<4>>();
    archive->Append(a); Check(archive->Join() == 1);
    for (int i = 0; i < 4; ++i) { b.Set(Input, 131073+i, std::array<uint32_t, 2>{}); archive->Append(b); }
    Check(!archive->Join()); Check(!archive->Get(1)); Check(archive->Get(2));
    archive->Append(a); Check(archive->Join() == 6);
    // 実TCP: 試合前接続・heartbeat・後から参加・二重配信・切断。
    auto host = std::make_unique<Transport>();
    host->StartHost(0); Check(Until([&] { return host->State() == Status::Waiting; }));
    const uint16_t port = host->Port();
    Check(!cccaster::main_app::network_wrapper::FindSpectatorEndpoint({"invalid", "", "127.0.0.1"}, port,
        [] { return false; }).empty());
    auto first = std::make_unique<Transport>(); first->StartViewer("127.0.0.1", port);
    Check(Until([&] { return first->State() == Status::Waiting; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    Check(first->Active()); Check(host->Publish(a));
    for (uint32_t i = 0; i < 200; ++i) { b.Set(Input, 131073+i, std::array<uint32_t, 2>{i, 0}); Check(host->Publish(b)); }
    auto late = std::make_unique<Transport>(); late->StartViewer("127.0.0.1", port);
    for (auto *viewer : {first.get(), late.get()}) {
        Check(Until([&] { return viewer->Take(b); })); Check(b.kind == Start);
        for (uint32_t i = 0; i < 200; ++i) {
            Check(Until([&] { return viewer->Take(b); })); Check(b.kind == Input && b.frame == 131073+i && b.payload[0] == i);
        }
    }
    first->Stop(); Check(Until([&] { return host->Viewers() == 1; }));
    host->Stop(); Check(Until([&] { return late->State() == Status::Disconnected; })); late->Stop();
    // ホットパスの単独往復。各測定の時計呼出コストも含む。
    std::vector<int64_t> samples; samples.reserve(20000);
    a.Set(Input, 65537, std::array<uint32_t, 2>{0, 0});
    for (unsigned i = 0; i < 20000; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        Check(q->PushInput(65537, 0, 0)); Check(q->Pop(b));
        samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "観戦キュー/TCP検査成功: roundtrip ns median=" << samples[10000] << " p99=" << samples[19800] << " max=" << samples.back() << '\n';
}
