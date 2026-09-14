#pragma once
// ============================================================================
// NetplayClock — ネットプレイ同期補正 計算エンジン（純粋関数群）
//
// 【責務】
//   NTP T1-T4 方式によるθ推定、α（1F周期補正）算出、スタート時刻管理。
//   スレッドを持たない。I/Oをしない。値を渡されて計算して返すだけ。
//   SyncCodec（パケット解析層）が呼び出す。
//
// 【θ推定方式 (NTP T1-T4 Ping-Pong)】
//   T1: 自分がパケットを送信した時刻（自分の WASAPI）
//   T2: 相手がパケットを受信した時刻（相手の WASAPI）← 相手が返信に載せる
//   T3: 相手が返信を送信した時刻（相手の WASAPI）← 相手が載せる
//   T4: 自分が返信を受信した時刻（自分の WASAPI）
//
//   RTT = (T4 - T1) - (T3 - T2)
//   θ   = ((T2 - T1) + (T3 - T4)) / 2
//
//   学習中は直近30標本の最小RTTを初期推定に使う。
//   独立16秒窓が一致したらアフィン時計モデルを固定し、継続観測で検証する。
//   旧GetTickUsは互換API。実通信の補正はPeerClockModelとPhaseFollowerが担当。
// ============================================================================

#include <cstdint>
#include "core_dll/timing/PeerClockModel.hpp"

namespace cccaster {
namespace core {
namespace timer {

// ── NTP T1-T4 サンプル ──
struct ThetaSample {
    int64_t t1;    // 自分の送信時刻
    int64_t t2;    // 相手の受信時刻
    int64_t t3;    // 相手の送信時刻
    int64_t t4;    // 自分の受信時刻
    int64_t rtt;   // 往復遅延 = (T4-T1) - (T3-T2)
    int64_t theta; // クロックオフセット = ((T2-T1) + (T3-T4)) / 2
};

class NetplayClock {
  public:
    // ── WASAPIクロック（WasapiClock委譲）──────────
    static int64_t GetTimeUs();

    // ── θ推定 (NTP T1-T4) ────────────────────────
    /// NTPサンプルを追加。内部でRTT/θを計算し、最小RTTフィルタを更新。
    void AddNtpSampleTicks(int64_t t1, int64_t t2, int64_t t3, int64_t t4);
    // 旧APIと診断表示の互換窓口。実通信・締切計算はTicksだけを使う。
    void AddNtpSample(int64_t t1, int64_t t2, int64_t t3, int64_t t4) {
        AddNtpSampleTicks(t1*60,t2*60,t3*60,t4*60);
    }

    /// 現在のθ推定値 [μs]（診断用。採用後は固定モデルの投影値）
    int64_t GetThetaUs() const {
        return _thetaTicks / 60;
    }

    /// 時計推定で採用した最小RTT [μs]
    int64_t GetRttUs() const {
        return _bestRttTicks == INT64_MAX ? INT64_MAX : _bestRttTicks / 60;
    }

    // 受理NTPサンプルの診断値。通信スレッドだけで読む。
    uint64_t GetRttSampleSerial() const { return _rttSampleSerial; }
    int64_t GetLatestRttUs() const { return _latestRttTicks / 60; }

    /// θが安定しているか（最小サンプル数 && σ < 閾値）
    bool IsThetaStable() const;
    bool HasTimingEstimate() const {
        return _sampleCount >= STABLE_MIN;
    }

    // ── 1F周期算出（α補正込み）──────────────────
    /// 旧θ補正の互換API。現行通信は時計モデルと入力締切で補正する。
    int64_t GetTickUs() const;

    // ── スタート時刻管理 ─────────────────────────
    void SetLocalStartTime(int64_t wasapiUs);
    void SetPeerStartTime(int64_t peerWasapiUs);
    int64_t GetAgreedStartTime() const;

    double GetDriftRate() const { return _model.DriftRate(_anchorTicks); }
    const PeerClockModel &Model() const { return _model; }
    int64_t GetThetaTicks() const { return _thetaTicks; }
    int64_t GetRttTicks() const { return _bestRttTicks; }
    int64_t EstimatePeerTimeTicks(int64_t now) const {
        return now + (_model.Ready(now) ? _model.OffsetTicks(now) : _thetaTicks);
    }
    int64_t PeerToLocalTicks(int64_t peer, int64_t now) const {
        return now + int64_t(std::llround(double(peer - EstimatePeerTimeTicks(now)) /
                                          (1.0 + _model.DriftRate(now))));
    }
    int64_t EstimatePeerTimeUs(int64_t now) const { return EstimatePeerTimeTicks(now*60)/60; }
    int64_t PeerToLocalUs(int64_t peer, int64_t now) const { return PeerToLocalTicks(peer*60,now*60)/60; }
    /// 全状態リセット
    void Reset();

    /// Counting開始時にベースラインθを記録。α補正はここからの差分のみで行う。
    void SetBaselineTheta() {
        _baselineThetaTicks = _thetaTicks;
    }
    int64_t GetBaselineTheta() const {
        return _baselineThetaTicks / 60;
    }

    // ── 定数 ────────────────────────────────────
    static constexpr int64_t BASE_TICK_US = 16666; // 60fps
    static constexpr int64_t MAX_TICK_US = 19332;  // BASE + MAX_ALPHA
    static constexpr int64_t MIN_TICK_US = 14000;  // 加速下限
    static constexpr int64_t MAX_ALPHA_US = 2666;  // 最大αμs (≈16%速度差)
    static constexpr int64_t DEAD_BAND_US = 500;   // デッドバンド
    static constexpr int64_t STRONG_TH_US = 16666; // 飽和閾値 (1F)

    static constexpr int SAMPLE_COUNT = 30;        // リングバッファサイズ
    static constexpr int STABLE_MIN = 10;          // 安定判定の最小サンプル数
    static constexpr double STABLE_SIGMA = 1000.0; // 安定判定σ閾値 [μs]

  private:
    // ── NTPサンプルリング ────────────────────────
    PeerClockModel _model;
    int64_t _anchorTicks = 0;
    ThetaSample _samples[SAMPLE_COUNT] = {};
    int _sampleIndex = 0;
    int _sampleCount = 0;
    int64_t _thetaTicks = 0;           // 最良θ（最小RTTサンプル）
    uint64_t _rttSampleSerial = 0;
    int64_t _latestRttTicks = 0;
    int64_t _bestRttTicks = INT64_MAX; // 最小RTT
    int64_t _baselineThetaTicks = 0;     // Counting開始時のθ（α補正の基準点）

    // ── スタート時刻 ─────────────────────────────
    int64_t _localStartTimeUs = 0;
    int64_t _peerStartTimeUs = 0;
};

} // namespace timer
} // namespace core
} // namespace cccaster
