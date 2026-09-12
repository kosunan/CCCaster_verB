# fix(core): VirtualClock::WaitForNextFrame の FPS異常を修正 (E-5)

## 変更内容
### `src/core/timer/VirtualClock.cpp` 
- `WaitForNextFrame()` 内の `_lastFrameTimeUs` の更新ロジックを修正。

変更前：
```cpp
_lastFrameTimeUs = GetRawQpcTimeUs(); // 実際の現在時刻
```

変更後：
```cpp
int64_t actualNow = GetRawQpcTimeUs();
if (actualNow - targetTime > _baseFrameDurationUs) {
    // 深刻な処理落ち時はリセット
    _lastFrameTimeUs = actualNow;
} else {
    // 正常時は理論値(開始予定時刻)を維持し、次フレームで処理時間を相殺する
    _lastFrameTimeUs = targetTime;
}
```

## 背景・理由 (docs/design/high_precision_timer_design.md 準拠)
従来の単純な「現在時刻から開始」では、自フレーム内で消費した「実処理時間」が加算され続け、1フレームの時間が `(16.6ms + 処理時間)` となってしまい FPS が 60 を大幅に下回る問題（遅延）が発生していました。
今回の修正により、実処理時間が `_baseFrameDurationUs (16.6ms)` 以内に収まる限り、**次のフレームの待機時間から実処理時間分が差し引かれる**ようになり、厳密な 60FPS の周期同期が実現されます。

## 動作確認
- `cccaster_hook` のビルド[100%]成功
- `DummyPeer.exe --test-mode e2e` 実行時に同期ループが正常に進行しタイマーがクラッシュ・ストールしないことを確認。
