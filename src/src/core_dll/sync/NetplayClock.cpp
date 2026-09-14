// ============================================================================
// NetplayClock.cpp — ネットプレイ同期補正 計算エンジン（実装）
//
// 【NTP T1-T4 方式】
//   各パケット受信時に (T1,T2,T3,T4) サンプルを蓄積し、
//   最小 RTT のサンプルが持つ θ を最良推定値として採用する。
//
// 【α補正】
//   3段階: デッドバンド(500μs) → 二乗カーブ → 飽和(2666μs)
// ============================================================================

#include "core_dll/sync/NetplayClock.hpp"
#include "core_dll/timing/WasapiClock.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace cccaster {
namespace core {
namespace timer {

// ============================================================================
// GetTimeUs — WasapiClock への委譲
// ============================================================================
int64_t NetplayClock::GetTimeUs() {
    return WasapiClock::GetTimeUs();
}

// ============================================================================
// Reset — 全状態リセット
// ============================================================================
void NetplayClock::Reset() {
    std::memset(_samples, 0, sizeof(_samples));
    _sampleIndex = 0;
    _sampleCount = 0;
    _thetaTicks = 0;
    _baselineThetaTicks = 0;
    _anchorTicks = 0;
    _model.Reset();
    _bestRttTicks = INT64_MAX;
    _rttSampleSerial = 0;
    _latestRttTicks = 0;

    _localStartTimeUs = 0;
    _peerStartTimeUs = 0;
}

// ============================================================================
// AddNtpSample — NTP T1-T4 サンプルを追加
// ============================================================================
//
// RTT = (T4 - T1) - (T3 - T2)
// θ   = ((T2 - T1) + (T3 - T4)) / 2
//
// 最小RTTフィルタリング: RTTが最小のサンプルのθを最良推定値として採用。
// 理由: RTT最小 = ネットワークキューイングの影響が最小 = 対称性が最も高い。
//
void NetplayClock::AddNtpSampleTicks(int64_t t1, int64_t t2, int64_t t3, int64_t t4) {
    // 差分と和がint64を越えない範囲で時刻を受理する。
    for (auto t : {t1,t2,t3,t4}) if (t < 0 || t > INT64_MAX/4) return;
    // RTTとθを計算
    int64_t rtt = (t4 - t1) - (t3 - t2);
    int64_t theta = ((t2 - t1) + (t3 - t4)) / 2;

    // 異常値フィルタ: RTT < 0 は無効
    if (rtt < 0 || rtt > ClockSecond || t4 < t1 || t3 < t2)
        return;

    ++_rttSampleSerial;
    _latestRttTicks = rtt;
    _model.AddTicks(t1 + (t4 - t1) / 2, theta, rtt);
    // リングバッファに追加
    ThetaSample &slot = _samples[_sampleIndex];
    slot.t1 = t1;
    slot.t2 = t2;
    slot.t3 = t3;
    slot.t4 = t4;
    slot.rtt = rtt;
    slot.theta = theta;

    _sampleIndex = (_sampleIndex + 1) % SAMPLE_COUNT;
    if (_sampleCount < SAMPLE_COUNT) {
        _sampleCount++;
    }

    // 最小RTTのサンプルを探索 → そのθを採用
    int64_t minRtt = INT64_MAX;
    int64_t bestTheta = 0;
    int64_t bestMid = INT64_MIN;
    for (int i = 0; i < _sampleCount; i++) {
        const auto mid = _samples[i].t1 + (_samples[i].t4 - _samples[i].t1) / 2;
        if (_samples[i].rtt < minRtt || (_samples[i].rtt == minRtt && mid > bestMid)) {
            bestMid = mid;
            minRtt = _samples[i].rtt;
            bestTheta = _samples[i].theta;
            _anchorTicks = _samples[i].t1 + (_samples[i].t4 - _samples[i].t1) / 2;
        }
    }

    _bestRttTicks = minRtt;
    _thetaTicks = bestTheta;
    const auto localMid = t1 + (t4-t1)/2;
    _anchorTicks = localMid;
    if (_model.Ready(localMid)) _thetaTicks = _model.OffsetTicks(localMid);

}

// ============================================================================
// IsThetaStable — θ安定判定
// ============================================================================
// 直近 STABLE_MIN サンプルの θ の σ < STABLE_SIGMA
bool NetplayClock::IsThetaStable() const {
    if (_sampleCount < STABLE_MIN)
        return false;

    int start = (_sampleIndex - STABLE_MIN + SAMPLE_COUNT) % SAMPLE_COUNT;
    double mean = 0;
    for (int i = 0; i < STABLE_MIN; i++) {
        mean += _samples[(start + i) % SAMPLE_COUNT].theta;
    }
    mean /= STABLE_MIN;

    double variance = 0;
    for (int i = 0; i < STABLE_MIN; i++) {
        double diff = _samples[(start + i) % SAMPLE_COUNT].theta - mean;
        variance += diff * diff;
    }
    variance /= (STABLE_MIN - 1);
    double sigma = std::sqrt(variance);

    return sigma < STABLE_SIGMA * 60;
}

// ============================================================================
// GetTickUs — θ直接補正込みの1F周期（α決定アルゴリズム）
// ============================================================================
//
// θ > 0 → 相手の時計が自分より進んでいる → 自分を速くする → 周期を短く
// θ < 0 → 相手の時計が自分より遅れている → 自分を遅くする → 周期を長く
//
// 3段階:
//   1. |θ| ≤ DEAD_BAND_US (500μs): 補正なし（ジッター吸収）
//   2. DEAD_BAND < |θ| ≤ STRONG_TH_US (16666μs): 二乗カーブ（穏やかに加速）
//   3. |θ| > STRONG_TH_US: 飽和（MAX_ALPHA_US = 2666μs）
//
int64_t NetplayClock::GetTickUs() const {
    // サンプル不足: 補正なし
    if (_sampleCount < 3)
        return BASE_TICK_US;

    // ベースラインθからの差分 (Δθ) を使う
    // 絶対クロック差はstartTime合意で吸収済みなので、
    // α補正は「合意後のドリフト」のみを対象とする
    int64_t deltaTheta = (_thetaTicks - _baselineThetaTicks) / 60;
    int64_t absTheta = (deltaTheta >= 0) ? deltaTheta : -deltaTheta;

    int64_t alpha = 0;

    if (absTheta <= DEAD_BAND_US) {
        // ── デッドバンド: ジッター域、補正なし ──
        alpha = 0;
    } else if (absTheta <= STRONG_TH_US) {
        // ── 二乗カーブ: 穏やかに加速 ──
        double t =
            static_cast<double>(absTheta - DEAD_BAND_US) / static_cast<double>(STRONG_TH_US - DEAD_BAND_US);
        alpha = static_cast<int64_t>(t * t * static_cast<double>(MAX_ALPHA_US));
    } else {
        // ── 飽和: 最大補正 ──
        alpha = MAX_ALPHA_US;
    }

    // 符号適用
    int64_t adjusted = BASE_TICK_US;
    if (deltaTheta > 0) {
        adjusted -= alpha; // 相手が先行 → 自分を速く
    } else {
        adjusted += alpha; // 相手が遅延 → 自分を遅く
    }

    // クランプ
    if (adjusted > MAX_TICK_US)
        adjusted = MAX_TICK_US;
    if (adjusted < MIN_TICK_US)
        adjusted = MIN_TICK_US;

    return adjusted;
}

// ============================================================================
// スタート時刻管理
// ============================================================================

void NetplayClock::SetLocalStartTime(int64_t wasapiUs) {
    _localStartTimeUs = wasapiUs;
}

void NetplayClock::SetPeerStartTime(int64_t peerWasapiUs) {
    // 相手のWASAPI時刻をθ変換してローカル基準に
    // peerTime = localTime + θ → localTime = peerTime - θ
    _peerStartTimeUs = peerWasapiUs - _thetaTicks / 60;
}

int64_t NetplayClock::GetAgreedStartTime() const {
    if (_localStartTimeUs <= 0 || _peerStartTimeUs <= 0)
        return 0;
    return std::max(_localStartTimeUs, _peerStartTimeUs);
}

} // namespace timer
} // namespace core
} // namespace cccaster
