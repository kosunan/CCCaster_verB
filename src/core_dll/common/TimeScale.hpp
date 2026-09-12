#pragma once
// ============================================================================
// TimeScale — 自動テスト用の時間圧縮
//
// 【何のためか】
//   harness も実機テストも実時間で動くため、2,800フレームの検証に47秒かかる。
//   フレーム周期を 1/N にすれば同じ検証が N 分の1の時間で終わり、
//   試行回数を増やせる。
//
// 【スケールしてよいもの / いけないもの】
//   スケールする: フレーム周期（Metronome のティック、通信スレッドの周期、
//                 ハーネスの外側ペース）。両プロセスが同じ値を読むので歩調は揃う。
//   スケールしない: θ推定・RTT・α補正。これらは実時刻で測る値であり、
//                 いじると測っているものが変わってしまう。
//
// 【何が検証できなくなるか】
//   4倍速では1フレームの実時間が 16.6ms → 4.2ms になる。RTT に対する余裕が
//   1/4 になるため、**タイミング余裕の検証にはならない**。
//   区切りの確認（実機ゲート）は必ず等倍で回すこと。
//
// 【有効化】
//   環境変数 CCCASTER_TIME_SCALE=4 など。既定は 1（等倍）。
// ============================================================================

#include <cstdint>
#include <cstdlib>

namespace cccaster::testing {

/// 時間圧縮の倍率。1〜64 の範囲外や未設定は 1（等倍）。
inline int TimeScale() {
    static const int scale = [] {
        const char *v = std::getenv("CCCASTER_TIME_SCALE");
        if (!v)
            return 1;
        const int s = std::atoi(v);
        return (s >= 1 && s <= 64) ? s : 1;
    }();
    return scale;
}

/// フレーム周期を圧縮する。0 にならないよう最低 1μs を保証する。
inline int64_t ScaleTickUs(int64_t us) {
    const int64_t scaled = us / TimeScale();
    return (scaled > 0) ? scaled : 1;
}

} // namespace cccaster::testing
