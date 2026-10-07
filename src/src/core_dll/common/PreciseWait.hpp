#pragma once
#include <cstdint>
#include <limits>

// ゲーム・音声・ログ・Platform.cppに依存しない時計／待機部品。
// 別の実行ファイルでも、このヘッダとPreciseWait.cppだけで利用できる。
namespace cccaster::platform {
// 共通の単調時計。Ticksは1/60µs単位で、WASAPI時刻や生QPC値ではない。
int64_t RealMonotonicTicks();
int64_t RealMonotonicUs();
inline constexpr int64_t PreciseSpinUs = 1000;

// 絶対締切の1ms前まで休止し、残りをPAUSEで待つ。戻り値は到達した実時刻。
// 過去の締切は即復帰。OSによる中断／締切超過を補正・隠蔽しない。
int64_t PreciseWaitUntilTicks(int64_t deadlineTicks);
// 1ms以下の指定は全区間スピン。周期処理には上の絶対締切版を使う。
void PreciseWaitUs(int64_t durationUs);
// CPUを休ませるポーリング用。スピンせず、起床の遅れを許容する。
void RealSleepUs(int64_t durationUs);
// x86ではPAUSE。OSスケジューラーへのCPU譲渡ではない。
void CpuRelax();

namespace precise_wait_detail {
// 時計値は非負。非常に長い相対待機でも符号付き乗算を溢れさせない。
constexpr int64_t DeadlineAfterUs(int64_t now, int64_t durationUs) {
    constexpr auto limit = std::numeric_limits<int64_t>::max();
    return durationUs <= 0 ? now : durationUs > (limit - now) / 60 ? limit : now + durationUs * 60;
}
// OSを差し替えて、早い起床・遅い起床と最後のスピンの契約を検証する。
// Backend::SleepUntilは精度を保証しない。毎回同じ絶対締切と再照合する。
template<class Backend>
int64_t WaitUntil(Backend &backend, int64_t deadline, int64_t spinTicks) {
    auto now = backend.Now();
    if (now >= deadline) return now;
    while (deadline - now > spinTicks) {
        backend.SleepUntil(deadline - spinTicks);
        now = backend.Now();
        if (now >= deadline) return now;
    }
    while (now < deadline) {
        backend.Relax();
        now = backend.Now();
    }
    return now;
}
}
}
