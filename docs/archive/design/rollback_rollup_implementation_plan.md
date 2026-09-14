# ロールバック・ロールアップ処理 実装計画書 (File Mapping Strategy)

`docs/design/core_dll/rollback_rollup_design.md` で定義した「5つの不安材料への解決策」を実現するために、`src/app/core_dll/` 内のモジュールにどのように責務を割り当て、どのファイル名で実装していくかの計画案を定義します。

---

## 1. 新規作成または大幅修正するファイル一覧

### A. スレッド競合保護付きのパケット受信トレイ
**ファイルパス:** `src/app/core_dll/network/UdpPacketQueue.hpp` / `.cpp` (新規作成)
**担当する不安要素:** D. マルチスレッドアクセスの衝突 (Thread Collision)
**責務:**
- `std::mutex` や `std::lock_guard` を用いたスレッドセーフなキュー（またはリングバッファ構造）。
- `UdpSocket` の受信スレッドはこのキューに「相手の入力とフレーム番号」をPushするだけで終わる。
- メインスレッドの `UpdateFrame` 処理の先頭でこのキューからPopし、そのタイミングでのみロールバック判定処理を非同期ではなく「同期的」に実行させる。

### B. 描画スキップとSFX補正・リプレイ補正の管理
**ファイルパス:** `src/app/core_dll/sync/RollbackEngine.cpp` (既存変更・拡張)
**担当する不安要素:** A. パフォーマンススパイク, B. 音の多重再生, C. リプレイ破損
**責務:**
- `DoRollback` 関数内のループにおいて、MBAAの `CC_SKIP_FRAMES_ADDR` を操作してDirectXの描画命令を停止させる機能の追加 (A)。
- 各不安要素を抽象化し、実際のハック処理は `GameHooks` や後述の専用マネージャーへ委譲するパイプラインを構築する。

### C. オーディオミュート管理
**ファイルパス:** `src/app/core_dll/sync/AudioFilter.hpp` / `.cpp` (新規作成)
**担当する不安要素:** B. 音の多重再生 (SFX Desync)
**責務:**
- 単純かつ堅牢な「完全ミュート機能」を提供する。
- `AudioFilter::EnableMute()` で、対象となるSFXメモリ領域全てのミュートフラグを立てる（あるいは音量ゼロにする）。
- `AudioFilter::DisableMute()` でミュートを解除する。過去の履歴追跡などの複雑な処理は一切行わない。

### D. ネイティブメモリの直接操作群 (リプレイ・RNG同期)
**ファイルパス:** `src/app/core_dll/game_interface/GameHooks.cpp` (既存変更・拡張)
**担当する不安要素:** C. リプレイ破損, E. RNGの手動同期
**責務:**
- **リプレイ補正:** `RepRound` 構造体へのポインタを強引にデクリメントする `FixReplayPointers(int rollbackFrames)` のような関数の露出。
- **RNG同期:** `getInitialRngState()` および `setInitialRngState()` として、`CC_RNG_STATE0123_ADDR` を取得・上書きするインターフェースの提供。対戦開始時のネットワーク調停時にこれを呼び出す。

---

## 2. 結合後のRollupパイプライン (UpdateFrame のイメージ)

メインループ（メインスレッド）から呼ばれる同期エンジンの全体像のイメージです。

```cpp
bool RollbackEngine::UpdateFrame(uint16_t localInput) {
    // 1. [スレッド安全] スレッドセーフなトレイから受信済みパケットをまとめてPop (解決策D)
    auto remotePackets = UdpPacketQueue::GetInstance().PopAll();
    for (auto& pkt : remotePackets) {
        ProcessRemoteInput(pkt.frameId, pkt.input); // ここでRollbackが必要か判定
    }

    if (NeedsRollback()) {
        int rFrames = GetRollbackFrames();

        // 2. [描画スキップ] 再計算中のCPU負荷下落を防ぐ (解決策A)
        GameHooks::SetSkipFrames(true);
        
        // 3. メモリの復元
        LoadState(rollbackPoint);
        
        // 4. [リプレイ補正] アドレス手動補正 (解決策C)
        GameHooks::FixReplayPointers(rFrames);

        // 5. ロールアップ実行ループ (再計算中は音を完全にミュート)
        AudioFilter::EnableMute();
        for (int f = rollbackPoint; f < _currentFrame; ++f) {
            AdvanceFrame(...);
        }
        AudioFilter::DisableMute();

        // 6. [描画再開] 
        GameHooks::SetSkipFrames(false);
    }
    
    // ... 現在のフレームの通常処理 ...
}
```

## 3. 次のステップ

この計画に沿って実装を開始する場合、依存関係の根元に近い以下の順番で進めるのが最も安全です。

1. **Phase A:** `AudioFilter` の新規作成と、`GameHooks` へのリプレイ・RNG操作用インターフェースの追加（メモリハックの基礎部分の移植）。
2. **Phase B:** `UdpPacketQueue` の作成とパケット受信スレッドとの結合（マルチスレッドアクセスの安全保障）。
3. **Phase C:** 最後に `RollbackEngine.cpp` の内部ロジックをリファクタリングし、上記モジュールを組み込んで `DoRollback` パイプラインを完成させる。
