#include <iostream>
#include <vector>
#include <random>
#include <numeric>
#include <algorithm>
#include <chrono>

// TimeSynchronizerからOWDとRTTの計算ロジックだけを抽出・模倣した単体テスト

namespace cccaster::sync::test {

struct SyncSample {
    int64_t rtt;
    int64_t offset;
};

// クライアント側が持つ時計のズレ（仮定）
// クライアントの時計はホストより常に +150,000us (150ms) 進んでいるとする
constexpr int64_t TRUE_CLOCK_OFFSET = 150000;

int64_t GetHostTimeUs() {
    static auto start = std::chrono::high_resolution_clock::now();
    auto now = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
}

int64_t GetClientTimeUs() {
    // クライアントは意図的に時計がズレている
    return GetHostTimeUs() + TRUE_CLOCK_OFFSET;
}

// 疑似的なネットワーク遅延ジェネレーター (30ms ~ 60ms)
int64_t GenerateRandomDelayUs() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<int64_t> dist(30000, 60000); // 30ms ~ 60ms
    return dist(gen);
}

void RunOwdSimulation() {
    std::cout << "[Test] Starting OWD and Clock Offset Estimation Simulation...\n";
    std::cout << "[Test] True Clock Offset (Client to Host) is theoretically: " << TRUE_CLOCK_OFFSET << " us\n";

    std::vector<SyncSample> samples;

    // 30回のPing-Pong通信をシミュレート
    for (int i = 0; i < 30; ++i) {
        // [1] クライアントが送信
        int64_t t1 = GetClientTimeUs();
        
        // --- ネットワーク上り遅延 ---
        int64_t delayUp = GenerateRandomDelayUs();

        // [2] ホストが受信
        int64_t t2 = (t1 - TRUE_CLOCK_OFFSET) + delayUp;

        // ホストでの処理遅延 (例えば 1ms ~ 5ms)
        std::uniform_int_distribution<int64_t> hostProcDist(1000, 5000);
        std::random_device rd2;
        std::mt19937 gen2(rd2());
        int64_t hostProcDelay = hostProcDist(gen2);

        // [3] ホストが返信を送信
        int64_t t3 = t2 + hostProcDelay;

        // --- ネットワーク下り遅延 ---
        int64_t delayDown = GenerateRandomDelayUs();

        // [4] クライアントが返信を受信
        int64_t t4 = (t3 + TRUE_CLOCK_OFFSET) + delayDown;

        // --- クライアント側での計算 ---
        int64_t rtt = (t4 - t1) - (t3 - t2);
        int64_t offset = ((t2 - t1) + (t3 - t4)) / 2;

        samples.push_back({rtt, offset});

        std::cout << "  Ping " << i + 1 << ": RTT = " << rtt / 1000.0 << " ms, "
                  << "Calc Offset = " << offset << " us (Diff: " << std::abs(offset - (-TRUE_CLOCK_OFFSET)) << " us)\n";
    }

    // 最小RTTフィルタリング
    auto bestSample = std::min_element(samples.begin(), samples.end(), 
        [](const SyncSample& a, const SyncSample& b) {
            return a.rtt < b.rtt;
        });

    std::cout << "\n[Test] --- Simulation Results ---\n";
    std::cout << "[Test] Best Sample (Lowest RTT): " << bestSample->rtt / 1000.0 << " ms\n";
    std::cout << "[Test] Estimated Clock Offset: " << bestSample->offset << " us\n";
    
    // 計算されたオフセットと真のオフセットの誤差
    // クライアントの式では T2(ホスト) - T1(クライアント) となっているので
    // offsetの正負は "クライアント時間 + offset = ホスト時間" となる
    // 真のオフセットは "クライアント時間 = ホスト時間 + 150000" つまり "クライアント時間 - 150000 = ホスト時間"
    // よって期待される calc_offset は -150000 付近。
    int64_t expected_offset = -TRUE_CLOCK_OFFSET;
    int64_t error = std::abs(bestSample->offset - expected_offset);
    
    std::cout << "[Test] Estimation Error: " << error << " us (" << error / 1000.0 << " ms)\n";
    
    if (error < 15000) { // 極端な非対称遅延ガチャでない限り15ms以内の誤差に収まるのが期待値
        std::cout << "[Test] SUCCESS: Clock offset successfully estimated within acceptable error bounds.\n";
    } else {
        std::cout << "[Test] WARNING: Clock offset estimation error is higher than expected due to extreme simulated jitter.\n";
    }

    // --- 擬似同期された時計を用いたOWD計算テスト ---
    std::cout << "\n[Test] Testing OWD Calculation with estimated offset...\n";
    
    // クライアントからホストへのパケット送信
    int64_t t_send_A = GetClientTimeUs();
    int64_t trueOwdUp = GenerateRandomDelayUs();
    int64_t t_recv_B = (t_send_A - TRUE_CLOCK_OFFSET) + trueOwdUp;
    
    // Calculate OWD (A -> B)
    int64_t estimated_offset = bestSample->offset;
    int64_t calcOwdUp = (t_recv_B - estimated_offset) - t_send_A;
    
    std::cout << "  Simulated True OWD (A->B): " << trueOwdUp / 1000.0 << " ms\n";
    std::cout << "  Calculated OWD (A->B):     " << calcOwdUp / 1000.0 << " ms\n";
    std::cout << "  OWD Estimation Error:      " << std::abs(calcOwdUp - trueOwdUp) / 1000.0 << " ms\n";
}

} // namespace cccaster::sync::test

int main() {
    cccaster::sync::test::RunOwdSimulation();
    return 0;
}
