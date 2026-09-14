# 仮想クロック（Virtual Clock）ドリフト補正およびスルー処理 実装設計書

## 1. 概要
格闘ゲームのP2P通信（ロールバックネットコード）において、ネットワーク越しに算出された相対クロックオフセット（θ）を適用する際、時計が突然未来や過去に飛ぶ「ステッピング（Stepping）」現象が発生すると、物理演算や入力判定が破綻します。
そのため、オフセットの変動を毎フレーム少しずつ適用する「スルー（Slewing）」処理と、ハードウェア固有の時計の進み方の違い（クロックドリフト）を予測するロジックを備えた `VirtualClock` を設計します。

## 2. 実装要件
1. **仮想クロック層**: 生のローカル時刻に補正値を加えた仮想的な絶対時刻を提供する。
2. **スルー（Slewing）**: 突然の同期ズレを検出した際、1フレームあたりの最大補正量（Max Slew Rate）を制限し、滑らかに時間を調整する。
3. **ドリフト率（Drift Rate）の予測**: 定期的な Ping-Pong により算出された目標θが右肩上がり（または下がり）に変化する場合、その変化率を算出し、時計のベース速度に乗算する。
4. **QPCを用いた Watchdog（安全装置）**: WASAPI がオーディオリセット等で数百ms以上飛躍した場合、即座にそれを検知してQPC（QueryPerformanceCounter）にフォールバックする。

---

## 3. クラス設計（C++）

### ヘッダー: `VirtualClock.hpp`
```cpp
#pragma once
#include <cstdint>
#include <atomic>
#include <windows.h>

namespace cccaster::sync {

class VirtualClock {
public:
    static VirtualClock& GetInstance();

    // システム起動時に呼び出す初期化処理
    void Initialize();

    // 測定された最新の目標オフセット（θ）を通知する（TimeSynchronizerから呼ばれる）
    void SetTargetOffset(double targetOffsetUs);

    // 毎フレーム（ゲームループ先頭など）で呼び出し、スルー処理とドリフト計算を進行させる
    void Update();

    // スルー処理とドリフト補正が適用された仮想絶対時刻を取得する
    int64_t GetSynchronizedTimeUs() const;

private:
    VirtualClock() = default;

    // --- ローカル時刻取得ヘルパー ---
    int64_t GetRawWasapiTimeUs() const;
    int64_t GetRawQpcTimeUs() const;

    // --- State Variables ---
    bool _isInitialized = false;
    bool _wasapiFaultDetected = false; // Watchdogによって異常検知されたフラグ

    // ロールバックによる時刻精度低下を防ぐため、内部計算は double を用いる
    double _currentCorrectionUs = 0.0; // 現在適用済みの補正値（滑らかに変化する）
    double _targetOffsetUs = 0.0;      // NTP(TimeSynchronizer)で算出した目標とする補正値

    // ドリフト予測用
    double _driftRate = 1.0;           // 通常は 1.0 (等倍)。遅れがちなら 1.00002 等になる
    int64_t _lastEstimationTime = 0;   // 最後に targetOffset を受け取った時刻 (QPC基準)

    // Watchdog 用
    int64_t _lastWasapiTime = 0;
    int64_t _lastQpcTime = 0;

    // --- Constants ---
    // 1フレームあたりに許容する最大のスルー（時間調整）量（マイクロ秒）
    // 60FPS(16666us) において、1フレームあたり 50us (0.3%の加速/減速)
    static constexpr double MAX_SLEW_RATE_US_PER_FRAME = 50.0;
    
    // Watchdogの閾値：WASAPIとQPCの進み方の差分が 15ms (15000us) を超えたら異常とみなす
    static constexpr int64_t WATCHDOG_FAULT_THRESHOLD_US = 15000;
};

} // namespace cccaster::sync
```

### 実装: `VirtualClock.cpp`
```cpp
#include "VirtualClock.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>

namespace cccaster::sync {

VirtualClock& VirtualClock::GetInstance() {
    static VirtualClock instance;
    return instance;
}

void VirtualClock::Initialize() {
    _lastWasapiTime = GetRawWasapiTimeUs();
    _lastQpcTime = GetRawQpcTimeUs();
    _currentCorrectionUs = 0.0;
    _targetOffsetUs = 0.0;
    _driftRate = 1.0;
    _lastEstimationTime = _lastQpcTime;
    _wasapiFaultDetected = false;
    _isInitialized = true;
}

void VirtualClock::SetTargetOffset(double targetOffsetUs) {
    int64_t now = GetRawQpcTimeUs();
    
    // ドリフト率 (Drift Rate) の計算
    // 例: 10秒間に offset が 200us 増えたら、ドリフト率は 1.0 + (200 / 10,000,000) = 1.00002
    if (_lastEstimationTime > 0 && now > _lastEstimationTime) {
        double elapsedUs = static_cast<double>(now - _lastEstimationTime);
        double offsetDelta = targetOffsetUs - _targetOffsetUs;
        
        // 極端なスパイクによるドリフト率の破壊を防ぐため、更新上限を設ける
        if (std::abs(offsetDelta) < 5000.0) { // 5ms以内の変動ならドリフトとして学習
            double newDriftRate = 1.0 + (offsetDelta / elapsedUs);
            
            // 指数移動平均(EMA)で滑らかにドリフト率を更新 (ノイズ対策)
            _driftRate = (_driftRate * 0.9) + (newDriftRate * 0.1);
        }
    }

    _targetOffsetUs = targetOffsetUs;
    _lastEstimationTime = now;
}

void VirtualClock::Update() {
    if (!_isInitialized) return;

    // --- 1. QPC Watchdog & WASAPI フェイルセーフ ---
    int64_t currentWasapi = GetRawWasapiTimeUs();
    int64_t currentQpc = GetRawQpcTimeUs();

    int64_t deltaWasapi = currentWasapi - _lastWasapiTime;
    int64_t deltaQpc = currentQpc - _lastQpcTime;

    // WASAPIの進行とQPCの進行の乖離をチェック
    if (!_wasapiFaultDetected) {
        if (std::abs(deltaWasapi - deltaQpc) > WATCHDOG_FAULT_THRESHOLD_US) {
            std::cerr << "[VirtualClock] WARNING: WASAPI clock jumped abnormally. Falling back to QPC.\n";
            _wasapiFaultDetected = true;
        }
    }

    _lastWasapiTime = currentWasapi;
    _lastQpcTime = currentQpc;

    // --- 2. スルー（Slewing）処理 ---
    double diff = _targetOffsetUs - _currentCorrectionUs;
    
    if (std::abs(diff) > 0.1) {
        // 目標にまだ達していない場合、スルーレートの範囲内で引き寄せる
        double step = std::min(MAX_SLEW_RATE_US_PER_FRAME, std::abs(diff));
        if (diff > 0) {
            _currentCorrectionUs += step;
        } else {
            _currentCorrectionUs -= step;
        }
    }
}

int64_t VirtualClock::GetSynchronizedTimeUs() const {
    // 異常検知時はQPCにフォールバック、正常時はWASAPIを使用
    int64_t rawHardwareTime = _wasapiFaultDetected ? GetRawQpcTimeUs() : GetRawWasapiTimeUs();
    
    // ベース時刻からの経過時間をドリフト率でスケールし、さらにスルー済みの補正値を加算する
    // ※ここでは簡易的に最新のローカル時刻に適用していますが、
    // 実運用では Initialize() 時点のベース時刻 `T0` を記憶し、(T - T0) * DriftRate とする式がより高精度です。
    return static_cast<int64_t>(rawHardwareTime + _currentCorrectionUs);
}

// (以下 GetRawWasapiTimeUs / GetRawQpcTimeUs 実装は既存の WasapiClock / QueryPerformanceCounter を呼び出す)
int64_t VirtualClock::GetRawWasapiTimeUs() const { /* ... */ return 0; }
int64_t VirtualClock::GetRawQpcTimeUs() const { /* ... */ return 0; }

} // namespace cccaster::sync
```

---

## 4. スルーレート（Max Slew Rate）の適切な初期値について

`MAX_SLEW_RATE_US_PER_FRAME` の考え方は、**「ゲームの体験（カクつき、フレームレートのブレ）に気づかない上限はどこか？」** に基づきます。

- **格闘ゲームの1フレーム**: 60FPS = `16666.6 us`
- **人間の知覚限界**:
  - 一般的に、1フレームの時間が 16.6ms から `16.1ms` に急上昇（減速）したり `17.1ms` まで遅延したりしても（約3%の変動）、映像として人間はカクつきを感じません。しかし、これが 2～3ミリ秒 (10%以上の変動) のブレになると、「ゲームが一瞬引っかかった」と感じます。

- **推奨される閾値**:
  - **保守的（非常に滑らか）**: 1フレームあたり `50us` (約 0.3%の変動)。
    - 例: 仮にPing-Pongで時計が 5000us (5ms) ズレていると判明した場合、それを補正しきるのに `100フレーム（約1.6秒）` かかります。徐々に追いつくため非常に自然で、ゲームロジックへの衝撃はゼロです。
  - **アグレッシブ（速やかな同期）**: 1フレームあたり `250us` (約 1.5%の変動)。
    - 5ms のズレを `20フレーム（約0.3秒）` で補正します。格闘ゲームのラウンド開始時やローディング中など、**「プレイ中ではないフェーズ」に限定してアグレッシブなレートを適用する** のが最も実用的です。

**実装上のテクニック**:
`MAX_SLEW_RATE` を定数ではなく可変（変数）にし、**プレイアブルなラウンド中は 50us、ロード画面やキャラセレ画面では 1000us** といったように切り替えることで、同期の速さとゲームプレイの安定性を両立できます。


---

# 片道通信時間（OWD）算出アルゴリズム 実装・テスト計画書

## 1. 目的
本ドキュメントは、NTPライクな相対的クロックオフセット推定を用いたUDP片道遅延（One-Way Delay: OWD）算出アルゴリズムの実装方針とテスト計画を定義する。
外部の絶対時刻同期（NTPサーバー等）を用いず、`WASAPI` などの高精度ローカルタイムスタンプを基準としたベストエフォートな擬似同期を実現する。

## 2. 実装設計 (C++)

すでにベースとなる `TimeSynchronizer` クラスが存在するため、これを拡張して完全なOWDの計算要件を満たす設計とする。

### 2.1. パケット構造定義

送受信するパケットは、`TimeSynchronizer.hpp` 内の定数と構造体として定義する。

```cpp
namespace cccaster::sync {

// パケットタイプ
enum PacketType : uint8_t {
    SYNC_REQ = 0x10,
    SYNC_RES = 0x11,
    SYNC_DONE = 0x12
};

// SYNC_REQ (A -> B)
// サイズ: 1 + 8 = 9 bytes
#pragma pack(push, 1)
struct SyncReqPacket {
    uint8_t type = SYNC_REQ;
    int64_t t1; // Aでの送信時刻
};
#pragma pack(pop)

// SYNC_RES (B -> A)
// サイズ: 1 + 8 + 8 + 8 = 25 bytes
#pragma pack(push, 1)
struct SyncResPacket {
    uint8_t type = SYNC_RES;
    int64_t t1; // Aでの送信時刻(そのまま返す)
    int64_t t2; // Bでの受信時刻
    int64_t t3; // Bでの送信時刻
};
#pragma pack(pop)

}
```

### 2.2. クロックオフセット（θ）の測定フェーズ

役割を明確化する。通常、A（クライアント）がB（ホスト）に対してPINGを打つモデルを採用する。

1. **A（クライアント）の処理 (`Update` メソッド内で周期実行)**
   - `WasapiClock`（フォールバック時はQPC）からローカル時刻 `T1` を取得。
   - `SyncReqPacket` に `T1` を詰めてBへ送信。

2. **B（ホスト）の処理 (`OnReceiveSyncPacket` メソッド内)**
   - パケット受信直後にローカル時刻 `T2` を取得。
   - 返信パケット生成直前にローカル時刻 `T3` を取得。
   - `SyncResPacket` に `[T1, T2, T3]` を詰めてAへ返信。

3. **A（クライアント）の返信受信処理 (`OnReceiveSyncPacket` メソッド内)**
   - 返信受信直後にローカル時刻 `T4` を取得。
   - $RTT = (T_4 - T_1) - (T_3 - T_2)$
   - $\theta = \frac{(T_2 - T_1) + (T_3 - T_4)}{2}$
   - サンプル履歴リスト `std::vector<SyncSample> _samples` に追加。

### 2.3. 最小RTTフィルタリング

一定回数（例: 30回）のサンプルが収集された後、クライアント側でフィルタリングを実行する。

```cpp
if (_samples.size() >= 30) {
    // RTTが最小のサンプルを探索
    auto bestSample = std::min_element(_samples.begin(), _samples.end(), 
        [](const SyncSample& a, const SyncSample& b) {
            return a.rtt < b.rtt;
        });
        
    // 最終的なオフセットとRTTの採用
    _clockOffsetUs = bestSample->offset;
    _estimatedRttUs = bestSample->rtt;
    _isSynced = true;
    
    // ホストに完了を通知 (SYNC_DONE)
}
```

### 2.4. 片道通信時間 (OWD) の算出フェーズ

同期完了後（$\theta$ が確定後）、他の一般的なUDPパケット（ゲームの入力同期など）の送信時に各自のタイムスタンプを付与し、受信側でOWDを算出する。

```cpp
// 基準クロック関数（常に自分の高精度ローカルタイマーを返す）
int64_t TimeSynchronizer::GetLocalTimeUs();

// 相手に合わせるための共通時計（ホスト基準）
int64_t TimeSynchronizer::GetSynchronizedTimeUs() {
    int64_t local = GetLocalTimeUs();
    return isHost ? local : local + _clockOffsetUs;
}
```

**OWD計算ヘルパー (例: 受信側での処理)**
```cpp
// A -> B (上り)
int64_t CalculateOwdAtoB(int64_t t_send_A, int64_t t_recv_B, int64_t theta) {
    return (t_recv_B - theta) - t_send_A;
}

// B -> A (下り)
int64_t CalculateOwdBtoA(int64_t t_send_B, int64_t t_recv_A, int64_t theta) {
    return t_recv_A - (t_send_B - theta);
}
```

---

## 3. テスト計画

本モジュールをDLLの組み込み環境で検証するため、以下の3段階のテストを実施する。

### テストフェーズ 1: 単体ロジックテスト (Offline / Mock)
- **目的**: タイムスタンプ取得と、RTT・オフセットの計算式が数学的に正しいかを検証する。
- **手法**: カスタム関数内でダミーの `T1, T2, T3, T4` 値を生成し、`CalculateOwdAtoB` 等が意図した遅延を返すかを確認する。
- **確認事項**:
  - 非対称な遅延（T1->T2 は 10ms, T3->T4 は 40ms）を与えた場合の$\theta$が計算通りになるか。
  - 最小RTTフィルタリングが正常に一番条件の良いサンプルを選択するか。

### テストフェーズ 2: ローカルループバックテスト (Localhost 127.0.0.1)
- **目的**: 実際の `WASAPI` (または QPC) と `UdpSocket` を使用したUDPパケットの送受信ロスや遅延ゼロ環境での動作確認。
- **手法**: `MainApp.exe` を2つ立ち上げ、127.0.0.1 の別ポート同士で接続を行う。
- **確認事項**:
  - PING送信が30回正常に行われるか。
  - ローカルであるため、計算されるRTTは0〜1ms程度に収まり、$\theta$もほぼ安定した微小な値になること。
  - クライアント側からホストへ `SYNC_DONE` が到達すること。

### テストフェーズ 3: リモート接続・人工遅延テスト
- **目的**: ジッターや非対称遅延が混じるネットワーク越しの環境を模倣し、耐性を確認する。
- **手法**: 
  - `Clumsy` （Windows用ネットワークシミュレーター）等のツールを用いて、人工的にラグ(Lag)やジッター(Jitter)を付与する。
  - ロード画面の同期フェーズ（`ProcessLoadingSync`）中にこの処理を組み込み、ゲーム進行が再開されるか確認する。
- **確認事項**:
  - ジッターがあっても最小RTTの抽出により、$\theta$の推定精度が安定しているか。
  - 定期的に補正を行う（クロックドリフト対策）仕組みが破綻せずに機能するか。


---


