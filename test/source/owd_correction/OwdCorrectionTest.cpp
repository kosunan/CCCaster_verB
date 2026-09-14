/**
 * OWD補正シミュレーションテスト
 * 
 * 目的: 相対WASAPIクロック補正方式の「動的最速補正速度」を検証する。
 * 
 * テスト方法:
 *   2者(Player1, Player2)のネットワーク遅延をシミュレートし、
 *   共通遅延カット + μs精度の動的補正が振動せず最速で収束するかを全組み合わせで検証。
 * 
 * 遅延パターン:
 *   A: 5~10ms   B: 5~20ms   C: 5~30ms   D: 5~40ms
 * 
 * 検証する組み合わせ (10パターン):
 *   A-A, A-B, A-C, A-D, B-B, B-C, B-D, C-C, C-D, D-D
 */

#include <iostream>
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <string>
#include <cstdint>

// ================================================================
// 遅延パターン定義
// ================================================================
struct DelayProfile {
    std::string name;
    int64_t minDelayUs; // μs
    int64_t maxDelayUs; // μs
};

static const DelayProfile PROFILES[] = {
    {"A(5-10ms)",   5000,  10000},
    {"B(5-20ms)",   5000,  20000},
    {"C(5-30ms)",   5000,  30000},
    {"D(5-40ms)",   5000,  40000},
};
static constexpr int NUM_PROFILES = 4;

// ================================================================
// 補正アルゴリズム（テスト対象）
// ================================================================
class RelativeCorrectionEngine {
public:
    static constexpr int64_t FRAME_DURATION_US = 16666; // 60FPS
    static constexpr int MIN_SLEW_FRAMES = 3;           // 最低補正フレーム数
    static constexpr double SAFETY_MARGIN = 1.5;        // σに対する安全マージン
    static constexpr int64_t DEADZONE_US = 2000;        // 2ms未満は補正しない
    static constexpr int OWD_HISTORY_SIZE = 30;         // σ計算用サンプル数

    void Reset() {
        _correctionRemainingUs = 0;
        _correctionPerFrameUs = 0;
        _slewFramesRemaining = 0;
        _owdHistory.clear();
        _frameDurationUs = FRAME_DURATION_US;
        _totalCorrectedUs = 0;
        _isInCorrection = false;
    }

    // OWDサンプル追加（σ計算に使用）
    void AddOwdSample(int64_t owdUs) {
        _owdHistory.push_back(owdUs);
        if ((int)_owdHistory.size() > OWD_HISTORY_SIZE) {
            _owdHistory.erase(_owdHistory.begin());
        }
    }

    // OWDジッター（標準偏差）を計算
    double GetOwdSigmaUs() const {
        if (_owdHistory.size() < 2) return FRAME_DURATION_US; // データ不足時は保守的

        double mean = 0;
        for (auto v : _owdHistory) mean += v;
        mean /= _owdHistory.size();

        double variance = 0;
        for (auto v : _owdHistory) {
            double diff = v - mean;
            variance += diff * diff;
        }
        variance /= (_owdHistory.size() - 1);
        return std::sqrt(variance);
    }

    // 補正を開始: correctionUs = 共通カット済みの自分の補正量
    void StartCorrection(int64_t correctionUs) {
        if (std::abs(correctionUs) < DEADZONE_US) {
            // デッドゾーン内 → 補正不要
            _isInCorrection = false;
            return;
        }

        double sigma = GetOwdSigmaUs();
        double safeSigma = sigma * SAFETY_MARGIN;

        // 振動しない最速補正: 1Fあたり safeSigma μs まで
        // slewFrames = correction / safeSigma
        int slewFrames = static_cast<int>(std::ceil(std::abs(correctionUs) / safeSigma));
        slewFrames = std::max(slewFrames, MIN_SLEW_FRAMES);

        _correctionRemainingUs = correctionUs;
        _correctionPerFrameUs = correctionUs / slewFrames;
        _slewFramesRemaining = slewFrames;
        _isInCorrection = true;
    }

    // 毎フレーム呼ぶ: 1Fの実効周期を返す
    int64_t TickFrame() {
        if (!_isInCorrection || _slewFramesRemaining <= 0) {
            _isInCorrection = false;
            _frameDurationUs = FRAME_DURATION_US;
            return _frameDurationUs;
        }

        // 残りフレーム数に応じた1Fあたりの補正量
        _frameDurationUs = FRAME_DURATION_US - _correctionPerFrameUs;

        // フレーム周期が極端にならないようクランプ (50% ~ 150%)
        _frameDurationUs = std::max(_frameDurationUs, FRAME_DURATION_US / 2);
        _frameDurationUs = std::min(_frameDurationUs, FRAME_DURATION_US * 3 / 2);

        _totalCorrectedUs += _correctionPerFrameUs;
        _correctionRemainingUs -= _correctionPerFrameUs;
        _slewFramesRemaining--;

        if (_slewFramesRemaining <= 0) {
            _isInCorrection = false;
        }

        return _frameDurationUs;
    }

    bool IsInCorrection() const { return _isInCorrection; }
    int64_t GetTotalCorrected() const { return _totalCorrectedUs; }
    int64_t GetFrameDuration() const { return _frameDurationUs; }
    int GetSlewFramesRemaining() const { return _slewFramesRemaining; }

private:
    int64_t _correctionRemainingUs = 0;
    int64_t _correctionPerFrameUs = 0;
    int _slewFramesRemaining = 0;
    int64_t _frameDurationUs = FRAME_DURATION_US;
    int64_t _totalCorrectedUs = 0;
    bool _isInCorrection = false;
    std::vector<int64_t> _owdHistory;
};

// ================================================================
// ネットワークシミュレーター
// ================================================================
class NetworkSimulator {
public:
    NetworkSimulator(uint32_t seed = 42) : _gen(seed) {}

    // 指定プロファイルの遅延を生成
    int64_t GenerateDelay(const DelayProfile& profile) {
        std::uniform_int_distribution<int64_t> dist(profile.minDelayUs, profile.maxDelayUs);
        return dist(_gen);
    }

private:
    std::mt19937_64 _gen;
};

// ================================================================
// 2者シミュレーション
// ================================================================
struct SimulationResult {
    std::string p1Profile;
    std::string p2Profile;

    // 補正の結果
    int64_t p1CorrectionUs;      // P1の補正量（共通カット後）
    int64_t p2CorrectionUs;      // P2の補正量（共通カット後）
    int p1SlewFrames;            // P1の補正に要したフレーム数
    int p2SlewFrames;            // P2の補正に要したフレーム数
    double p1SlewTimeMs;         // P1の補正に要した時間
    double p2SlewTimeMs;         // P2の補正に要した時間
    double p1OwdSigmaMs;         // P1のOWDジッター
    double p2OwdSigmaMs;         // P2のOWDジッター
    int64_t commonCutUs;         // 共通カット量
    bool p1Oscillated;           // P1で振動が発生したか
    bool p2Oscillated;           // P2で振動が発生したか
    double p1MaxSpeedRatio;      // P1の最大速度変化率(%)
    double p2MaxSpeedRatio;      // P2の最大速度変化率(%)
};

SimulationResult RunPairSimulation(
    const DelayProfile& profile1,
    const DelayProfile& profile2,
    int numOwdSamples = 30,
    uint32_t seed = 12345)
{
    SimulationResult result;
    result.p1Profile = profile1.name;
    result.p2Profile = profile2.name;
    result.p1Oscillated = false;
    result.p2Oscillated = false;

    NetworkSimulator net(seed);
    RelativeCorrectionEngine engine1, engine2;
    engine1.Reset();
    engine2.Reset();

    // Phase 1: OWDサンプル収集 (NTP風プロトコルのシミュレーション)
    for (int i = 0; i < numOwdSamples; i++) {
        int64_t owd1to2 = net.GenerateDelay(profile1);
        int64_t owd2to1 = net.GenerateDelay(profile2);
        engine1.AddOwdSample(owd1to2);
        engine2.AddOwdSample(owd2to1);
    }

    result.p1OwdSigmaMs = engine1.GetOwdSigmaUs() / 1000.0;
    result.p2OwdSigmaMs = engine2.GetOwdSigmaUs() / 1000.0;

    // Phase 2: 遅延シナリオ生成
    // 「ある瞬間のP1とP2の遅延」をシミュレート
    // 各プレイヤーのWASAPIクロック遅延を模擬
    int64_t p1DelayUs = net.GenerateDelay(profile1);
    int64_t p2DelayUs = net.GenerateDelay(profile2);

    // 共通遅延カット
    int64_t commonDelay = std::min(p1DelayUs, p2DelayUs);
    result.commonCutUs = commonDelay;

    result.p1CorrectionUs = p1DelayUs - commonDelay;
    result.p2CorrectionUs = p2DelayUs - commonDelay;

    // Phase 3: 補正実行シミュレーション
    engine1.StartCorrection(result.p1CorrectionUs);
    engine2.StartCorrection(result.p2CorrectionUs);

    // P1の補正シミュレーション
    int p1Frames = 0;
    int64_t prevDuration1 = RelativeCorrectionEngine::FRAME_DURATION_US;
    int directionChanges1 = 0;
    result.p1MaxSpeedRatio = 100.0;

    while (engine1.IsInCorrection()) {
        int64_t duration = engine1.TickFrame();
        double ratio = (double)duration / RelativeCorrectionEngine::FRAME_DURATION_US * 100.0;
        if (std::abs(ratio - 100.0) > std::abs(result.p1MaxSpeedRatio - 100.0)) {
            result.p1MaxSpeedRatio = ratio;
        }
        // 振動検出: フレーム周期の増減方向が反転
        if (p1Frames > 0) {
            int64_t prevDiff = prevDuration1 - RelativeCorrectionEngine::FRAME_DURATION_US;
            int64_t currDiff = duration - RelativeCorrectionEngine::FRAME_DURATION_US;
            if (prevDiff != 0 && currDiff != 0 && ((prevDiff > 0) != (currDiff > 0))) {
                directionChanges1++;
            }
        }
        prevDuration1 = duration;
        p1Frames++;
        if (p1Frames > 600) break; // 10秒上限
    }
    result.p1SlewFrames = p1Frames;
    result.p1SlewTimeMs = p1Frames * RelativeCorrectionEngine::FRAME_DURATION_US / 1000.0;
    result.p1Oscillated = (directionChanges1 > 2);

    // P2の補正シミュレーション
    int p2Frames = 0;
    int64_t prevDuration2 = RelativeCorrectionEngine::FRAME_DURATION_US;
    int directionChanges2 = 0;
    result.p2MaxSpeedRatio = 100.0;

    while (engine2.IsInCorrection()) {
        int64_t duration = engine2.TickFrame();
        double ratio = (double)duration / RelativeCorrectionEngine::FRAME_DURATION_US * 100.0;
        if (std::abs(ratio - 100.0) > std::abs(result.p2MaxSpeedRatio - 100.0)) {
            result.p2MaxSpeedRatio = ratio;
        }
        if (p2Frames > 0) {
            int64_t prevDiff = prevDuration2 - RelativeCorrectionEngine::FRAME_DURATION_US;
            int64_t currDiff = duration - RelativeCorrectionEngine::FRAME_DURATION_US;
            if (prevDiff != 0 && currDiff != 0 && ((prevDiff > 0) != (currDiff > 0))) {
                directionChanges2++;
            }
        }
        prevDuration2 = duration;
        p2Frames++;
        if (p2Frames > 600) break;
    }
    result.p2SlewFrames = p2Frames;
    result.p2SlewTimeMs = p2Frames * RelativeCorrectionEngine::FRAME_DURATION_US / 1000.0;
    result.p2Oscillated = (directionChanges2 > 2);

    return result;
}

// ================================================================
// 統計的多回検証（同じ組み合わせをN回走らせて安定性を確認）
// ================================================================
struct AggregatedResult {
    std::string p1Profile;
    std::string p2Profile;
    int numTrials;

    double avgCorrectionMs;     // 平均補正量(ms) (遅れている側)
    double maxCorrectionMs;     // 最大補正量(ms)
    double avgSlewTimeMs;       // 平均補正時間(ms)
    double maxSlewTimeMs;       // 最大補正時間(ms)
    int oscillationCount;       // 振動が発生した回数
    double avgMaxSpeedChange;   // 平均の最大速度変化(%)
    double worstMaxSpeedChange; // 最悪の速度変化(%)
    double avgCommonCutMs;      // 平均共通カット量(ms)
};

AggregatedResult RunAggregatedTest(
    const DelayProfile& profile1,
    const DelayProfile& profile2,
    int numTrials = 100)
{
    AggregatedResult agg;
    agg.p1Profile = profile1.name;
    agg.p2Profile = profile2.name;
    agg.numTrials = numTrials;
    agg.avgCorrectionMs = 0;
    agg.maxCorrectionMs = 0;
    agg.avgSlewTimeMs = 0;
    agg.maxSlewTimeMs = 0;
    agg.oscillationCount = 0;
    agg.avgMaxSpeedChange = 0;
    agg.worstMaxSpeedChange = 100.0;
    agg.avgCommonCutMs = 0;

    for (int trial = 0; trial < numTrials; trial++) {
        auto r = RunPairSimulation(profile1, profile2, 30, 10000 + trial * 7);

        // 遅れている側（補正量が大きい方）の統計
        int64_t maxCorr = std::max(r.p1CorrectionUs, r.p2CorrectionUs);
        double corrMs = maxCorr / 1000.0;
        double slewMs = (r.p1CorrectionUs >= r.p2CorrectionUs) ? r.p1SlewTimeMs : r.p2SlewTimeMs;
        double speedChange = (r.p1CorrectionUs >= r.p2CorrectionUs) ? r.p1MaxSpeedRatio : r.p2MaxSpeedRatio;
        bool osc = r.p1Oscillated || r.p2Oscillated;

        agg.avgCorrectionMs += corrMs;
        if (corrMs > agg.maxCorrectionMs) agg.maxCorrectionMs = corrMs;
        agg.avgSlewTimeMs += slewMs;
        if (slewMs > agg.maxSlewTimeMs) agg.maxSlewTimeMs = slewMs;
        if (osc) agg.oscillationCount++;
        agg.avgMaxSpeedChange += std::abs(speedChange - 100.0);
        if (std::abs(speedChange - 100.0) > std::abs(agg.worstMaxSpeedChange - 100.0)) {
            agg.worstMaxSpeedChange = speedChange;
        }
        agg.avgCommonCutMs += r.commonCutUs / 1000.0;
    }

    agg.avgCorrectionMs /= numTrials;
    agg.avgSlewTimeMs /= numTrials;
    agg.avgMaxSpeedChange /= numTrials;
    agg.avgCommonCutMs /= numTrials;

    return agg;
}

// ================================================================
// メイン
// ================================================================
int main() {
    std::cout << "===================================================================\n";
    std::cout << " OWD補正シミュレーションテスト — 動的最速補正速度の検証\n";
    std::cout << " 遅延パターン: A(5-10ms) B(5-20ms) C(5-30ms) D(5-40ms)\n";
    std::cout << " 全10組み合わせ x 100試行 = 1000シミュレーション\n";
    std::cout << "===================================================================\n\n";

    // 全10組み合わせ
    std::vector<AggregatedResult> results;
    for (int i = 0; i < NUM_PROFILES; i++) {
        for (int j = i; j < NUM_PROFILES; j++) {
            auto agg = RunAggregatedTest(PROFILES[i], PROFILES[j], 100);
            results.push_back(agg);
        }
    }

    // --- 結果テーブル ---
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "+---------------+---------------+--------+--------+--------+--------+------+--------+--------+\n";
    std::cout << "| P1            | P2            | Avg補正| Max補正| Avg時間| Max時間| 振動 | Avg速度| Worst  |\n";
    std::cout << "|               |               | (ms)   | (ms)   | (ms)   | (ms)   | 回数 | 変化%  | 速度%  |\n";
    std::cout << "+---------------+---------------+--------+--------+--------+--------+------+--------+--------+\n";

    int totalOscillations = 0;
    int totalTests = 0;
    bool allPassed = true;

    for (auto& r : results) {
        std::cout << "| " << std::left << std::setw(13) << r.p1Profile
                  << " | " << std::setw(13) << r.p2Profile
                  << " | " << std::right << std::setw(6) << r.avgCorrectionMs
                  << " | " << std::setw(6) << r.maxCorrectionMs
                  << " | " << std::setw(6) << r.avgSlewTimeMs
                  << " | " << std::setw(6) << r.maxSlewTimeMs
                  << " | " << std::setw(4) << r.oscillationCount
                  << " | " << std::setw(6) << r.avgMaxSpeedChange
                  << " | " << std::setw(6) << r.worstMaxSpeedChange
                  << " |\n";

        totalOscillations += r.oscillationCount;
        totalTests += r.numTrials;

        // 判定基準
        if (r.oscillationCount > 0) allPassed = false;
    }

    std::cout << "+---------------+---------------+--------+--------+--------+--------+------+--------+--------+\n\n";

    // --- 共通カット効果の確認 ---
    std::cout << "--- 共通遅延カット効果 ---\n";
    std::cout << "+---------------+---------------+----------+----------+\n";
    std::cout << "| P1            | P2            | Avg共通   | Avgカット |\n";
    std::cout << "|               |               | カット(ms)| 率(%)    |\n";
    std::cout << "+---------------+---------------+----------+----------+\n";

    for (auto& r : results) {
        double totalAvgDelay = r.avgCorrectionMs + r.avgCommonCutMs;
        double cutRatio = (totalAvgDelay > 0) ? (r.avgCommonCutMs / totalAvgDelay * 100.0) : 0;
        std::cout << "| " << std::left << std::setw(13) << r.p1Profile
                  << " | " << std::setw(13) << r.p2Profile
                  << " | " << std::right << std::setw(8) << r.avgCommonCutMs
                  << " | " << std::setw(8) << cutRatio
                  << " |\n";
    }
    std::cout << "+---------------+---------------+----------+----------+\n\n";

    // --- 詳細: 1つの代表的なケースのフレームごとの推移 ---
    std::cout << "--- 詳細ログ: C-D (5-30ms vs 5-40ms) 代表ケース ---\n";
    auto detailResult = RunPairSimulation(PROFILES[2], PROFILES[3], 30, 99999);
    std::cout << "P1 遅延: " << detailResult.p1Profile 
              << ", P2 遅延: " << detailResult.p2Profile << "\n";
    std::cout << "共通カット: " << detailResult.commonCutUs / 1000.0 << " ms\n";
    std::cout << "P1 補正量: " << detailResult.p1CorrectionUs / 1000.0 << " ms"
              << " → " << detailResult.p1SlewFrames << " F"
              << " (" << detailResult.p1SlewTimeMs << " ms)\n";
    std::cout << "P2 補正量: " << detailResult.p2CorrectionUs / 1000.0 << " ms"
              << " → " << detailResult.p2SlewFrames << " F"
              << " (" << detailResult.p2SlewTimeMs << " ms)\n";
    std::cout << "P1 OWD σ: " << detailResult.p1OwdSigmaMs << " ms\n";
    std::cout << "P2 OWD σ: " << detailResult.p2OwdSigmaMs << " ms\n";
    std::cout << "P1 最大速度: " << detailResult.p1MaxSpeedRatio << "%\n";
    std::cout << "P2 最大速度: " << detailResult.p2MaxSpeedRatio << "%\n";
    std::cout << "P1 振動: " << (detailResult.p1Oscillated ? "YES" : "NO") << "\n";
    std::cout << "P2 振動: " << (detailResult.p2Oscillated ? "YES" : "NO") << "\n";

    // --- 最終判定 ---
    std::cout << "\n===================================================================\n";
    std::cout << " 総合結果\n";
    std::cout << "===================================================================\n";
    std::cout << "総シミュレーション数: " << totalTests << "\n";
    std::cout << "振動発生回数: " << totalOscillations << " / " << totalTests << "\n";

    if (allPassed) {
        std::cout << "[PASS] 全テストで振動なし。動的最速補正が安全に動作。\n";
    } else {
        std::cout << "[FAIL] 一部テストで振動を検出。パラメータ調整が必要。\n";
    }

    return allPassed ? 0 : 1;
}
