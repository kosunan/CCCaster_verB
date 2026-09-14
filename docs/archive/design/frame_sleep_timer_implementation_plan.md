# 高精度WASAPIフレームスリープタイマー (FrameSleepTimer) 実装設計書

## 1. 目的
格闘ゲームにおける 60FPS (1フレームあたり約 16666 マイクロ秒) の進行速度を極めて正確に維持するため、`WASAPI (IAudioClock)` を基準とした高精度のスリープ機能を提供する。
既存の `Sleep()` や `QueryPerformanceCounter` に依存したゲーム内のループ待機処理（`worldTimer`関連の処理など）をオーバーライドし、OSのスケジューラ精度(タイマー分解能)に影響されない精密なペーシングを実現する。

## 2. 要件
1. **処理時間の差し引き**: 1フレームの開始時刻（`T_start`）を記録し、実際のゲームロジック・描画処理が終わった時刻（`T_end`）で差分（`Elapsed` = 処理にかかった時間）を計算する。
2. **残り時間の待機**: 1フレームの目標時間（`Target = 16666 us`等）から `Elapsed` を引いた「残り時間」だけを正確に待機（Sleep）する。
3. **混合スリープアプローチ**:
   - `std::this_thread::sleep_for` や `Sleep()` は精度が甘く（OSタイマー分解能の1ms～15msの誤差が出る）、オーバーランしてフレーム落ちを引き起こすリスクがある。
   - そのため、**残り時間が長い場合（例: 2ms以上）は `Sleep` で大まかに休ませてCPU負荷を下げ、最後の数ミリ秒～数百マイクロ秒は WASAPI の時刻を監視しながら空回り（SpinLock / Busy-Wait）する設計** とする。
4. **WASAPI基準の採用**: 時刻の測定は `cccaster::core_dll::timer::VirtualClock` もしくは `TimeSynchronizer::GetLocalTimeUs()` (WASAPI優先、QPCフォールバック) を用いる。

---

## 3. クラス設計（C++）

### ヘッダー: `FrameSleepTimer.hpp`
```cpp
#pragma once
#include <cstdint>

namespace cccaster::core_dll::timer {

class FrameSleepTimer {
public:
    static FrameSleepTimer& GetInstance();

    // 1フレームの目標時間を設定する (例: 60FPSなら 16666 または 16667 us)
    void SetTargetFrameTimeUs(int64_t targetUs = 16666);

    // フレームの処理が開始したタイミングで呼び出し、開始時刻をマークする
    void MarkFrameStart();

    // フレームの終端で呼び出す。
    // MarkFrameStart()からの経過時間を計測し、目標フレーム時間に達するまで精密にスリープ(待機)する。
    void WaitUntilNextFrame();

private:
    FrameSleepTimer() = default;

    int64_t _frameStartUs = 0;
    int64_t _targetFrameTimeUs = 16666;

    // スリープを Busy-Wait(スピンロック) に切り替える閾値 (2ms = 2000us)
    // これより残り時間が長い場合は Sleep(1) を用いてCPU負荷を下げる。
    static constexpr int64_t SPIN_LOCK_THRESHOLD_US = 2000; 
};

} // namespace cccaster::core_dll::timer
```

### 実装: `FrameSleepTimer.cpp`
```cpp
#include "cccaster/core_dll/timer/FrameSleepTimer.hpp"
#include "cccaster/core_dll/timer/TimeSynchronizer.hpp"
#include <windows.h>
#include <thread>

namespace cccaster::core_dll::timer {

FrameSleepTimer& FrameSleepTimer::GetInstance() {
    static FrameSleepTimer instance;
    return instance;
}

void FrameSleepTimer::SetTargetFrameTimeUs(int64_t targetUs) {
    _targetFrameTimeUs = targetUs;
}

void FrameSleepTimer::MarkFrameStart() {
    // 常に最新の生のハードウェア時刻（WASAPI優先）を取得する
    _frameStartUs = TimeSynchronizer::GetLocalTimeUs();
}

void FrameSleepTimer::WaitUntilNextFrame() {
    // 1. フレーム内処理時間の計算
    int64_t nowUs = TimeSynchronizer::GetLocalTimeUs();
    int64_t elapsedUs = nowUs - _frameStartUs;
    int64_t remainingUs = _targetFrameTimeUs - elapsedUs;

    // もし既に処理落ち(目標時間を超過)しているなら即座にリターン
    if (remainingUs <= 0) {
        return;
    }

    // OSのタイマー分解能を最高(1ms)にする設定 (※アプリ起動時に1度だけ呼ぶ設計でも良い)
    timeBeginPeriod(1); 

    // 2. 精度の荒い Sleep() によるハイブリッド待機
    // 残り時間が閾値(2ms)以上ある場合だけ Sleep(1) し、CPU負荷を劇的に下げる。
    while (remainingUs > SPIN_LOCK_THRESHOLD_US) {
        Sleep(1); 
        
        nowUs = TimeSynchronizer::GetLocalTimeUs();
        elapsedUs = nowUs - _frameStartUs;
        remainingUs = _targetFrameTimeUs - elapsedUs;
    }

    timeEndPeriod(1);

    // 3. Busy-Wait (SpinLock) による超精密待機
    // 最後の数ミリ秒・数百マイクロ秒は WASAPI の時間をずっと監視し続けて極限まで精度を上げる
    while (true) {
        nowUs = TimeSynchronizer::GetLocalTimeUs();
        if ((nowUs - _frameStartUs) >= _targetFrameTimeUs) {
            break; // 待ち終了
        }
        
        // 極小の空回り防止として Yield を挟む（OSによるが無くてもよい）
        std::this_thread::yield(); 
    }
}

} // namespace cccaster::core_dll::timer
```

---

## 4. この実装のメリットと導入箇所

### 導入箇所
* 旧 `CCCaster` のロジックでは、`worldTimer` という固定長整数カウンタを用いて、ゲーム内の `Sleep` 処理を手書きのアセンブリ・パッチで処理していました。
* ロールバックや観戦同期において、メインゲームループ（`App` 側や `MainController`）が `1フレーム進める` という処理を呼び出した直後に、**この `FrameSleepTimer::WaitUntilNextFrame()` を1回呼び出す構成** に差し替えることで、劇的にフレーム安定性が向上します。

### 補足事項（ハイブリッド待機アプローチ）
完全な `Busy-Wait (whileループで時間を監視し続ける)` は精度が最も高いですが、CPUコアの利用率が常に `100%` に貼りついてしまい発熱などの問題を引き起こします。
そのため、`timeBeginPeriod(1)` と `Sleep(1)` を組み合わせて、目標時刻の `2ms` 前まではCPUを休ませ、最後の一瞬だけ `Busy-Wait` で高精度に合わせる（混合スリープ）という格闘ゲームのPC向け移植でよく用いられる標準的なテクニックを採用しています。
