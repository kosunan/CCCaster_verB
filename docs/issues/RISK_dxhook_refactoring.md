# DxHook.cpp リファクタリング — 不安要素10項目 & 対策

> [!IMPORTANT]
> 各リスクに **深刻度 (S/A/B/C)** と **対策の難易度** を付記。S が最も危険。

---

## R-01: 初期化順序の暗黙依存 【深刻度 S】

### 問題
現在の初期化シーケンスは **3スレッド × 暗黙の時間依存** で成り立っている：

```
[OS] DllMain → CreateThread(InitThread)
[InitThread]  Sleep(100) → GameHooks.Init → DxHook.Init → SceneRunner.Init
[ゲームスレッド] EndScene → (初回) ImGui Init + InputHook Init + DirectInputHook Init
                          → (初回) Present 動的フック
```

コールバック方式に変更すると、`GameFrameOrchestrator::Register()` をどのタイミングで呼ぶかが**極めてクリティカル**になる。
- `DxHook::Initialize()` の直後だと、まだ `SceneRunner::Init()` が完了していない
- `SceneRunner::Init()` の後だと、既に EndScene が数回呼ばれている可能性がある

### 対策
- `Register()` は `dllmain.cpp` の `InitThread` 末尾（`SceneRunner::Init()` 直後）で呼ぶ
- DxHook 側はコールバックが `nullptr` なら無操作（null チェック）
- EndScene 内のコールバック呼び出しを `if (onFrameLogic)` でガードし、Register前の空振りを安全に処理

---

## R-02: EndScene 内の Sleep(16) と Present 内の Adaptive_FrameSleep の二重制御 【深刻度 S】

### 問題
現在 **2箇所** でフレームレート制御が行われている：
1. `Hooked_Present` (L184): `Adaptive_FrameSleep()` — VirtualClock ベースの精密60fps制御
2. `Hooked_EndScene` (L329): `Sleep(16)` — ハードコード16msスリープ

MBAAは1フレームに EndScene を**複数回**（~8回）呼ぶため、`Sleep(16)` が8回 = **128ms/frame** = 約8fps に低下する可能性がある。これはスピードハック時の暴走抑制として入れた可能性があるが、`Adaptive_FrameSleep` と責務が衝突している。

### 対策
- リファクタリング前に `Sleep(16)` を削除した状態で**動作テスト**を行い、副作用を確認
- 問題がなければ削除、問題があれば「バックバッファ EndScene のみ」に限定する条件を付ける
- この判断はリファクタリングの**前提条件**として先に解決すべき

---

## R-03: Present フックの動的設置タイミング 【深刻度 A】

### 問題
Present フックは EndScene の中で**動的に**設置される（L207-223）：
```cpp
if (!isPresentHooked) {
    void** gameDeviceVtable = *reinterpret_cast<void***>(pDevice);
    void* gamePresentAddr = gameDeviceVtable[17];
    MH_CreateHook(gamePresentAddr, ...);
}
```
この「EndScene 内で Present をフックする」パターンは、ダミーデバイスの vtable ではなく**ゲーム実デバイス**の vtable を使う必要があるため。リファクタリング時にこのロジックを移動すると、pDevice が取得できないコンテキストに出てしまう。

### 対策
- Present 動的フック設置は**DxHook.cpp 内に残す**（infra の責務）
- コールバック化するのは Present フック内の `VirtualClock` 呼び出しのみ
- 「何をフックするか」はインフラ、「フック内で何をするか」がドメイン、と明確に分ける

---

## R-04: std::function コールバックのパフォーマンス 【深刻度 B】

### 問題
`std::function` は内部で仮想呼び出し + ヒープアロケーション（小さいラムダはSBO最適化）を行う。EndScene は**毎フレーム複数回**呼ばれるため、呼び出しオーバーヘッドが懸念される。

### 対策
- `std::function` の代わりに**生の関数ポインタ** (`void(*)()`) を使用
- ラムダキャプチャ不要（static 関数を登録するだけ）なので関数ポインタで十分
- 仮想関数テーブル経由のコールバックは不要 → オーバーヘッドはほぼゼロ

---

## R-05: バックバッファ判定とロジック実行の結合 【深刻度 A】

### 問題
現在、`SceneRunner::Step()` と `DirectInputHook::Poll()` は **バックバッファ判定が true の場合のみ** 実行される（L275-330 の `if (isBackBuffer)` ブロック内）。MBAAは EndScene を多重呼び出しするため、この判定は「1Fにつき1回だけロジックを実行する」ためのガードとして機能している。

コールバック化の際、この「バックバッファ時のみ」という実行条件を `GameFrameOrchestrator` 側に移動すると、オーケストレータがバックバッファ判定の知識を持つことになり、責務が曖昧になる。

### 対策
- DxHook がバックバッファ判定を行い、true の場合のみコールバックを呼ぶ
- コールバックは「呼ばれたら1F分の処理を実行する」だけ
- **判定はインフラ、実行はドメイン** の原則を維持

---

## R-06: ImGui コンテキストのスレッド安全性 【深刻度 A】

### 問題
ImGui はスレッドセーフではない。現在は全てゲームスレッド（EndScene コールバック内）で実行されているため問題ないが、コールバック化によって将来的に別スレッドから呼ばれるリスクが生まれる。特に `UIManager::Render()` が ImGui API を直接使用しているため、呼び出し元のスレッドが変わると即クラッシュする。

### 対策
- `GameFrameOrchestrator` のドキュメントに「ゲームスレッドからのみ呼ばれることを保証」と明記
- DxHook.hpp のコールバック登録メソッドに `@note ゲームスレッド(EndScene)から呼ばれる。スレッドセーフではない。` コメントを付与
- 将来の安全弁として、デバッグビルドでスレッド ID アサーションを入れる:
  ```cpp
  assert(GetCurrentThreadId() == g_gameThreadId);
  ```

---

## R-07: テストコード (test_overlay.cpp) との互換性 【深刻度 B】

### 問題
既存の `test_overlay.cpp` は `NetplayOverlay` クラスの private メンバに `friend` 経由でアクセスするテスト設計。DxHook 自体を触るテストは存在しないが、今回新設する `GameFrameOrchestrator` が `UIManager` 経由で `NetplayOverlay` を呼ぶため、テストのコンパイルパスが変化する可能性がある。

また、test_overlay は `DxHook.hpp` を `#include` **していない**ため直接の影響はないが、CMakeLists.txt の変更でリンクエラーが発生する可能性がある。

### 対策
- リファクタリング後に `test_overlay` のビルド＆実行を必ず確認
- `GameFrameOrchestrator` は `test_overlay` のリンク対象に含めない（テスト不要：ゲームスレッド内でしか動かない）
- テスト互換性を検証ステップの必須チェック項目に追加

---

## R-08: GameMode メモリアドレスの管理場所散逸 【深刻度 B】

### 問題
`GAME_MODE_ADDRESS = 0x54EEE8` は現在 DxHook.cpp にハードコードされている。これを `GameFrameOrchestrator` に移動すると、MBAA メモリアドレスが `domain_memory/MbaaConstants.hpp` と `domain_session/GameFrameOrchestrator.cpp` の **2箇所** に散逸するリスクがある。

### 対策
- `MbaaConstants.hpp` に `GAME_MODE_ADDRESS` を定義し、`GameFrameOrchestrator` はそこから参照
- DxHook.cpp からは完全に除去
- メモリアドレスの一元管理ポリシーを文書化（`memory_and_hook_design.md` のアドレス表に追記）

---

## R-09: Shutdown 時のコールバック解除とリソースリーク 【深刻度 B】

### 問題
`DxHook::Shutdown()` は `DLL_PROCESS_DETACH` 時に呼ばれる。コールバック方式導入後、Shutdown 時にコールバックポインタをクリアしないと、既に破棄された `GameFrameOrchestrator` のコードを指すダングリングポインタが残る。`DLL_PROCESS_DETACH` の順序は Windows が制御するため、EndScene が Shutdown 前に呼ばれてダングリングコールバックを実行する可能性がある。

### 対策
- `DxHook::Shutdown()` でコールバックを `nullptr` にクリア
- コールバック呼び出し前に `nullptr` チェック（R-01 と同じガード）
- Shutdown のコールバッククリアを EndScene/Present のフック解除**より前**に実行

---

## R-10: 将来のヘッドレスモード対応 【深刻度 C】

### 問題
ヘッドレスモード（`autoTestMode`）では D3D9 デバイスが存在しない環境で動作する可能性がある。現在は DxHook が全く初期化されないため問題ないが、コールバック方式にすると「DxHook が初期化されない → コールバックが登録されない → SceneRunner::Step() が永遠に呼ばれない」という経路が生まれる。

### 対策
- ヘッドレスモードの場合は `GameFrameOrchestrator::Register()` を**呼ばない**
- ヘッドレスモードには別の Step() 呼び出し経路（現状: `SceneRunner::Init()` 内部のタイマーループ?）を維持
- dllmain.cpp の InitThread に `if (!ctx.autoTestMode) GameFrameOrchestrator::Register()` ガードを追加

---

## 対策サマリ表

| # | リスク | 深刻度 | 対策の核心 | 実装難易度 |
|---|---|---|---|---|
| R-01 | 初期化順序 | **S** | null チェック + InitThread 末尾で Register | 低 |
| R-02 | Sleep(16) 二重制御 | **S** | 事前テスト → 削除判断（リファクタ前に解決） | 中 |
| R-03 | Present 動的フック | A | DxHook 内に残す | 低 |
| R-04 | std::function 性能 | B | 生の関数ポインタを使用 | 低 |
| R-05 | バックバッファ判定の責務 | A | DxHook が判定、コールバックは実行のみ | 低 |
| R-06 | ImGui スレッド安全性 | A | ドキュメント + デバッグアサート | 低 |
| R-07 | テスト互換性 | B | リファクタ後にビルド＆実行確認 | 低 |
| R-08 | GameMode アドレス散逸 | B | MbaaConstants.hpp に一元化 | 低 |
| R-09 | Shutdown ダングリング | B | コールバック nullptr クリア | 低 |
| R-10 | ヘッドレスモード | C | autoTestMode ガード | 低 |

> [!TIP]
> **推奨実行順序**: R-02（Sleep(16)問題の事前解決）→ R-08（アドレス一元化）→ 本体リファクタリング → R-07（テスト検証）
