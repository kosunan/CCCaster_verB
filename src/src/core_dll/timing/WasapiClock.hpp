#pragma once
// ============================================================================
// WasapiClock — WASAPI IAudioClock ベースの高精度クロック
//
// WASAPIの再生位置を基準に、短区間をQPCで連続補間する。音声位相へ速度で調律し、
// 相関標本の変動を出力時刻の段差にしない。精度はデバイスとドライバーに依存する。
// 使用中の取得失敗または速度異常時は実QPCへ連続的に切り替え、起動中は復帰させない。
// 音声APIは専用スレッドだけが呼ぶ。読取りは公開モデルのQPC補間のみ。
// ============================================================================

#include <cstdint>
#include <thread>
#include <atomic>
#include <memory>
#include "core_dll/timing/ClockProjection.hpp"
#include "core_dll/timing/ThreadSignal.hpp"

namespace cccaster {
namespace core {
namespace timer {
class BoundaryRace;

class WasapiClock {
  public:
    static WasapiClock &GetInstance();

    /// WASAPI / QPC の高精度時刻を取得する [μs]。
    /// WASAPI が利用可能な場合はそちらを優先し、
    /// 利用不可の場合は QPC にフォールバックする。
    static int64_t GetTimeUs();
    static uint32_t GetSourceGeneration();
    static int64_t GetTimeTicks(); // 1/60µs、60Hzの1Fは正確に1000000ticks
    struct ReadSample {
        int64_t qpc = 0, time = 0, anchorShift = 0, clamp = 0, ppm = 0;
    };
    // 診断用。同じ読取りに使ったQPCを返し、追加の時計読取りはしない。
    static int64_t GetTimeTicks(ReadSample *sample);
    // 最終200µs用。WASAPIの公開モデルを一度だけ読み、実QPCで待つ。
    static int64_t WaitForRelease(int64_t deadlineTicks);
    // 入力公開待ち/粗待機の前に締切を補助時計へ渡す。ゲームスレッド専用。
    static void PrepareRelease(int64_t deadlineTicks);
    // 初期化はゲームスレッドのコア固定後だけ。公開済みプールの取得は入力側も可能。
    static void InitializeHelpers();
    static BoundaryRace *SharedHelpers();
    struct ReleaseSample {
        int64_t ready, due, exit, readyLate, ppm;
        int64_t boundary, armed, received;
        int workers, valid, covered, winner, requested;
        int64_t stamps[4];
        int64_t completed, publicationUpper;
        bool upperFromReader;
    };
    // ゲームスレッド専用。次Presentで整形し、最終スピンに追加採時を挟まない。
    inline static ReleaseSample releaseSample{};

    /// WASAPI IAudioClock が利用可能かどうか。
    bool IsAvailable() const {
        return _available.load();
    }
    bool IsFallback() const { return fallback_.load(std::memory_order_relaxed); }

  private:
    WasapiClock();
    ~WasapiClock();
    WasapiClock(const WasapiClock &) = delete;
    WasapiClock &operator=(const WasapiClock &) = delete;

    int64_t GetWasapiTimeTicks();
    bool FillSilence();
    static int64_t GetQpcTimeUs();

    void InitializeAudio();
    void ReleaseAudio();
    void Worker();
    ClockProjection projection_;
    std::unique_ptr<BoundaryRace> boundary_;
    std::atomic<BoundaryRace *> sharedBoundary_{nullptr};
    bool boundaryInitialized_ = false;
    int64_t preparedAudio_ = 0, preparedQpc_ = 0, preparedPpm_ = 0;
    std::atomic<bool> stopping_{false};
    ThreadSignal wake_;
    std::thread worker_;
    std::atomic<bool> _available{false};
    std::atomic<bool> fallback_{false};
    bool comInitialized_ = false;

    // COMオブジェクト（前方宣言のため void* で保持、.cpp で具象型にキャスト）
    void *_pAudioClient = nullptr;
    void *_pAudioClock = nullptr;
    void *_pRenderClient = nullptr;
    uint32_t _bufferFrames = 0;
    int64_t _lastFillUs = 0;
    uint64_t _frequency = 0;
    bool _isStarted = false;
};

} // namespace timer
} // namespace core
} // namespace cccaster
