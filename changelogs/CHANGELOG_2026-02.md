# refactor: FastBootMonitor の責務分離と GameHooks への機能移譲

## 2026-02-28: ランチャー側のパッチ・メニュー進行ロジックを core_dll 側に移行

### 変更内容
- [MODIFY] `FastBootMonitor.hpp/.cpp`: ランチャーの役割をゲーム起動処理と DLL インジェクト、およびエントリポイントロックを通じたスレッド同期に限定。長大なメニュー監視ループやメモリパッチ処理は削除し、単一責任の原則を適用した。
- [MODIFY] `GameHooks.cpp`: 従来の `FastBootMonitor` が行っていた高速ブート向けの各種パッチ（非アクティブ判定無効化等）や、キャラクターセレクト画面への自動メニュー遷移と入力偽装（DirectMemoryWrite）のロジックを引き継ぎ、DLL 側で独立スレッドから実行するように統合。
- [MODIFY] `MainController.hpp/.cpp` および `IpcData.hpp`: CLI 統合時の柔軟なモード制御（VersusCPU, Replay 等の追加）に対応できるよう、`SharedState` (IPC) を経由して対象ゲームモードを DLL 側に伝達する仕組みを整備。
- [MODIFY] `MbaaConstants.hpp`: FastBoot・パッチ実行に必要な共有アドレス (`CC_AUTO_ACTIVATE_ADDR`, `CC_FORCE_GOTO_ADDR`) を定義に追加。

---

# refactor: DxHook 責務分離 — ドメインロジックを GameFrameOrchestrator に移設

## 2026-02-28: DxHook の純粋インフラ化

### 変更内容
- DxHook.cpp からドメイン固有ロジック（SceneRunner::Step, DirectInputHook::Poll, GameMode→UiPhase変換, UIManager::Render, VirtualClock直接呼び出し, Sleep(16)）を完全除去
- コールバック方式（FrameCallback / SkipModeCallback 生の関数ポインタ）で外部から注入する設計に変更
- [NEW] `GameFrameOrchestrator.hpp/.cpp` — 分離したドメインロジックの統合先
- [MODIFY] `dllmain.cpp` — InitThread末尾でコールバック登録（autoTestModeガード付き）
- 10項目のリスク分析に基づく対策を適用（R-01～R-10）

---

# test: present_hunterのログを相対パス化しv24へ更新

## 2026-02-28: EndScene多重呼び出し検証用のテストDLL更新

### 変更内容
- 	ests/lab/present_hunter.cpp のログ出力パスを絶対パスから相対パス(present_hunter_log.txt)に変更し、デッドロックを回避
- 出力バージョンを 24 へ繰り上げ

---

# fix: domain_UIリネームに伴うテストコード等のパス修正漏れに対する追従

## 2026-02-28: ControllerMapper等の分割リファクタリングに伴うテスト修正

### 変更内容
- src/tests/* 以下のインクルードパスに domain_overlay が残っていたのを domain_UI へ修正
- ControllerMapper::Draw の引数が不要になった変更に伴い、NetplayOverlay.cpp と stub_controller_mapper.cpp の呼び出しを修正

---

# refactor: DxHook.cppのUI描画およびStep実行処理をPresentフック内のスリープ前へ完全移行

## 2026-02-28: Hooked_Present への描画とStepロジックの集約

### 変更内容
- Hooked_EndScene の多重呼び出し(毎フレーム数十回)による不完全なバックバッファ上での描画やロジック実行を廃止。
- Hooked_Present のスリープ（Adaptive_FrameSleep）実行直前に、UI（ImGui）の独自描画サイクルと SceneRunner::Step() の実行を移動。
- これにより入力遅延とジッターが最小化され、描画ステートの完全性とアプリケーションの安定性が向上した。

---

# fix: F4マッピングUI表示中・操作中の不明なクラッシュを修正

## 2026-02-28: DirectInputHook と SceneRunner の実行制御修正

### 根本原因
1. **SceneRunner::Step() の過剰実行**:
   MBAA は「スプライト1枚引くたびに EndScene を呼ぶ」というレガシーな描画構造を持っており、1フレーム（16.6ms）の間に EndScene が複数回（約8回）呼ばれ、BackBuffer 判定もすべて通過していた。
   このため、`SceneRunner::Step()` が1フレームにつき8回も実行され、通信送信やゲームのフレーム更新を過剰に回してしまい、内部状態の破壊やオーバーフロー、ソケットの輻輳を引き起こしていた。
2. **DirectInputHook の COM 再入クラッシュ**:
   `DirectInputHook::Poll()` の内部で `GetDeviceState` などを呼ぶ際、Windows/DirectX 内の COM メッセージポンプが動作し、USB切断などの `WM_DEVICECHANGE` が同期的に割り込む（再入・Re-entrancy）可能性があった。
   この際 `RefreshDevices()` が呼ばれて `g_Controllers` (vector) がクリアされると、実行中の `Poll` のループ用イテレータが無効化されてアクセス違反（クラッシュ）を引き起こしていた。

### 修正内容
- **DxHook.cpp**:
  `EndScene` 内での `SceneRunner::Step()` 実行条件に「前回の実行から 12ms (12000us) 以上経過していること」を追加し、1フレームにつき**必ず先頭の1回だけ**が呼ばれるように厳密に制御した。
- **DirectInputHook.cpp**:
  `Poll()` 実行中は `g_isPolling` フラグを立て、その間に `RefreshDevices()` が割り込んできた場合は即座に配列をクリアせず要求を保留 (`g_refreshPending`) とし、`Poll()` 完了直後に安全にリフレッシュを処理するように変更した。

---

# fix: スピードハックフックによるImGuiのDeltaTime異常増加を修正

## 2026-02-28: Hooked_Present内でのDeltaTime上書き

### 根本原因
- `ImGui_ImplWin32_NewFrame` 内部で `QueryPerformanceCounter` が呼ばれるが、これは CCCaster のスピードハック処理によりプロセス全体でフックされている。
- 早送り/スキップ中（1000倍速など）は、実際には 0.016秒 しか経過していなくても、ImGuiの計算上は `DeltaTime = 16.6秒` となってしまう。
- その結果、アニメーション等の進行が暴走し、UIの高速点滅や一部計算でのゼロ除算クラッシュが発生していた。

### 修正内容
- `DxHook::Hooked_Present` 内の `ImGui_ImplWin32_NewFrame();` 呼び出し直後に、`VirtualClock::QPCNowUs()`（フックされていない生のカウンタ）を用いて現実時間の経過を計算し、`ImGui::GetIO().DeltaTime` を手動で上書きするようにした。

---

# fix: F4 キーリピートによるマッピングUI高速トグル修正

## 2026-02-28: WM_KEYDOWN のリピート除外

### 根本原因
- Windows の `WM_KEYDOWN` はキー押しっぱなしでリピート発火する
- F4 を押すたびに `OnMappingInput()` が高速で Open/Close を繰り返し → 高速点滅 → クラッシュ

### 修正内容
- `lParam & (1 << 30)` でキーリピート判定、リピート時はホットキー処理をスキップ
- マッピング中はリピートもゲームにブロック

---

# fix: EndScene 多重描画防止 + UIManager try-catch 保護

## 2026-02-28: DxHook タイムスタンプガード

### 修正内容
- **EndScene 多重描画防止**: タイムスタンプベースで 1ms 未満の連続呼出しをスキップ
- **UIManager::Render try-catch**: UI描画中の例外をキャッチして EndScene パイプラインを保護

---

# fix: マッピング中クラッシュ・高速点滅の根本修正

## 2026-02-28: InputHook マッピング中キーブロック

### 修正内容
- **根本原因**: マッピング中の Left/Right キーがゲームにも渡されていた → キャラセレ画面でゲーム操作が高速実行 → 画面遷移＋クラッシュ
- **修正**: `InputHook.cpp` で `IsMappingWindowOpen()` 時に WM_KEYDOWN/WM_KEYUP を全てゲームにブロック
- **追加**: `UIManager::IsMappingWindowOpen()` メソッド追加

---

# fix: バインド未反映 + クラッシュ対策

## 2026-02-28: SaveBinds 直後に ReloadConfigs 追加 + CheckInputBind 例外保護

### 修正内容
- **バインド未反映**: `SaveBinds()` 完了直後に `DirectInputHook::ReloadConfigs()` を呼出し（キーボード・ジョイスティック両パス）
- **クラッシュ対策**: `CheckInputBind()` の `std::stoi` を `try-catch` で保護（不正バインド文字列への防御）

---

# fix: マッピングUI 全バインド・高速点滅バグ修正

## 2026-02-28: IsKeyPressed リピート無効化 + バインド開始ガード

### 修正内容
- **全 `IsKeyPressed()` 呼び出しに `repeat=false` を追加** — ImGui デフォルトのキーリピートが原因でバインドが高速連打されていた
- **バインド開始フレームで `ProcessBindingInput` をスキップ** — `startBinding` と同フレームで入力処理されるのを防止
- **バインド開始条件に `pos==0` チェック追加** — 既にバインド中の場合に再開始しない

---

# refactor: ControllerMapper View/Logic 完全分離

## 2026-02-28: Controller_Ui_Logic / Controller_Ui_View 分離

### 変更内容
- [NEW] `Controller_Ui_Logic.hpp/.cpp` — 状態管理・入力処理・保存ロジック
- [MODIFY] `Controller_Ui_View.cpp` — 描画専用に全面書き直し
- [REMOVE from build] `ControllerMapper.cpp` — Logic + View に分解
- CMakeLists.txt 更新

---

# fix: ControllerMapper 不安要素6件修正

## 2026-02-28: ControllerMapper リファクタリング

### 修正内容
- **#7** 未使用 `MappingState` enum 削除
- **#3** サニタイズ重複 → `SanitizeDeviceName()` + `GetDeviceNameById()` ヘルパー抽出
- **#6** バインド開始重複 → `startBinding()` ラムダで共通化
- **#8** `GetAnyInputEdge` 二重呼出し → Edge キャッシュで1回のみ取得
- **#10** 2つの閉じ経路 → ImGui クローズボタン無効化、F4 のみに統一
- **#5** `showMappingWindow` 参照渡し → 廃止、`Draw()` 引数なし化

---

# refactor: 画面別UIシステム (View/Logic 分離)

## 2026-02-28: domain_UI 画面別UI設計 + 実装

### 変更内容

#### 新規ファイル (12ファイル)
- `UIManager.hpp/.cpp` — 画面切替エントリポイント (UiPhase enum)
- `State_Ui_Logic.hpp/.cpp` — 共有状態データ (D/R/FPS/RTT/Jitter)
- `State_Ui_View.hpp/.cpp` — 常時ステータスバー描画
- `CharaSelect_Ui_View.hpp/.cpp` — キャラセレ画面
- `InGame_Ui_View.hpp/.cpp` — 対戦画面 (FPS/RTT/ずれ表示)
- `Rematch_Ui_View.hpp/.cpp` — 再戦画面 (プレースホルダ)
- `Controller_Ui_View.hpp/.cpp` — F4マッピング画面ラッパー

#### 変更ファイル
- `DxHook.cpp` — GameMode→UiPhase 変換、UIManager::Render()
- `InputHook.cpp` — NetplayOverlay→UIManager 全置換
- `CMakeLists.txt` — ソースファイル追加

#### 削除予定
- `NetplayOverlay.hpp/.cpp` — UIManager + State + Views に分解済み

---

# refactor: domain_overlay を domain_UI にリネーム

## 2026-02-28: オーバーレイフォルダリネーム

### 変更内容
- `domain_overlay/` → `domain_UI/` にフォルダリネーム
- 全 include パス更新 (NetplayOverlay.cpp, ControllerMapper.cpp, DxHook.cpp, InputHook.cpp)
- CMakeLists.txt 更新 (core_dll, tests)

---

# refactor: SceneRunner をゲームスレッドに統合 + フレーム制御修正

## 2026-02-28: フレーム制御アーキテクチャ修正

### 根本問題
SceneRunner が別スレッドで実行されていたため、ゲームメモリの読み書きに
スレッド競合が発生し、60fps 制御が機能しなかった。

### 変更内容

#### SceneRunner Init/Step 分割
- `Run()` → `Init()` / `Step()` / `IsReady()` に分割
- `Step()` はゲームスレッド (EndScene) から毎フレーム呼ばれる
- `SleepFrame()` と DEBUG `Sleep(16)` を除去

#### DxHook フレーム制御統合
- `Hooked_Present`: `Adaptive_FrameSleep()` で 60fps 制御
- `Hooked_EndScene`: `SceneRunner::Step()` でフレームロジック実行

#### PauseFlag 直書き廃止
- `CC_PAUSE_FLAG_ADDR` 直書き → `SetGamePause()` / `SetGameResume()` に集約

#### WaitForNextFrame リネーム
- `WaitForNextFrame()` → `Adaptive_FrameSleep()` に全箇所リネーム

#### SessionContext 永続化
- `dllmain.cpp` の `SessionContext ctx` を `static` に変更

### 変更ファイル
- `SceneRunner.hpp` / `.cpp`, `DxHook.cpp`, `dllmain.cpp`
- `MbaaSpeedController.hpp`, `VirtualClock.hpp` / `.cpp`
- `GameControl.hpp`, `RollbackEngine.cpp`

---

# fix(TimeSynchronizer,RollbackEngine,SceneRunner): 全QPC呼び出しをRealQPCに統一 (U-09)

## 2026-02-27

### Bug Fix — U-09: TimeHooks 1000倍速 TimeSync干渉
- **TimeSynchronizer.cpp: GetLocalTimeUs() QPC差し替え漏れ修正**
  - `GetLocalTimeUs()` のQPCフォールバックパスで `QueryPerformanceCounter()` を直接呼んでいたため、
    `TimeHooks::SetTimeMultiplier(1000)` 適用時にフック済みの1000倍速時間を取得していた
  - `TimeHooks::RealQueryPerformanceCounter()` に差し替え
  - **影響**: SYNC_REQ送信間隔、SYNC_TIMEOUT判定、θ計測タイムスタンプ(T1/T4)

- **RollbackEngine.cpp: QPCNowUs() QPC差し替え漏れ修正**
  - ロールバック処理時間計測 (`lastRollupTimeUs`) が超倍速環境で1000倍に水増しされていた
  - `TimeHooks::RealQueryPerformanceCounter()` に差し替え

- **SceneRunner.cpp: AutoTestログ QPC差し替え漏れ修正**
  - AutoTestモードのログ経過時間 (`t=XXXms`) が超倍速環境で1000倍速の値になっていた
  - `TimeHooks::RealQueryPerformanceCounter()` に差し替え

### 改善
- **SceneRunner.cpp: 文字化けコメント修正**
  - Shift_JIS/UTF-8エンコーディング不整合で文字化けしていた全コメントを正しいUTF-8日本語に修正

### テスト
- `.ai_workspace/test_timesync_qpc/test_main.cpp` に検証テスト8件を追加
  - RealQPC単調増加、フック未適用時の一致、超倍速加速検証
  - GetLocalTimeUs BUG版 vs 修正版比較
  - PING_INTERVAL経過判定の干渉、RealQPCの加速免疫
  - SYNC_TIMEOUT誤判定の再現、θ計測RTT汚染確認
  - 全8件 PASS



