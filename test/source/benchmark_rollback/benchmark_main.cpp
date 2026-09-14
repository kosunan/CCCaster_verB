// ============================================================================
// Rollback Engine Performance Benchmark
// ============================================================================
// 旧実装 (legacy) vs 最適化版 (optimized) の性能比較
//
// テスト対象:
//   1. SaveState / LoadState (メモリダンプの保存・復元)
//   2. SFX OR マージ (ロールバック時のSFXフィルタ初期化)
//   3. SFX 0x80 フラグ設定
//   4. SaveRerunSounds (再計算中のSFX履歴更新)
//   5. FinishedRerunSounds (ロールバック完了後のSFX処理)
//
// ビルド: cmake --build build
// 実行:   build/RollbackBenchmark.exe
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>

// 旧実装
#include "legacy/MemDumper.hpp"
#include "legacy/SfxFilter.hpp"

// 最適化版
#include "optimized/MemDumper.hpp"
#include "optimized/SfxFilter.hpp"

// ============================================================================
// ユーティリティ
// ============================================================================

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double, std::micro>; // マイクロ秒

struct BenchResult {
    double minUs;
    double maxUs;
    double avgUs;
    double medianUs;
};

BenchResult Analyze(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
    return {
        samples.front(),
        samples.back(),
        sum / static_cast<double>(samples.size()),
        samples[samples.size() / 2]
    };
}

void PrintResult(const char* name, const char* variant, BenchResult r) {
    printf("  %-12s | %8.2f us | %8.2f us | %8.2f us | %8.2f us\n",
           variant, r.minUs, r.medianUs, r.avgUs, r.maxUs);
}

void PrintHeader(const char* testName) {
    printf("\n=== %s ===\n", testName);
    printf("  %-12s | %11s | %11s | %11s | %11s\n",
           "Variant", "Min", "Median", "Avg", "Max");
    printf("  -------------|-------------|-------------|-------------|------------\n");
}

void PrintSpeedup(BenchResult legacy, BenchResult optimized) {
    printf("  >> Speedup: %.2fx (median), %.2fx (avg)\n",
           legacy.medianUs / optimized.medianUs,
           legacy.avgUs / optimized.avgUs);
}

// ============================================================================
// テスト1: SaveState / LoadState
// ============================================================================

void BenchSaveLoadState(int iterations) {
    PrintHeader("SaveState / LoadState");

    // テスト用に連続メモリ領域をシミュレート
    // Generator.cpp のダンプリスト構造を模倣:
    //   - miscAddrs: ~30エントリ、4B〜数百B
    //   - playerAddrs × 4: 各~70エントリ、4B〜256B
    //   - effects × 1000: 各0x33C(828B)
    // 合計: 約300エントリ、数百KB

    // シミュレーション用の大きなメモリブロック
    static constexpr size_t SIM_MEM_SIZE = 2 * 1024 * 1024; // 2MB
    std::vector<uint8_t> simMemory(SIM_MEM_SIZE);

    // ランダムデータで埋める
    std::mt19937 rng(42);
    for (auto& b : simMemory) b = static_cast<uint8_t>(rng() & 0xFF);

    // Generator.cpp風のダンプエントリを生成
    // 小さなエントリ(4B〜64B)が多数 + 大きなエントリ(828B)が1000個
    std::vector<legacy::DumpEntry> legacyEntries;
    std::vector<optimized::DumpEntry> optEntries;

    uintptr_t base = reinterpret_cast<uintptr_t>(simMemory.data());

    // misc: 30個の小エントリ（4〜128B、間にGapあり）
    size_t offset = 0;
    for (int i = 0; i < 30; ++i) {
        size_t sz = 4 + (rng() % 125);
        size_t gap = rng() % 48; // 0〜47バイトのGap
        legacyEntries.push_back({base + offset, sz});
        optEntries.push_back({base + offset, sz});
        offset += sz + gap;
    }

    // players × 4: 各 70 エントリ
    for (int p = 0; p < 4; ++p) {
        size_t playerBase = offset;
        for (int i = 0; i < 70; ++i) {
            size_t sz = 4 + (rng() % 252);
            size_t gap = rng() % 32;
            legacyEntries.push_back({base + offset, sz});
            optEntries.push_back({base + offset, sz});
            offset += sz + gap;
        }
    }

    // effects × 200: 各 828B（本番は1000個だが縮小）
    for (int i = 0; i < 200; ++i) {
        size_t sz = 828;
        legacyEntries.push_back({base + offset, sz});
        optEntries.push_back({base + offset, sz});
        offset += sz + 4;
    }

    // セットアップ
    legacy::MemDumper legacyDumper;
    legacyDumper.SetEntries(legacyEntries);

    optimized::MemDumper optDumper;
    optDumper.SetEntries(optEntries);

    printf("  Entries: legacy=%zu, optimized(merged)=%zu, totalSize=legacy:%zuB / opt:%zuB\n",
           legacyDumper.GetEntryCount(), optDumper.GetEntryCount(),
           legacyDumper.GetTotalSize(), optDumper.GetTotalSize());

    // バッファ確保
    std::vector<char> legacyBuf(legacyDumper.GetTotalSize());
    std::vector<char> optBuf(optDumper.GetTotalSize());

    // ウォームアップ
    legacyDumper.SaveState(legacyBuf.data());
    optDumper.SaveState(optBuf.data());

    // --- SaveState ベンチマーク ---
    {
        std::vector<double> legacySamples(iterations);
        std::vector<double> optSamples(iterations);

        for (int i = 0; i < iterations; ++i) {
            auto t0 = Clock::now();
            legacyDumper.SaveState(legacyBuf.data());
            auto t1 = Clock::now();
            legacySamples[i] = Duration(t1 - t0).count();
        }

        for (int i = 0; i < iterations; ++i) {
            auto t0 = Clock::now();
            optDumper.SaveState(optBuf.data());
            auto t1 = Clock::now();
            optSamples[i] = Duration(t1 - t0).count();
        }

        auto lr = Analyze(legacySamples);
        auto or_ = Analyze(optSamples);
        PrintResult("SaveState", "Legacy", lr);
        PrintResult("SaveState", "Optimized", or_);
        PrintSpeedup(lr, or_);
    }

    // --- LoadState ベンチマーク ---
    {
        std::vector<double> legacySamples(iterations);
        std::vector<double> optSamples(iterations);

        for (int i = 0; i < iterations; ++i) {
            auto t0 = Clock::now();
            legacyDumper.LoadState(legacyBuf.data());
            auto t1 = Clock::now();
            legacySamples[i] = Duration(t1 - t0).count();
        }

        for (int i = 0; i < iterations; ++i) {
            auto t0 = Clock::now();
            optDumper.LoadState(optBuf.data());
            auto t1 = Clock::now();
            optSamples[i] = Duration(t1 - t0).count();
        }

        auto lr = Analyze(legacySamples);
        auto or_ = Analyze(optSamples);
        PrintResult("LoadState", "Legacy", lr);
        PrintResult("LoadState", "Optimized", or_);
        PrintSpeedup(lr, or_);
    }
}

// ============================================================================
// テスト2: SFXフィルタ操作
// ============================================================================

void BenchSfxFilter(int iterations) {
    // SFX履歴にランダムデータを事前設定
    std::mt19937 rng(123);

    auto fillRandom = [&](uint8_t* arr, size_t len) {
        for (size_t i = 0; i < len; ++i)
            arr[i] = (rng() % 4 == 0) ? 1 : 0; // 25%の確率で再生
    };

    // --- InitFilterForRollback ---
    {
        PrintHeader("SFX InitFilterForRollback (OR merge + 0x80 flag)");

        constexpr uint32_t ROLLBACK_FRAMES = 8; // 典型的なロールバック深度
        constexpr uint32_t ORIG_FRAME = 1000;
        constexpr uint32_t RB_FRAME = ORIG_FRAME - ROLLBACK_FRAMES;

        std::vector<double> legacySamples(iterations);
        std::vector<double> optSamples(iterations);

        for (int i = 0; i < iterations; ++i) {
            legacy::SfxFilter lf;
            for (uint32_t f = RB_FRAME; f <= ORIG_FRAME; ++f)
                fillRandom(&lf.sfxHistory[f % legacy::NUM_STATES][0], legacy::SFX_ARRAY_LEN);
            memset(lf.filterArray, 0, legacy::SFX_ARRAY_LEN);

            auto t0 = Clock::now();
            lf.InitFilterForRollback(RB_FRAME, ORIG_FRAME);
            auto t1 = Clock::now();
            legacySamples[i] = Duration(t1 - t0).count();
        }

        for (int i = 0; i < iterations; ++i) {
            optimized::SfxFilter of;
            for (uint32_t f = RB_FRAME; f <= ORIG_FRAME; ++f)
                fillRandom(&of.sfxHistory[f % optimized::NUM_STATES][0], optimized::SFX_ARRAY_LEN);
            memset(of.filterArray, 0, optimized::SFX_ALIGNED_LEN);

            auto t0 = Clock::now();
            of.InitFilterForRollback(RB_FRAME, ORIG_FRAME);
            auto t1 = Clock::now();
            optSamples[i] = Duration(t1 - t0).count();
        }

        auto lr = Analyze(legacySamples);
        auto or_ = Analyze(optSamples);
        PrintResult("InitFilter", "Legacy", lr);
        PrintResult("InitFilter", "Optimized", or_);
        PrintSpeedup(lr, or_);
    }

    // --- SaveRerunSounds ---
    {
        PrintHeader("SFX SaveRerunSounds (branchless)");

        std::vector<double> legacySamples(iterations);
        std::vector<double> optSamples(iterations);

        for (int i = 0; i < iterations; ++i) {
            legacy::SfxFilter lf;
            // filterArrayにランダム値を設定（0x80との混合）
            for (size_t j = 0; j < legacy::SFX_ARRAY_LEN; ++j) {
                int r = rng() % 4;
                if (r == 0) lf.filterArray[j] = 0x80;
                else if (r == 1) lf.filterArray[j] = 0x81;
                else lf.filterArray[j] = 0;
            }

            auto t0 = Clock::now();
            lf.SaveRerunSounds(500);
            auto t1 = Clock::now();
            legacySamples[i] = Duration(t1 - t0).count();
        }

        for (int i = 0; i < iterations; ++i) {
            optimized::SfxFilter of;
            for (size_t j = 0; j < optimized::SFX_ARRAY_LEN; ++j) {
                int r = rng() % 4;
                if (r == 0) of.filterArray[j] = 0x80;
                else if (r == 1) of.filterArray[j] = 0x81;
                else of.filterArray[j] = 0;
            }

            auto t0 = Clock::now();
            of.SaveRerunSounds(500);
            auto t1 = Clock::now();
            optSamples[i] = Duration(t1 - t0).count();
        }

        auto lr = Analyze(legacySamples);
        auto or_ = Analyze(optSamples);
        PrintResult("RerunSounds", "Legacy", lr);
        PrintResult("RerunSounds", "Optimized", or_);
        PrintSpeedup(lr, or_);
    }

    // --- FinishedRerunSounds ---
    {
        PrintHeader("SFX FinishedRerunSounds");

        alignas(16) uint8_t gameSfxLegacy[legacy::SFX_ARRAY_LEN];
        alignas(16) uint8_t gameSfxOpt[optimized::SFX_ALIGNED_LEN];

        std::vector<double> legacySamples(iterations);
        std::vector<double> optSamples(iterations);

        for (int i = 0; i < iterations; ++i) {
            legacy::SfxFilter lf;
            memset(gameSfxLegacy, 0, sizeof(gameSfxLegacy));
            for (size_t j = 0; j < legacy::SFX_ARRAY_LEN; ++j)
                lf.filterArray[j] = (rng() % 3 == 0) ? 0x80 : 0;

            auto t0 = Clock::now();
            lf.FinishedRerunSounds(gameSfxLegacy);
            auto t1 = Clock::now();
            legacySamples[i] = Duration(t1 - t0).count();
        }

        for (int i = 0; i < iterations; ++i) {
            optimized::SfxFilter of;
            memset(gameSfxOpt, 0, sizeof(gameSfxOpt));
            for (size_t j = 0; j < optimized::SFX_ARRAY_LEN; ++j)
                of.filterArray[j] = (rng() % 3 == 0) ? 0x80 : 0;

            auto t0 = Clock::now();
            of.FinishedRerunSounds(gameSfxOpt);
            auto t1 = Clock::now();
            optSamples[i] = Duration(t1 - t0).count();
        }

        auto lr = Analyze(legacySamples);
        auto or_ = Analyze(optSamples);
        PrintResult("Finished", "Legacy", lr);
        PrintResult("Finished", "Optimized", or_);
        PrintSpeedup(lr, or_);
    }
}

// ============================================================================
// テスト3: 正当性検証（結果一致確認）
// ============================================================================

bool VerifyCorrectness() {
    printf("\n=== Correctness Verification ===\n");
    bool allPassed = true;

    // SFX InitFilterForRollback の結果一致確認
    {
        std::mt19937 rng(999);
        legacy::SfxFilter lf;
        optimized::SfxFilter of;

        for (uint32_t f = 90; f <= 100; ++f) {
            for (size_t j = 0; j < legacy::SFX_ARRAY_LEN; ++j) {
                uint8_t val = (rng() % 4 == 0) ? 1 : 0;
                lf.sfxHistory[f % legacy::NUM_STATES][j] = val;
                of.sfxHistory[f % optimized::NUM_STATES][j] = val;
            }
        }
        memset(lf.filterArray, 0, legacy::SFX_ARRAY_LEN);
        memset(of.filterArray, 0, optimized::SFX_ALIGNED_LEN);

        lf.InitFilterForRollback(90, 100);
        of.InitFilterForRollback(90, 100);

        bool match = (memcmp(lf.filterArray, of.filterArray, legacy::SFX_ARRAY_LEN) == 0);
        printf("  InitFilterForRollback: %s\n", match ? "PASS" : "FAIL");
        if (!match) allPassed = false;
    }

    // SaveRerunSounds の結果一致確認
    {
        std::mt19937 rng(777);
        legacy::SfxFilter lf;
        optimized::SfxFilter of;

        for (size_t j = 0; j < legacy::SFX_ARRAY_LEN; ++j) {
            uint8_t val;
            int r = rng() % 4;
            if (r == 0) val = 0x80;
            else if (r == 1) val = 0x81;
            else if (r == 2) val = 0x01;
            else val = 0;
            lf.filterArray[j] = val;
            of.filterArray[j] = val;
        }

        lf.SaveRerunSounds(50);
        of.SaveRerunSounds(50);

        bool match = (memcmp(
            &lf.sfxHistory[50 % legacy::NUM_STATES][0],
            &of.sfxHistory[50 % optimized::NUM_STATES][0],
            legacy::SFX_ARRAY_LEN) == 0);
        printf("  SaveRerunSounds:      %s\n", match ? "PASS" : "FAIL");
        if (!match) allPassed = false;
    }

    printf("  Overall: %s\n", allPassed ? "ALL PASSED" : "SOME FAILED");
    return allPassed;
}

// ============================================================================
// メイン
// ============================================================================

int main() {
    printf("============================================================\n");
    printf("  Rollback Engine Performance Benchmark\n");
    printf("  Legacy (scalar) vs Optimized (SSE2 + Gap-merge)\n");
    printf("============================================================\n");

    // まず正当性検証
    if (!VerifyCorrectness()) {
        printf("\n!!! Correctness check failed. Aborting benchmark. !!!\n");
        return 1;
    }

    constexpr int ITERATIONS = 1000;
    printf("\n  Iterations per test: %d\n", ITERATIONS);

    BenchSaveLoadState(ITERATIONS);
    BenchSfxFilter(ITERATIONS);

    printf("\n============================================================\n");
    printf("  Benchmark Complete\n");
    printf("============================================================\n");

    return 0;
}
