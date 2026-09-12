# refactor: DLL側CB干渉処理の全面削除 — 入力パイプラインのリセット

## 2026-03-12: SceneRunner / MatchScene から CB 操作を完全撤去

### 概要
SceneRunner.cpp と MatchScene.cpp に蓄積していた CB（MenuInputBuffer / MatchInputBuffer）への
全干渉処理を削除し、入力パイプラインをゼロベースからの再構築に備えるリセットを実施。
通信スレッド側（NetplaySession / SyncCodec）および UIManager は変更なし。

### 削除内容（計 17箇所）

#### SceneRunner.cpp（12箇所）
- `OnPhaseChanged` 内の `MenuInputBuffer::Reset()` ×2, `MatchInputBuffer::Reset()` ×1
- メトロノーム gap 計算の `GetWriteHead()` ×2 → CB非依存に簡素化（常にメトロノーム待機）
- セクション(E) 丸ごと：入力ポーリング + CB書込み（`DirectInputHook::Poll`, `WriteSlot`, `SetWriteHead`）
- `s_introStarted` ゲートフラグ（変数定義 + Init内初期化 + フェーズ遷移リセット + intro検出ロジック）
- セクション(G) 丸ごと：相手入力待機ブロッキングループ + `ReadFrameForGame` + `WriteInput`
- 統合フローログから CB 変数参照（`wh`, `rp`, `ef`, `crf`）を除去
- 不要 include 削除: `MenuInputBuffer.hpp`, `MatchInputBuffer.hpp`, `DirectInputHook.hpp`

#### MatchScene.cpp（5箇所）
- `ReadBufferAndWrite()` 関数全体の削除（CB→SceneInputFilter→WriteInput パイプライン）
- `OnCharaSelect`, `OnLoading`, `OnInGame` 内の `ReadBufferAndWrite()` 呼出し削除
- `HandleRoundStartSync` 内の `MatchInputBuffer::GetWriteHead()` → `phaseBaseFrame=0` に
- `OnInGame` 内の introState/noInputFlag ゲートチェックを削除
- 不要 include 削除: `SceneInputFilter.hpp`, `MenuInputBuffer.hpp`, `MatchInputBuffer.hpp`, `GamePhaseDetector.hpp`

### 変更ファイル
- [MODIFY] `engine/SceneRunner.cpp`: CB干渉12箇所を全削除、needKeepalive=true固定
- [MODIFY] `engine/MatchScene.cpp`: CB干渉5箇所を全削除、各OnXxxは空関数orフラグ管理のみ

---

# fix: InGame突入時の同期デッドロック完全解消および冗長入力実装によるパケットロス耐性強化

## 2026-03-11: netplay同期の安定化（パケットロス20%・遅延90ms環境下の完全動作）

### 概要
1. **同期デッドロックの解消**:
   - `SceneRunner` の相手入力待ちブロックによる進行停止中、キープアライブパケット（通信の維持）が対象フレームやフラグが0の不正な状態で送信され、相手に無視されていた重大なバグを修正。
   - `NetplaySession::ThreadMain` のダミー送信処理を改修し、現在の最新の入力ヘッド（Menu/Match）と対応する `SyncCodec::FLAG_BUFFER_*` を正しく指定するよう修正した結果、InGame開始時にフリーズする問題が完全に解消。
2. **冗長入力 (Redundant Inputs) の実装**:
   - 通信パケット (`SyncPayload`) のペイロード末尾に、直近最大10フレーム分の入力履歴を同封（`inputs[10]`）。
   - 受信側 (`SyncCodec::ProcessReceivedPacket`) にて、同封された過去のフレームに対する `ConfirmRemote` を個別に実行する補完ロジックを追加。大幅なパケットロス環境でも後続のパケットで素早く状態が復元される仕組みを確立。

### 変更ファイル
- [MODIFY] `core_dll/network/SyncCodec.cpp`: パケット組立て時に過去10Fを取得してアペンド。受信時にループで展開して複数回の `ConfirmRemote` を呼び出し。
- [MODIFY] `core_dll/sync/NetplaySession.cpp`: `_state.needKeepalive` 起因で送信されるパケットに対し、正しい CB の最新読取位置と対象フラグをセット。

---

# feat: CB分割 (Menu/Match) とWT基準の相対フレーム同期実装

## 2026-03-11: FrameInputBufferの責務分割とパケットルーティングの追加

### 概要
1. 単一の `FrameInputBuffer` を、UI入力用の `MenuInputBuffer`（ロールバックなし）と対戦入力用の `MatchInputBuffer`（ロールバックおよびWT基準フレーム対応）に分割。
2. 同期におけるフレーム計算の基準を、絶対WTから「各フェーズ開始時のWT（`phaseBaseWorldTimer`）を0フレーム目とした相対WTフレーム」へ変更。
3. `SyncCodec` で送受信するパケットに対し、Menu用かMatch用かを示す識別フラグ (`bufferTargetFlag`) を付与し、受信時に対応するバッファへ正確にルーティングする機構を追加。
4. `SceneRunner` のフェーズ遷移時に、前画面のバッファを初期化し過去の入力による汚染を防ぐ処理を追加。

### 変更ファイル
- [NEW] `core_dll/sync/MenuInputBuffer.hpp`
- [NEW] `core_dll/sync/MatchInputBuffer.hpp`
- [DELETE] `core_dll/sync/FrameInputBuffer.hpp`
- [MODIFY] `core_dll/engine/MatchContext.hpp`: `phaseBaseWorldTimer` 追加
- [MODIFY] `core_dll/network/SyncCodec.hpp` & `.cpp`: パケットへのフラグ付与、バッファ振り分けでの `ConfirmRemote` 呼び出し
- [MODIFY] `core_dll/sync/NetplaySession.hpp` & `.cpp`: 両バッファの監視・送信に対応、初期化処理追加
- [MODIFY] `core_dll/ui/UIManager.cpp` & `core_dll/engine/MatchScene.cpp` & `core_dll/engine/SceneRunner.cpp`: `FrameInputBuffer` 参照を新しい2つのバッファに置換し、場面ごとに読み書き先を切り替え

---

# docs: engine/ フォルダのコメントを実装と整合

## fix: メトロノーム待機のフェーズ遅延判定追加と、InGame CB書き込みのintroState条件厳格化

### 2026-03-10: 相手のフェーズ先行時の待機スキップと、introState(1,2)明示的チェック

### 概要
1. 自分のフェーズ到達が相手より遅れている場合（`!localPhaseReady && peerPhaseReady`）、メトロノーム待機をスキップして高速に相手に追いつかせるロジックを追加。
2. `FrameInputBuffer` への書き込み条件を修正し、`InGame` フェーズでは `introState` が `1` または `2` の場合のみ書き込みを許可するよう厳格化。

### 変更ファイル
- [MODIFY] `engine/SceneRunner.cpp`:
  - `(D) メトロノーム精密待機` で `syncState.peerPhaseReady` と `syncState.localPhaseReady` を比較する `isPhaseDelayed` 判定を追加。
  - `(E) CB書込み` で `introNow` が `1` または `2` であることを明示的にチェックするロジックに変更。

---## 2026-03-10: 廃止済み参照の削除 + 実装乖離の修正

### 修正ファイル (6ファイル)
- `SceneRunner.cpp`: Step() 処理フロー (A)-(I) を現在の実装に合わせて書き換え (SleepFrame → メトロノーム等)
- `FrameControl.hpp`: 3層→2層構成、SleepFrame 使用例削除、Pause/Rollup 説明改善
- `MatchScene.hpp`: 各画面業務を実装と一致 (Loading: CB書込みなし / Rematch: DirectInputHook 直接)
- `MatchContext.hpp`: Run()→Step()、UdpSocket→FrameInputBuffer
- `SceneRunner.hpp`: SendFunc をレガシー表記
- `SceneInputFilter.hpp`: 「後日詳細実装」→ 現在の状況記載

---

# refactor: WndProcHook の UI 処理を UIManager に分離

## 2026-03-10: hook 層とロジック層の責務分離

### 変更
- [MODIFY] `hook/WndProcHook.cpp`: フック設置・復元のみの薄い層に簡素化 (139行→50行)
- [MODIFY] `ui/UIManager.hpp`: `HandleWndProcMessage` 宣言追加 + `windows.h` include
- [MODIFY] `ui/UIManager.cpp`: WndProc UI ロジックを `HandleWndProcMessage` に集約
  - ImGui ハンドラ, ホットキー(F4/Ctrl+数字/Alt+数字), マッピング制御, デバイスホットプラグ, WantCapture

---

# refactor: core_dll フォルダ再編成 — hook/ + mbaa_mem/ 新設

## 2026-03-10: hook系コードとMBAAメモリ系コードの分離

### 新設フォルダ
- **hook/** — 外部APIフック (DxHook, TimeHooks, DirectInputHook, WndProcHook)
- **mbaa_mem/** — MBAAメモリ操作・検出 (旧 inject/ + detect/ 統合)

### 移動ファイル (19ファイル)
- `timing/` → `hook/`: DxHook.cpp/hpp, TimeHooks.cpp/hpp
- `input/` → `hook/`: DirectInputHook.cpp/hpp, WndProcHook.cpp/hpp
- `inject/` → `mbaa_mem/`: dllmain.cpp, MbaaPatcher.cpp/hpp, MemoryPatcher.hpp
- `detect/` → `mbaa_mem/`: MbaaAddresses.hpp, MbaaInputDefs.hpp, GamePhase.hpp, GamePhaseDetector.hpp, PhaseMonitor.cpp, FastBootRunner.cpp/hpp
- `input/` → `engine/`: SceneInputFilter.cpp/hpp

### 廃止フォルダ
- `inject/`, `input/`, `detect/` → 空フォルダ化

### 関連更新
- 24ファイルの `#include` パス更新
- CMakeLists.txt 更新

---

# feat: Rematch メニュー選択を GAME_TICK パケットに統合

## 2026-03-10: retryMenuIndex フィールド追加

### 変更ファイル
- [MODIFY] `network/GameTickCodec.cpp`:
  - `GameTickPayload` に `int8_t retryMenuIndex` 追加 (-1=未決定, 0-2=選択済み)
  - `BuildGameTickPacket`: SharedSyncState.localRetryMenuIndex を読取りパケットに設定
  - `ProcessReceivedPacket`: 受信した retryMenuIndex を `MatchScene::SetRemoteRetryMenuIndex` に転送
  - `MatchScene.hpp` include 追加
- [MODIFY] `sync/NetplaySession.hpp`:
  - `SharedSyncState` に `localRetryMenuIndex` (atomic<int8_t>) 追加
- [MODIFY] `engine/MatchScene.cpp`:
  - ローカル選択検出時に SharedSyncState.localRetryMenuIndex も更新
  - `ResetRematch` で SharedSyncState.localRetryMenuIndex を -1 にリセット

### テスト結果 (dual_test.bat)
- fip=2400+ まで安定動作
- alive=1 / RTT=110-123μs 安定

---

# fix: 相手入力未到着時にゲーム進行をブロックする待機機構を追加

## 2026-03-10: readPos > crf で相手入力到着までスピン待機

### 問題
ReadFrameForGame で confirmed=false のフレームはゲームメモリに書込まれず、
HOST 側で自分の操作が反映されない（相手には送信される）非対称な挙動が発生。

### 変更ファイル
- [MODIFY] `engine/SceneRunner.cpp`:
  - (G) セクションに crf ベースの待機ロジック追加
  - readPos > confirmedRemoteFrame の場合、crf が追いつくまで Sleep(0) スピン待機
  - 3秒タイムアウト付き（TIMEOUT 発生時はログ出力して続行）

### テスト結果 (dual_test.bat)
- TIMEOUT なし / Peer Disconnected なし
- HOST/CLIENT 両方で fip=2340+ まで安定動作

---

# refactor: メトロノーム駆動アーキテクチャ刷新 — SleepFrame 廃止 + ゲームスレッド精密待機

## 2026-03-09: 独立スレッド廃止 → WaitForNextTick + gap キャッチアップ

### 問題
1. SleepFrame(ef-WT gap) が effectiveHead 停滞時に無限ブロック → 画面フリーズ
2. メトロノーム独立スレッドがティック蓄積 → FastBoot中に ConsumeTicks 呼ばれず → wh 大ジャンプ
3. 3分割サブティック(SUB_TICKS_PER_FRAME) の残骸

### 変更ファイル
- [MODIFY] `timing/Metronome.hpp`:
  - 独立スレッド(ThreadMain, _pendingTicks, ConsumeTicks) 完全廃止
  - `WaitForNextTick(skipWait)` 追加: ゲームスレッドが直接精密待機
- [MODIFY] `timing/Metronome.cpp`:
  - ThreadMain 削除、Start() は _nextTickUs 初期化のみ、Stop() はフラグのみ
  - WaitForNextTick: α補正付き間隔で _nextTickUs を進め、SleepUntil で精密待機
- [MODIFY] `engine/FrameControl.hpp`:
  - SleepFrame() 削除 → SetRenderSkipByGap(gap) に置換
  - 未使用 include (FrameInputBuffer.hpp, MbaaAddresses.hpp) 除去
- [MODIFY] `engine/SceneRunner.cpp`:
  - Step() 根本刷新: gap判定 → WaitForNextTick(skipWait) → 入力取得 → CB 1F書込み
  - gap = peerLatestFrame - localWriteHead ≥ 2 → skipWait=true (キャッチアップ)
  - ConsumeTicks ループ廃止 → 毎 Step() で 1F だけ CB 書込み
  - FrameInputBuffer.hpp 直接 include 追加
- [MODIFY] `sync/NetplaySession.hpp`:
  - GetLatestPeerFrame() 委譲メソッド追加

### テスト結果 (dual_test.bat)
- fip=2400 まで安定動作（約40秒）
- wh-ef 差が一定（delay+RB 分のみ）、crf 正常追従
- 画面フリーズなし（SleepFrame 廃止で構造的に解消）
- alive=1 / RTT=130μs 前後安定

---

# fix: FastBoot中パケット途絶による Peer Disconnected を修正 + 3分割サブティック廃止

## 2026-03-09: keepalive 機構追加 + 通信スレッドループ刷新

### 問題
Counting 開始後の FastBoot 期間中、SceneRunner が CB に書込まないため通信スレッドがパケットを送信せず、
双方の疎通カウンタ(DISCONNECT_TIMEOUT_FRAMES=180) がタイムアウトして Peer Disconnected が発生。
また 3分割サブティック(SUB_TICKS_PER_FRAME=3, ≈5.5ms間隔) の残骸がループに残存していた。

### 変更ファイル
- [MODIFY] `sync/NetplaySession.hpp`:
  - `SharedSyncState` に `needKeepalive` フラグ追加（初期値 true）
  - `SUB_TICKS_PER_FRAME` 廃止 → `KEEPALIVE_INTERVAL_FRAMES=3` (≈50ms) に置換
  - `_keepaliveCounter` フィールド追加
- [MODIFY] `sync/NetplaySession.cpp`:
  - ThreadMain を1フレーム間隔(BASE_TICK_US ≈16.6ms) ループに刷新
  - Counting モード: CB writeHead 変化 → ゲームデータ送信（従来通り）
  - CB変化なし + needKeepalive=true → KEEPALIVE_INTERVAL_FRAMES ごとに keepalive 送信
  - subTickUs/subTickIdx 変数を完全削除
- [MODIFY] `engine/SceneRunner.cpp`:
  - CB書込み判定結果に応じて `needKeepalive` フラグを設定（shouldWrite=true→false, else→true）
- [MODIFY] `network/PacketRouter.cpp`:
  - 非CC10パケット(EXE SessionNegotiator残存)のログをサイレントドロップに変更
  - 未使用 DebugLog.hpp include 除去

### 副修正
- [MODIFY] `network/GameTickCodec.cpp`:
  - WriteFrameSlot() の `buf.CommitFrame()` → `buf.WriteSlot()` + `buf.SetWriteHead()` に修正
  - （前回のリファクタリングで CommitFrame 削除後のビルドエラー対応）

### テスト結果 (dual_test.bat)
- HOST/CLIENT 両方で `alive=1` 確認（修正前: `alive=0`）
- `Peer Disconnected` 解消
- `UNKNOWN packet` ログ解消
- CharaSelect 到達後 fip=540+ まで安定動作（RTT=130μs, α1=0, α2=0）
- ImGui + InputHook 初期化まで正常に進行

---

# refactor: CommitFrame 責務分離 — バッファからゲームロジックを除去 + InGame CB 書込みを intro 0→1 に同期

## 2026-03-09: FrameInputBuffer を純粋データ格納に、Phase/intro 判定を SceneRunner に移動

### 概要
FrameInputBuffer::CommitFrame(引数なし) がフェーズ判定・入力取得・rollbackable判定を内包していた問題を修正。
バッファは WriteSlot/SetWriteHead のみ提供し、「いつ・何を書くか」は SceneRunner が判断する設計に変更。
同時に InGame の CB 書込み開始を intro 0→1 遷移に揃え、両 peer 間の フレーム番号ゼロ点を一致させる。

### CB書込み対象（変更後）
- CharaSelect: 常時書込み
- InGame: intro 0→1 遷移後のみ（以前: InGame フェーズなら常時）
- Loading / Rematch / 他: 書込まない（Rematch は以前は書込んでいたが不要）

### 変更ファイル
- [MODIFY] `sync/FrameInputBuffer.hpp`: CommitFrame(引数なし) 削除、`_isHost` 削除、`GamePhaseDetector.hpp`/`DirectInputHook.hpp` include 削除、Initialize() から isHost 引数削除
- [MODIFY] `engine/SceneRunner.cpp`: Phase/intro 判定付き入力書込みロジック追加（s_introStarted ゲート）
- [MODIFY] `sync/NetplaySession.cpp`: Initialize() 呼出しから isHost 引数削除

---

# docs: sync/ コメントを実装に合わせて修正

## 2026-03-09: sync/ 配下4ファイルのコメント最適化

### 変更内容
- [MODIFY] `sync/FrameInputBuffer.hpp`: スレッド安全性（DLLスレッド/ioスレッド明記）、データフロー（ReadFrameForGame反映）、readPos算出式修正
- [MODIFY] `sync/NetplaySession.hpp`: "4層分離版" 削除、モード遷移にθ安定条件追加、DLLスレッド明記
- [MODIFY] `sync/NetplaySession.cpp`: "4層分離版" 削除、ThreadMain コメントにサブティック間隔(≈5.5ms)追記
- [MODIFY] `sync/NetplayClock.hpp`: 呼び出し元を NetplaySession → GameTickCodec に修正

---

# refactor: ISpeedController/MbaaSpeedController/SpeedMode 削除 → SpeedFlags に簡素化

## 2026-03-09: 速度制御の3層抽象化を解体し、2フラグのみの SpeedFlags に置換

### 概要
速度制御の本質は「描画スキップ(RenderSkip) + ティックバイパス(TickBypass)」の2フラグの ON/OFF のみ。
不要な抽象インターフェース・シングルトン・4値enum を全削除し、atomic<bool>×2 の SpeedFlags 構造体に置換。

### 削除
- [DELETE] `timing/ISpeedController.hpp` (57行): 実装が1つだけの抽象インターフェース
- [DELETE] `timing/MbaaSpeedController.hpp` (142行): シングルトン + SpeedMode enum + 冗長ログ

### 新規
- [NEW] `timing/SpeedFlags.hpp` (46行): RenderSkip + TickBypass の atomic<bool>×2 + SetHighSpeed/SetNormalSpeed

### 変更
- [MODIFY] `engine/FrameControl.hpp`: SpeedFlags 直接使用に変更、MaintainState() 削除
- [MODIFY] `engine/GameFrameOrchestrator.cpp`: MbaaSpeedController → SpeedFlags (3箇所)
- [MODIFY] `engine/SceneRunner.cpp`: MaintainState() 呼出し2箇所削除
- [MODIFY] `inject/dllmain.cpp`: TimeHooks 初期化後に SetTimeMultiplier(1000)/SetSleepBypass(true) を1回設定
- [MODIFY] `engine/SceneFastBoot.hpp`, `engine/SceneFastBoot.cpp`, `sync/FrameInputBuffer.hpp`, `detect/FastBootRunner.cpp`: コメント内参照更新

---

# refactor: 互換転送ヘッダ MbaaConstants.hpp を削除

## 2026-03-09: MbaaConstants.hpp 削除 + 個別ヘッダ参照に置換

### 変更内容
- [DELETE] `mbaa_game/constants/MbaaConstants.hpp` (MbaaAddresses + MbaaInputDefs の転送のみ)
- 8ファイルの #include を個別ヘッダに直接置換
- 5箇所のコメント内参照を更新

---

# refactor: InputHook → WndProcHook にリネーム、hooks/ に統合

## 2026-03-09: WndProcHook リネーム + フォルダ統合

### 変更内容
- [MOVE+RENAME] `mbaa_sync/input/InputHook.*` → `mbaa_sync/hooks/WndProcHook.*`
- `mbaa_sync/input/` フォルダ廃止
- クラス名 `InputHook` → `WndProcHook`

---

# refactor: 汎用的なフォルダ名/クラス名を目的がわかる名称にリネーム

## 2026-03-09: フォルダ4件 + ファイル/クラス6件リネーム

### フォルダ
- `fg_netplay/buffer/` → `fg_netplay/frame_input/`
- `fg_netplay/common/` → 廃止（WasapiClock を `frame_sync/` に統合）
- `mbaa_sync/common/` → `mbaa_sync/hooks/`

### ファイル/クラス
- `InputsContainer` → `FrameInputSlot`
- `NetplayState` → `FrameSyncState`
- `SceneBusiness` → `MatchScene`
- `GameControl` → `FrameControl`
- `SessionContext` → `MatchContext`
- `GameMonitor` → `PhaseMonitor`

---

# refactor: sync 系コンポーネントをリネーム — 目的がわかる名称に統一

## 2026-03-09: sync/ フォルダ + 3クラスをリネーム

### 変更内容
- `fg_netplay/sync/` → `fg_netplay/frame_sync/`
- `SyncCoordinator` → `NetplaySession` (通信スレッドのセッション管理)
- `CentralBuffer` → `FrameInputBuffer` (フレーム番号ベースの入力バッファ)
- `SyncCalculator` → `GameTickCodec` (GAME_TICK パケットのエンコード/デコード)
- 全61箇所の参照を一括更新

---

# refactor: READY/START/PING を GAME_TICK に統合、旧互換パケットを全削除

## 2026-03-09: パケット種別を GAME_TICK 1種に統合

### 概要
READY/START/PING を GAME_TICK 内の flags.bit0=ready と startTimeUs フィールドで表現。
PacketRouter から CS_INPUT/LOADING_INPUT/REMATCH_MENU/GAME_INPUT/旧レガシー処理を全削除。

### 変更ファイル
- [MODIFY] `SyncCalculator.hpp`: PKT_READY/PKT_START 削除、BuildGameTickPacket に ready/startTimeUs 追加
- [MODIFY] `SyncCalculator.cpp`: ProcessReceivedPacket を GAME_TICK 単一処理に統合
- [MODIFY] `SyncCoordinator.hpp`: PKT_READY/PKT_START 互換定数を削除
- [MODIFY] `SyncCoordinator.cpp`: 全フェーズで BuildGameTickPacket を使用
- [MODIFY] `PacketRouter.cpp`: 277行→50行 (CC10→SyncCoordinator 転送のみに簡素化)

### GameTickPayload (43B)
```
t_send(8) | echo_t1(8) | echo_t2(8) | baseFrame(4) | buttons(2) | direction(2)
| delay(1) | maxRollback(1) | flags(1) | startTimeUs(8)
```

---

# refactor: 通信処理を fg_netplay/network/ に統合

## 2026-03-09: ネットワーク関連ファイルを専用フォルダに移動

### 変更内容
- [NEW] `fg_netplay/network/` ディレクトリ新設
- [MOVE] UdpSocket, NetworkSimulator (`fg_netplay/common/` → `fg_netplay/network/`)
- [MOVE] NetplayManager (`fg_netplay/sync/` → `fg_netplay/network/`)

---

# refactor: GamePhase を mbaa_game へ移動 + メモリアクセス処理を memory/ に統合

## 2026-03-09: mbaa_game 内構造再編

### 変更内容
- [MOVE] `fg_netplay/types/GamePhase.hpp` → `mbaa_game/monitor/`（ゲーム固有概念として再分類）
- [MERGE] `mbaa_game/{common,patcher,state,dump}/` → `mbaa_game/memory/`（メモリアクセス統合）
  - MemoryPatcher, MemDumper, MbaaPatcher, StateBuffer, StateRingBuffer, DumpEntryList

---

# refactor: overlay/UI を core_dll/ui/ に統合

## 2026-03-09: 3箇所に分散していた UI ファイルを統合

### 概要
`fg_netplay/overlay/`、`mbaa_game/ui/`、`mbaa_sync/overlay/` に分散していた
全22ファイル（overlay + UI）を `core_dll/ui/` に統合。

### 変更内容
- [MOVE] `fg_netplay/overlay/*` (12ファイル) → `ui/`
- [MOVE] `mbaa_game/ui/*` (8ファイル) → `ui/`
- [MOVE] `mbaa_sync/overlay/*` (2ファイル: UIManager) → `ui/`
- [MODIFY] 全 #include パス更新 (25箇所)
- [MODIFY] `core_dll/CMakeLists.txt`, `tests/CMakeLists.txt` パス更新

---

# refactor: platform/ 解体 — 各層の common/ にコモン機能を再配置

## 2026-03-08: platform/ ディレクトリを廃止し、使用元の層に再配置

### 概要
独立した `platform/` 層は全層から参照される形になり、層分離の意図に反していたため解体。
各ファイルを実際に使用する層の `common/` サブディレクトリに再配置。

### 変更内容
- [MOVE] `platform/network/` (UdpSocket, NetworkSimulator) → `fg_netplay/common/`
- [MOVE] `platform/clock/` (WasapiClock) → `fg_netplay/common/`
- [MOVE] `platform/hooks/` (DxHook, TimeHooks, DirectInputHook) → `mbaa_sync/common/`
- [MOVE] `platform/hooks/ISpeedController.hpp` → `mbaa_game/speed/`（唯一の使用元と同ディレクトリに）
- [MOVE] `platform/memory/` (MemoryPatcher, MemDumper) → `mbaa_game/common/`
- [MOVE] `platform/common/DebugLog.hpp` → `common/`（core_dll直下、全層共有）
- [DELETE] `platform/` ディレクトリ

---

# refactor: core_dll 4層アーキテクチャ再構成 — platform/fg_netplay/mbaa_sync/mbaa_game

## 2026-03-08: ディレクトリ構造を4層に再編成

### 概要
core_dll の全ファイルを4つのアーキテクチャ層に再配置。ロジック変更なし、
ディレクトリ移動 + include パス更新 + CMakeLists 更新のみ。

### 4層構造
| 層 | 概念 | 何を知っているか |
|---|---|---|
| `platform/` | 低レイヤー基盤 | OS, D3D9, UDP, WASAPI |
| `fg_netplay/` | 格闘ゲームネット対戦エンジン | フレーム同期, ロールバック, NTP |
| `mbaa_sync/` | MBAA同期処理（接合層） | fg_netplay + mbaa_game の橋渡し |
| `mbaa_game/` | MBAAゲーム固有 | MBAAメモリレイアウト, GameMode |

### 主な変更
- [MOVE] 旧8ディレクトリ → 新4ディレクトリに全ファイル再配置 (git mv)
- [NEW] `fg_netplay/types/GamePhase.hpp`: GamePhase enum 定義を GamePhaseDetector.hpp から分離
- [MODIFY] `mbaa_game/monitor/GamePhaseDetector.hpp`: inline enum → include に変更
- [MODIFY] 全 .hpp/.cpp の #include パスを新構造に更新 (157箇所)
- [MODIFY] `core_dll/CMakeLists.txt`: 全 .cpp パスを新構造に更新
- [MODIFY] `cli_launcher/CMakeLists.txt`: UdpSocket/NetworkSimulator パス更新
- [MODIFY] `tests/CMakeLists.txt`: OverlayRenderer/NetplayOverlay パス更新
- [MODIFY] `cli_launcher/*.cpp`: 3ファイルの include パス更新
- [MODIFY] `tests/*.cpp`: 2ファイルの include パス更新

### 旧→新ディレクトリ対応
| 旧 | 新 |
|---|---|
| `adapter_network/` | `platform/network/` |
| `adapter_os_hooks/api_hook/` | `platform/hooks/` |
| `adapter_os_hooks/input/DirectInputHook` | `platform/hooks/` |
| `adapter_os_hooks/input/InputHook` | `mbaa_sync/input/` |
| `adapter_netplay/timer/WasapiClock` | `platform/clock/` |
| `adapter_netplay/timer/NetplayClock` | `fg_netplay/sync/` |
| `adapter_netplay/` (Sync*, Metronome, NetplayManager) | `fg_netplay/sync/` |
| `adapter_netplay/` (SyncCalculator, PacketRouter) | `mbaa_sync/protocol/` |
| `pure_sync_engine/` (CentralBuffer, Inputs, NetplayState) | `fg_netplay/buffer/` |
| `pure_sync_engine/RollbackEngine` | `mbaa_sync/rollback/` |
| `session_orchestrator/session/` | `mbaa_sync/orchestrator/` |
| `session_orchestrator/scene/` | `mbaa_sync/scene/` |
| `game_memory_accessor/patcher/MemoryPatcher` | `platform/memory/` |
| `game_memory_accessor/dump/MemDumper` | `platform/memory/` |
| `game_memory_accessor/` (残り全部) | `mbaa_game/` |
| `feature_overlay_ui/` (NetplayOverlay, State_Ui_*, InGame_Ui_*, Rematch_Ui_*, OverlayRenderer) | `fg_netplay/overlay/` |
| `feature_overlay_ui/UIManager` | `mbaa_sync/overlay/` |
| `feature_overlay_ui/` (CharaSelect_Ui_*, Controller_Ui_*, ControllerMapper) | `mbaa_game/ui/` |
| `common/` | `platform/common/` |
| `dllmain.cpp` | `mbaa_sync/dllmain.cpp` |

---

# feat: IntroBarrier — peer 双方の intro=2 到達を確認後にラウンド開始

## 2026-03-08: IntroBarrier 同期ロジック追加

### 概要
ラウンド開始時に両 peer が intro=2 (イントロ完了) に到達したことを相互確認してから
ゲーム進行を再開する IntroBarrier 機能を追加。片方のみ先行してゲームが進む問題を解消。

### 変更ファイル
- [MODIFY] `SyncCalculator.cpp`:
  - `GameTickPayload` に `flags` フィールド追加 (bit0: introComplete)
  - `ProcessReceivedPacket()`: flags の introComplete ビットで `peerIntroComplete` を設定
  - `BuildGameTickPacket()`: `localIntroComplete` 状態を flags に反映
  - `SyncCoordinator.hpp` の include 追加
- [MODIFY] `SceneBusiness.cpp`:
  - `OnLoading()`: Loading 中に `localIntroComplete=true` を事前シグナル
  - `HandleRoundStartSync()`: intro=2 到達時に `localIntroComplete` 設定 + `peerIntroComplete` のバリア待機追加
  - `ResetInGame()`: `peerIntroComplete` のみリセット（次ラウンドの peer 到達を待つため）
  - `OnInGame()`: コメント・変数名を IntroBarrier 対応に更新

---

# chore: build.bat にクリーンビルドオプション + デュアルインスタンスデプロイ追加

## 2026-03-08: ビルドスクリプト改善

### 変更内容
- [MODIFY] `build.bat`:
  - `build.bat clean` でビルドディレクトリを削除するクリーンビルドオプション追加
  - デプロイ先を `_TEST_MBAACC/cccaster/` から `_TEST_MBAACC/MBAACC_1/cccaster/` + `_TEST_MBAACC/MBAACC_2/cccaster/` のデュアルインスタンス構成に変更
  - デプロイ先ディレクトリの存在チェック付き

---

# refactor: 目的軸フォルダ構造への全面移行（3層→9フォルダ）

## 2026-03-09: core_dll フォルダ再配置

### 変更内容
- 旧3層構造（`fg_netplay/`, `mbaa_sync/`, `mbaa_game/`）を廃止
- 「やりたい事の順序」に基づく9フォルダに全61ファイルを再配置:
  - `inject/` (4) — ゲームに介入する
  - `detect/` (7) — ゲームの状態を知る
  - `input/` (6) — 入力を奪う
  - `timing/` (10) — 時間を支配する
  - `network/` (10) — 相手と通信する
  - `sync/` (7) — 入力を同期する
  - `rollback/` (7) — 過去を修正する
  - `engine/` (10) — 全体を駆動する
  - `ui/` (22, 移動なし) — 情報を表示する
- 全ファイルの `#include` パスを新構造に置換（45ファイル/113件）
- `CMakeLists.txt` 2ファイル更新（`core_dll/`, `cli_launcher/`）
- クリーンビルド（EXE + DLL + test_overlay）成功確認

---

# docs: GameControl.hpp の旧描画制御コメントを現行 API Hook 仕様に修正

## 2026-03-08: GameControl.hpp コメント修正（旧 CC_SKIP_FRAMES/CC_PAUSE_FLAG 時代の記述を削除）

### 変更内容
- [MODIFY] `GameControl.hpp`: 速度制御関数のコメントを現行の RenderSkip/TickBypass + SleepFrame gap 方式に統一
  - `SetModeHighSpeedSkip()`: 旧「描画スキップ=ON(1)」→ 新「RenderSkip=ON, TickBypass=ON」
  - `SetModeRollupSkip()`: 旧「描画スキップ=frames」→ 新「RenderSkip=ON, TickBypass=ON」
  - `SetModeNormalSpeed()`: 旧「描画スキップ=ON(1)固定」→ 新「RenderSkip=OFF（SleepFrameが動的制御）」
  - `SetModePause()`: 旧「描画スキップ=ON(1)固定, PauseFlag=1」→ 新「RenderSkip=OFF, SleepFrameで自然待機」
  - `MaintainState()`: 旧「CC_SKIP_FRAMES/CC_PAUSE_FLAG再設定」→ 新「空実装（SleepFrameが動的制御）」
  - Layer 2 例: 旧「PauseForSync()」→ 新「SetModePause()」（削除済み関数名を修正）
  - Layer 1 例: 旧「SetPauseFlag(1), SetSkipFrames(1)」→ 新「GetInputBasePtr(), WriteP1Input()」
  - Layer 3 例: 旧「SceneInGame, SceneCharaSelect」→ 新「SceneBusiness」

---

# docs: GamePhase enum コメントを実装に合わせて修正

## 2026-03-08: GamePhaseDetector.hpp の GamePhase コメント修正

### 変更内容
- [MODIFY] `GamePhaseDetector.hpp`: GamePhase enum の各メンバコメントに記載されていた
  GameMode 値が実際の `GameMonitor.cpp` の switch 文と不一致だったため修正。
  - `MainMenu`: 旧「2, 4, 6, 8, 9, 10, 16, 18, 19, 25, 26」→ 新「2=TITLE, 65535=STARTUP」
  - `Loading`: 旧「21」→ 新「8=LOADING, 13=LOADING_DEMO」
  - `InGame`: 旧「22」→ 新「1=IN_GAME, 26=REPLAY」
  - `Rematch`: 旧「23」→ 新「5=RETRY」

---

# feat: ネットワーク遅延・パケットロスシミュレーション機能

## 2026-03-07: 送受信パケットにランダム遅延+ランダムロスを挿入するテスト機能

### 概要
テスト用途で、パケット送信・受信の両方にランダム遅延(50~90ms)とパケットロス(20%)を
シミュレートする機能を追加。CLI引数で有効化し、無効時はゼロオーバーヘッド。

### 新規ファイル
- [NEW] `adapter_network/NetworkSimulator.hpp`: シミュレーション設定管理シングルトン（遅延範囲/ロス率/乱数生成）
- [NEW] `adapter_network/NetworkSimulator.cpp`: Enable/GetRandomDelayMs/ShouldDrop の実装

### 変更ファイル
- [MODIFY] `adapter_network/UdpSocket.cpp`:
  - `Send()`: パケットロス判定（ShouldDrop→即return）+ ASIO steady_timer による非同期遅延送信
  - `DoReceive()`: パケットロス判定（コールバックスキップ）+ steady_timer による非同期遅延受信
  - `Impl` に `pendingTimers` リスト追加（タイマー寿命管理）
- [MODIFY] `cli_launcher/main.cpp`: `--sim-delay min,max` / `--sim-loss N` 引数パース追加
- [MODIFY] `core_dll/CMakeLists.txt`: NetworkSimulator.cpp 追加
- [MODIFY] `cli_launcher/CMakeLists.txt`: NetworkSimulator.cpp 追加
- [MODIFY] `_TEST_MBAACC/dual_test.bat`: HOST/CLIENT に `--sim-delay 50,90 --sim-loss 20` 追加

### CLI使用方法
```
CCCaster_v10.exe --headless --host --port 7500 --sim-delay 50,90 --sim-loss 20
```

### 設計メモ
- ASIOの `steady_timer` で遅延を実現（io_contextスレッドをブロックしない非同期設計）
- NetworkSimulator はグローバルシングルトン（DLLとEXE両方で同一インスタンス参照）
- シミュレーション無効時は `IsEnabled()` の atomic load のみで分岐（実質ゼロコスト）

---

# fix: core_dll 統合問題5件修正 — パケット衝突・二重処理・初期化・処理順序

## 2026-03-06: コンポーネント間統合問題の修正

### 問題
パケット通信、DLLゲーム制御、メトロノーム、セントラルバッファが個別に機能するが
正常なリンク（対戦同期）が成立しない。5つの根本原因を特定・修正。

### 修正内容

#### 1. PKT_GAME_TICK と TYPE_LOADING_INPUT の 0x20 衝突 (CRITICAL)
- [MODIFY] `SyncCalculator.hpp`: PKT_GAME_TICK を 0x20 → 0x30 に変更

#### 2. PacketRouter → SyncCoordinator 二重処理経路
- [MODIFY] `PacketRouter.cpp`: 全パケット転送を廃止し、SyncCoordinator管轄パケット(PING/READY/START/GAME_TICK)のみ選択的に転送。到達不能なswitch case(0x00/0x15/0x16)を削除。

#### 3. confirmedRemoteFrame 初期値 0 によるゲームフリーズ
- [MODIFY] `CentralBuffer.hpp`: `GetEffectiveHead()` に confirmed==0 の安全処理を追加。`InitializeConfirmedRemoteFrame()` メソッド追加。
- [MODIFY] `SyncCoordinator.cpp`: Start() で confirmedRemoteFrame を writeHead(200) と同値で初期化。

#### 4. SceneRunner の処理順序が設計と不一致
- [MODIFY] `SceneRunner.cpp`: Step() の処理順を SleepFrame→SceneBusiness から SceneBusiness→SleepFrame に修正（設計書のInput→SleepFrame→Logicに準拠）。

#### 5. 統合フロー確認ログ追加
- [MODIFY] `SceneRunner.cpp`: 60フレームごとに writeHead/readPos/effectiveHead/confirmedRemoteFrame/synced/peerAlive を出力。

### テスト結果
- HOST/CLIENT 両方で `synced=1 peerAlive=1` を確認
- F=2640+ まで安定フレームカウント（25秒間）
- RTT=80-100μs（ローカルループバック）
- α1=0 α2=0（ドリフトなし）

---

# refactor: SyncCoordinator 4層分離 — メトロノーム・同期計算器・通信・ゲーム

## 2026-03-05: 通信スレッドとメトロノームの分離

### 問題
SyncCoordinator がメトロノーム（α補正付きタイミング制御）とパケット送受信を
1ループで結合していたため、α補正が送受信間隔まで引きずる構造上の問題があった。

### 修正内容
- [NEW] `Metronome.hpp/cpp`: α1+α2補正付きフレームカウンタ（独立スレッド）
- [NEW] `SyncCalculator.hpp/cpp`: Θ/RTT計算、α1/α2算出、CentralBuffer書込み、パケット組立て
- [MODIFY] `SyncCoordinator.hpp/cpp`: 通信専用にスリム化（計算・バッファ操作を委譲）
- [MODIFY] `CMakeLists.txt`: 新規ソース2件追加

### 設計メモ
| 層 | 責務 | α補正の影響 |
|---|---|---|
| メトロノーム | カウンタのカウントアップ | α1+α2で間隔変動 |
| 同期計算器 | Θ計算、α算出、CentralBuffer書込み | なし |
| 通信スレッド | パケット送受信のみ | 受けない（固定間隔） |
| ゲームスレッド | CentralBuffer監視 | 間接的 |

---



## 2026-03-05: PRESS LEFT/RIGHT TO ASSIGN 点滅アニメーション修正

### 問題
`ImGui::GetTime()` がDLLのタイムフック高速化の影響を受け、
「PRESS LEFT/RIGHT TO ASSIGN」テキストの点滅が異常に高速になっていた。

### 修正内容
- [MODIFY] `Controller_Ui_View.cpp`:
  - `ImGui::GetTime()` ベースから static フレームカウンタベースに変更
  - `s_pulseFrame * (2π / 60)` で60F=1周期のゆるやかなパルスに

---



## 2026-03-05: オーバーレイ 1F1回描画（Present最終描画方式）

### 問題
MBAAは1フレームで EndScene を約8回呼び、バックバッファに複数回描画する。
最初のEndSceneでImGuiを描画しても後続のゲーム描画パスで上書きされるため、
EndScene内でのImGui描画はそのままでは機能しない。

### 修正内容
- [MODIFY] `GameFrameOrchestrator.cpp`:
  - `s_imguiFrameReady` フラグを追加
  - OnEndScene: 最初のバックバッファヒット時にImGuiフレームデータを準備（NewFrame/Render）
    → `RenderDrawData` は呼ばず、`s_imguiFrameReady = true` をセット
  - OnPresent: `s_imguiFrameReady` が true なら
    `BeginScene → RenderDrawData → EndScene` で全ゲーム描画の上に最終描画
    → `s_imguiFrameReady = false` にリセット

### 設計メモ
D3D9の呼び出し順は EndScene×N → Present×1。
Present直前にBeginScene/EndSceneで再描画することで、ゲームの全描画の上に
ImGuiオーバーレイが最上位レイヤーとして確実に表示される。

---



## 2026-03-05: game_memory_accessor リファクタリング

### 変更内容

#### A. MbaaConstants.hpp の分割
- [NEW] `constants/MbaaAddresses.hpp`: メモリアドレス・定数定義を抽出
- [NEW] `constants/MbaaInputDefs.hpp`: 入力ボタン・方向キー・入力合成マクロを抽出
- [MODIFY] `MbaaConstants.hpp`: 互換転送ヘッダに変換（既存コードの後方互換を維持）
- [DELETE] `IndexedFrame` union — `NetplayState.hpp` に同一定義あり（重複除去）
- [DELETE] `gameModeStr()` — 呼出箇所0のデッドコード

#### B. ディレクトリ再構成（7サブディレクトリ）
- `constants/` — MbaaAddresses.hpp, MbaaInputDefs.hpp
- `monitor/` — GamePhaseDetector.hpp, GameMonitor.cpp
- `speed/` — MbaaSpeedController.hpp
- `boot/` — FastBootRunner.hpp/.cpp
- `patcher/` — MemoryPatcher.hpp, MbaaPatcher.hpp/.cpp
- `dump/` — MemDumper.hpp, DumpEntryList.hpp
- `state/` — StateBuffer.hpp/.cpp, StateRingBuffer.hpp

#### C. include パス更新
- CMakeLists.txt: .cppパス3件更新
- 内部ファイル: 7件のincludeパス更新
- 外部参照: 6件のincludeパス更新（直接参照のみ）
- 残り12件の外部参照は互換転送ヘッダ経由で変更不要

---

# chore: tools/ ディレクトリをGit追跡対象から除外

## 2026-03-04: tools/ を .gitignore に追加

### 変更内容
- [MODIFY] `.gitignore`: `tools/` エントリを追加
- `git rm -r --cached tools/` でインデックスから除外（ファイルはディスク上に残存）

### 理由
tools/ 配下（dummy_peer, lan_test 等）はローカル開発ツールであり、メインリポジトリの追跡対象から外す。

---

# refactor: CentralBuffer readPos算出方式 — playHead廃止、readPos=writeHead-delay-maxRollback

## 2026-03-04: CentralBuffer readPos 算出方式への変更

### 設計変更
- playHead (独立カウンタ) を廃止
- DLL読取位置を `GetReadPos() = writeHead - delay - maxRollback` で毎フレーム算出
- 非ロールバック区間: confirmed=true のスロットのみ消費（未確定なら待つ）
- ロールバック区間: 予測入力で進行可（後からロールバック）

### 変更ファイル
- [MODIFY] `CentralBuffer.hpp`: playHead削除、GetReadPos()/SetSyncParams() 追加、AdvancePlayHead/SetPlayHead/GetPlayHead 削除
- [MODIFY] `SyncCoordinator.cpp`: SetSyncParams(delayFrames, maxRollback) 呼出追加
- [MODIFY] `SceneLoading.cpp`: GetReadPos() + confirmed チェック方式に更新
- [MODIFY] `SceneCharaSelect.cpp`: 同上

---



## 2026-03-04: 全Scene入力をCentralBuffer一元化

### 変更方針
全Sceneの入力をCentralBuffer.GetSlot(playHead)→GC::WriteInput()の統一パターンに変更。
DLL側は一切ディレイ計算を行わない（通信スレッドがバッファ書込み時に処理済み）。

### 変更ファイル
- [MODIFY] `SceneLoading.cpp`: 全面書き換え — 独自ディレイバッファ/Phase判定/atomic s_remoteInput 削除
- [MODIFY] `SceneLoading.hpp`: `SetRemoteLoadingInput` 宣言削除
- [MODIFY] `SceneCharaSelect.cpp`: 全面書き換え — Filter A/B/C/atomic s_remoteCharaInput/ProcessDelayInput 削除
- [MODIFY] `SceneCharaSelect.hpp`: `SetRemoteInput` 宣言削除
- [MODIFY] `PacketRouter.cpp`: SetRemoteInput/SetRemoteLoadingInput 呼出を廃止コメントに置換

---



## 2026-03-04: autoTest 関連コード削除

### 削除対象
- `SceneRunner.cpp`: `GenerateRandomTestInput()` 関数、autoTestMode 入力注入ブロック、`s_latestRemoteFrame`、`DirectInputHook` include、`cstdlib`/`ctime` include
- `SessionContext.hpp`: `autoTestMode` フィールド
- `dllmain.cpp`: `ctx.autoTestMode = state.headlessMode` 設定行、ログ出力の autoTest パラメータ

### 理由
通信スレッドが蓄積した CentralBuffer 以外からの入力取得パスを排除。入力はすべて CentralBuffer 経由で一元管理する。

---



## 2026-03-04: CentralBuffer 統合 (Phase 1-3)

### 変更方針
- CentralBuffer を唯一のフレームデータパスとして再定義
- SharedSyncState.currentFrame → CentralBuffer.writeHead に統一
- FrameSlot = {frame, gamePhase, rollbackable, localInput, remoteInput, confirmed}

### 変更ファイル
- [MODIFY] `CentralBuffer.hpp`: FrameSlot 再定義 + WriteSlot/ConfirmRemote/GetWriteHead/SetWriteHead API
- [MODIFY] `SyncCoordinator.cpp`: currentFrame→writeHead、WriteLocalInput→WriteSlot、WriteRemoteInput→ConfirmRemote、SharedSyncState.remoteInputs削除
- [MODIFY] `GameControl.hpp`: SleepFrame の参照先を CentralBuffer.GetWriteHead() に変更

---



## 2026-03-04: 描画制御の統一

### 変更方針
- CC_SKIP_FRAMES_ADDR は使用禁止（全箇所コメントアウト）
- 描画の ON/OFF は API hook (RenderSkip → OnPresentSkip) で制御

### 変更ファイル
- [MODIFY] `GameControl.hpp`: CC_SKIP_FRAMES 使用禁止の注意コメント追記
- [MODIFY] `GameFrameOrchestrator.cpp`: OnPresentSkip が RenderSkip() を参照するよう変更
- [MODIFY] `MbaaSpeedController.hpp`: MaintainState の CC_SKIP_FRAMES=0 強制を削除、SetMode内の使用をコメントアウト
- [MODIFY] `MbaaConstants.hpp`: CC_SKIP_FRAMES_ADDR 定義をコメントアウト
- [MODIFY] `FastBootRunner.cpp`: WriteMemory呼出をコメントアウト
- [MODIFY] `DumpEntryList.hpp`: ダンプ登録をコメントアウト

---



## 2026-03-04: SleepFrame 制御方式の全面見直し

### 変更方針
- worldTimer はゲームエンジンが毎F++するのみ（DLL側の読取/書込を廃止）
- currentFrame 初期値を 2000 に変更（worldTimer より先行開始）
- SleepFrame は gap = currentFrame - worldTimer で制御:
  - gap <= 0: worldTimer が追いついた → currentFrame 変化を待機
  - gap == 1: 通常速度で 1F 進行
  - gap >= 2: 描画OFF で高速に追いつかせる

### 削除した機能
- HighSpeed パスの worldTimer → currentFrame 転記
- ワールドタイマーゲート (360F 先行制限)
- Counting 遷移時の worldTimer 読取

### 変更ファイル
- [MODIFY] `GameControl.hpp`: SleepFrame を gap 方式に全面書き直し
- [MODIFY] `SyncCoordinator.cpp`: 初期値=2000, worldTimer読取削除, MbaaConstants include削除

---



## 2026-03-03: SleepFrame HighSpeed パス修正

### 問題
- HighSpeed (TickBypass=true) 中に SleepFrame が即リターンし worldTimer が暴走
- currentFrame が追いつけず NormalSpeed 復帰時に大きな乖離
- 乖離の解消時間が各マシンで異なるため worldTimer が永久にずれる

### 変更内容
- [MODIFY] `GameControl.hpp`: SleepFrame の HighSpeed パスで `currentFrame = *CC_WORLD_TIMER_ADDR` を毎F書込み
- [MODIFY] `SyncCoordinator.hpp`: 非const版 `GetMutableState()` 追加（SleepFrame からの currentFrame 書込み用）

---



## 2026-03-03: currentFrame 初期値を CC_WORLD_TIMER_ADDR に設定

### 問題
- Counting 開始時に `currentFrame=0` で始まるため、各マシンの worldTimer との差が大きい
- ワールドタイマーゲート（360F先行制限）で不要なフリーズが発生
- フリーズ解除タイミングが各マシンで異なるため worldTimer が永久にずれる

### 変更内容
- [MODIFY] `SyncCoordinator.cpp`:
  - `MbaaConstants.hpp` を include 追加
  - Counting 遷移時に `currentFrame = *CC_WORLD_TIMER_ADDR` で初期化
  - キャッチアップバーストが遅れた側の worldTimer を追いつかせる

---



## 2026-03-03: Catch-up バースト + Δθログ改善

### 問題
- SyncCoordinator に相手フレームとの比較・バースト進行（キャッチアップ）ロジックが未実装
- 相手がフレーム先行しても追いつくメカニズムがなかった

### 変更内容
- [MODIFY] `SyncCoordinator.hpp`: `_latestPeerFrame` フィールド追加
- [MODIFY] `SyncCoordinator.cpp`:
  - GAME_TICK 受信時に `gtp.baseFrame` → `_latestPeerFrame` を更新
  - Counting モード: `_latestPeerFrame > frame+1` の場合バースト進行（CentralBuffer にローカル入力を埋めつつスキップ）
  - ログを `Δθ`(ベースライン差分) + `peerF` 表示に改善
- [MODIFY] `NetplayClock.hpp`: `GetBaselineTheta()` getter 追加

---



## 2026-03-03: GetTickUs ベースラインθ差分化

### 問題
- NTP θ はマシン間の絶対クロック差（例: ±7073秒）を正確に反映するが、
  `GetTickUs()` がこの巨大な θ をそのままα補正に使い、常に最大飽和 (tick=14000/19332) で暴走

### 修正内容
- [MODIFY] `NetplayClock.hpp`: `_baselineTheta` フィールドと `SetBaselineTheta()` を追加
- [MODIFY] `NetplayClock.cpp`: `GetTickUs()` の absTheta を `θ - baseline` (Δθ) に変更。
  符号判定も Δθ ベースに修正
- [MODIFY] `SyncCoordinator.cpp`: Counting 遷移時に `_clock.SetBaselineTheta()` を呼出し

---



## 2026-03-03: WaitStart停滞修正 + UDPポートバインド統一

### 問題1: WaitStart → Counting 遷移不可
- PING(0x00) にペイロードがなく、WaitStart 中に NTP サンプルが蓄積されない
- `IsThetaStable()` (最低10サンプル) が永久に false → START 未送信 → Counting 不到達

### 問題2: クライアントがOS割当ポートを使用
- `SessionNegotiator` がクライアント時に `UdpSocket(0)` でバインド → 意図しないエフェメラルポート

### 変更内容
- [MODIFY] `SyncCoordinator.cpp`:
  - `PingPayload` 構造体新設（t_send, echo_t1, echo_t2）
  - `SendPing()`: エコー情報を搭載して送信（GAME_TICK と同等の NTP エコー機構）
  - `DrainAndProcessPackets()`: PING 受信時に `AddNtpSample()` 呼出し + エコー追跡更新
- [MODIFY] `SessionNegotiator.cpp`: `UdpSocket(isHost ? port : 0)` → `UdpSocket(port)` に変更

---



## 2026-03-03: §2 Central Ring Buffer + §3 ゲート閾値修正

### 変更内容
- [NEW] `CentralBuffer.hpp`: 600スロットリングバッファ。FrameSlot（自入力, 相手入力,
  localBaseFrame, remoteBaseFrame, remoteConfirmed）。WriteLocal/RemoteInput（通信スレッド）、
  GetSlot/PlayHead/ConsumeMismatch（ゲームスレッド）。観戦者再生用の将来拡張を想定。
- [MODIFY] `SyncCoordinator.cpp`:
  - GAME_TICK 受信時に `CentralBuffer::WriteRemoteInput()` 呼出し（SharedSyncState と並行書込み）
  - subTick0 のローカル入力確定後に `CentralBuffer::WriteLocalInput()` 呼出し
  - CentralBuffer include 追加
- [MODIFY] `GameControl.hpp`: ワールドタイマーゲートの閾値を `worldTimer > syncFrame` →
  `worldTimer > syncFrame + 360`（3秒以上先行時のみ停止）に変更。軽微な先行はα補正で自然吸収。

---



## 2026-03-03: SyncCoordinator 3連パケット + NetplayClock NTP方式

### 背景
旧Θ推定は片道 offset_raw の最大値フィルタで精度が限られていた。
入力パケットは subTickIdx==0 でのみ送信され、冗長送信によるロス対策がなかった。

### 変更内容: NetplayClock (NTP T1-T4 min-RTT)
- [MODIFY] `NetplayClock.hpp`: `AddThetaSample()` → `AddNtpSample(t1,t2,t3,t4)` に変更。
  `ThetaSample` 構造体追加（T1-T4, RTT, θ）。旧 max-offset / EMA ドリフト / 基準θ偏差を廃止。
  3段階α定数（`DEAD_BAND_US=500`, `STRONG_TH_US=16666`, `MAX_ALPHA_US=2666`）追加。
- [MODIFY] `NetplayClock.cpp`: RTT/θ計算式を NTP 準拠に改修。
  最小RTTフィルタリングで最良θを採用。GetTickUs() を3段階α補正（デッドバンド→二乗→飽和）に簡素化。

### 変更内容: SyncCoordinator (GAME_TICK 3連パケット)
- [MODIFY] `SyncCoordinator.hpp`: `PKT_GAME_TICK=0x20` 追加、`SendGameTick()` 宣言追加、
  エコー追跡フィールド (`_lastPeerT1`, `_lastPeerRecvUs`)、現フレーム入力 (`_currentInputButtons/Direction`) 追加。
- [MODIFY] `SyncCoordinator.cpp`:
  - `GameTickPayload` 構造体追加（baseFrame, t_send, echo_t1, echo_t2, buttons, direction）
  - `SendGameTick()`: GAME_TICK パケット組立+送信（NTPエコー付き）
  - `DrainAndProcessPackets()`: GAME_TICK 受信時に NTP T1-T4 サンプル投入 + エコー追跡 + 入力書込み
  - Counting モード: 3サブティック全てで GAME_TICK を送信（入力はフレーム開始時に確定・固定）

---



## 2026-03-03: CC_WORLD_TIMER_ADDR vs currentFrame フレーム同期制御

### 背景
SyncCoordinator::currentFrame（通信スレッド駆動）とゲーム内蔵の CC_WORLD_TIMER_ADDR が
独立してカウントアップしており、両者のずれを検出・制御する仕組みがなかった。

### 変更内容
- [MODIFY] `GameControl.hpp`: `SleepFrame()` の currentFrame ポーリング後に
  ワールドタイマーゲートを追加。
  - `worldTimer > syncFrame`（ゲーム先行）→ スピンウェイトで currentFrame の追いつきを待機
  - `worldTimer ≤ syncFrame`（早回し中）→ 何もしない（早回し許容）
  - 高速早回し中のため Sleep なしスピンウェイト

---



## 2026-03-02: Sleep(1)ポーリング廃止 → 精密サブティック方式

### 背景
旧ループは `Sleep(1)` (~1-15ms不定) でポーリングし、200ms間隔でPING/READY送信。
ティック精度が低く、ハンドシェイク応答も最大200ms遅延。

### 変更内容
- [MODIFY] `SyncCoordinator.hpp` — `PING_INTERVAL_US`/`READY_INTERVAL_US` 削除、`SUB_TICKS_PER_FRAME=3` 追加、`SleepUntil()` 宣言追加、`_lastPingSentUs`/`_lastReadySentUs` フィールド削除。
- [MODIFY] `SyncCoordinator.cpp`:
  - `SleepUntil()`: Sleep+スピンのハイブリッド精密スリープ（2ms以上→Sleep(1)、未満→YieldProcessor）
  - `ThreadMain()`: 1F(≈16666μs) を3分割 → ~5555μs間隔のサブティックループ
  - WaitReady/WaitStart: 毎サブティック (~5.5ms) でPING/READY送信（旧200ms→35倍高速化）
  - Counting: `subTickIdx==0` で1Fに1回フレーム進行、サブティック毎にドレイン処理
  - `nextSubTickUs` 累積方式でドリフト補正と連動

---



## 2026-03-02: ドリフト補正の非線形θ偏差ベース刷新 + タイムアウト短縮

### 背景
旧ドリフト補正は `driftRate × BASE_TICK_US` で ~0.03μs/F の補正しか効かず実質無効。
また切断タイムアウト10秒が長すぎ、相手の強制切断時のレスポンスが悪い。

### 変更内容
- [MODIFY] `NetplayClock.hpp` — `_baseThetaUs`（初期基準θ）と `GetThetaDeltaUs()` を追加。θ絶対値ではなく偏差で補正。
- [MODIFY] `NetplayClock.cpp` — GetTickUs() を4段階非線形θ偏差補正に刷新:
  - |Δ| ≤ 500μs: デッドバンド（補正なし）
  - 500μs < |Δ| ≤ 1F(16666μs): 三乗カーブ（攻撃的非線形、最大2666μs）
  - 1F < |Δ| ≤ 2.2F(36665μs): 最大補正（2666μs = 16%速度差）
  - |Δ| > 2.2F: フレームスキップ相当（±16666μs = 1F丸ごと）
- [MODIFY] `SyncCoordinator.hpp` — DISCONNECT_TIMEOUT_FRAMES を 600F(10秒) → 180F(3秒) に変更。

---



## 2026-03-02: ネゴシエーション時UDPポートのDLL側再利用 + レースコンディション修正

### 背景
SyncCoordinatorの同期テストにおいて、VM→ホスト方向のUDP通信が不到達であり、
ホスト側SyncCoordinatorが `WaitReady` から遷移できない問題が発生していた。

### 根本原因1: ポート不一致（VM→ホスト不到達）
- CLI Launcher のネゴシエーション時にOS割当されたエフェメラルポートが `state.localPort` に記録される
- しかし `dllmain.cpp` でクライアント側が `localPort=0` にハードコードされ、DLL側で別のエフェメラルポートが割り当てられていた
- VM側の `peerPort` はネゴ時のポートを指すため、DLLの新ポートに到達しない

### 根本原因2: レースコンディション（WaitReady永久停滞）
- VM側でホストの最初のREADYパケットがSyncCoordinator起動前に到着→ドロップ
- ホスト側がWaitStartに遷移後、PINGのみ送信しREADYを停止
- VM側SyncCoordinatorは永久にWaitReadyに留まる

### 変更内容
- [MODIFY] `dllmain.cpp` — `ctx.localPort = ctx.isHost ? state.localPort : 0` を `ctx.localPort = state.localPort` に変更。HOST/CLIENT共にネゴシエーション時と同じポートを再利用。
- [MODIFY] `SyncCoordinator.cpp` — WaitStartフェーズでもREADYパケットを200ms毎に継続送信するよう変更。遅延起動した相手SyncCoordinatorに対応。

### テスト結果
- 両側でWaitReady→WaitStart→Counting遷移に成功
- θ推定安定（drift≈0）、alive=1（疎通維持）
- IPC syncCompleted flag正常セット
- F=900+ まで安定フレームカウント確認

---

# refactor: 同期エンジン再設計 — NetplayClock + SyncCoordinator 3モード化

## 2026-03-02: TimeSynchronizer/VirtualClock を廃止し NetplayClock に統合

### 設計
- **NetplayClock** (新規): 純粋関数群 — θ推定、ドリフトレート学習(EMA)、1F周期算出、スタート時刻管理
- **SyncCoordinator** (改修): 3モード・ステートマシン — WaitReady→WaitStart→Counting
- **WasapiClock** (変更なし): WASAPI/QPC時計基盤

### 新規ファイル
- `adapter_netplay/timer/NetplayClock.hpp/.cpp` — 同期補正計算エンジン (~150行)

### 改修ファイル
- `SyncCoordinator.hpp/.cpp` — SyncMode列挙、READY(0x15)/START(0x16)パケット、NetplayClock統合、600Fベース疎通タイムアウト

### 削除ファイル (1,399行)
- `TimeSynchronizer.hpp/.cpp` (753行) — θ推定はNetplayClockに統合、3パケットハンドシェイク/Phase1/Phase2は廃止
- `VirtualClock.hpp/.cpp` (646行) — 全機能が呼び出し元なし

### 外部参照修正 (9箇所)
- PacketRouter.cpp/hpp, IpcData.hpp, SceneRunner.cpp, SceneLoading.hpp, SceneCharaSelect.cpp, State_Ui_Logic.hpp, NetplayManager.hpp, MainController.cpp

---



## 2026-03-02: 50F→120F、完了後NormalSpeed（描画ON）

### 変更内容
- `SceneCharaSelect.cpp`: FAST_BOOT_SKIP_FRAMES = 120 定数追加。50F→120Fに変更。
  120F完了後は Pause ではなく NormalSpeed に遷移（RenderSkip=false→描画ON）。

---

# refactor: ゲームメモリへのポーズフラグ書き込みを廃止

## 2026-03-02: CC_PAUSE_FLAG_ADDR 書き込みを全削除
通信スレッドの currentFrame が進まないことで一時停止が実現されるため、
ゲームメモリへの直接ポーズフラグ書き込み（CC_PAUSE_FLAG_ADDR）は不要。

### 変更内容
- `MbaaSpeedController.hpp`: SetGamePause()/SetGameResume() を完全削除。MaintainState() からポーズフラグ維持ロジック削除。
- `GameControl.hpp`: 未使用の SetPauseFlag() を削除。

---

# refactor: DxHookを純粋フック基盤に分離

## 2026-03-02: ImGui管理等をGameFrameOrchestratorに移動

### 設計原則
- DxHook.cpp: D3D9 EndScene/Present/Reset のフック設定＋コールバック呼出のみ
- GameFrameOrchestrator.cpp: ビジネスロジック（DLLロジック実行・ImGui描画・高速スキップ）

### 変更内容
- `DxHook.hpp`: コールバック型を `DeviceCallback(LPDIRECT3DDEVICE9)` に変更。ImGui関連メンバ削除。EndScene/Present/PresentSkip/PreReset/PostReset の5コールバック。
- `DxHook.cpp`: ImGui初期化・バックバッファ判定・InputHook初期化・MbaaSpeedController参照を全削除。フック関数はコールバック呼出＋元関数呼出のみ。
- `GameFrameOrchestrator.hpp`: Shutdown()追加。5つのコールバック関数（OnEndScene/OnPresent/OnPresentSkip/OnPreReset/OnPostReset）宣言。
- `GameFrameOrchestrator.cpp`: ImGui遅延初期化・バックバッファ判定・RenderSkipチェック・UIManager描画・Reset管理をDxHookから移動。
- `dllmain.cpp`: GameFrameOrchestrator::Shutdown()をDxHook::Shutdown()の前に追加。

---

# refactor: 高速モードをRenderSkip+TickBypassの2軸制御に刷新

## 2026-03-02: Sleep完全廃止 + DxHook描画スキップ + 通信スレッド周期バイパス

### 設計原則
- 高速モード: DxHook EndScene描画スキップ（RenderSkip=true） + 通信スレッド1F周期を待たない（TickBypass=true）
- 通常モード: SyncCoordinator の currentFrame 変化をポーリングして待機
- Sleep(0)/Sleep(1) のような無条件スリープは完全廃止

### 変更内容
- `MbaaSpeedController.hpp`: `RenderSkip()` / `TickBypass()` staticアトミックフラグ追加。SetMode() で高速モード時 ON、通常/Pause 時 OFF。
- `DxHook.cpp`: Hooked_EndScene 先頭で RenderSkip チェック → true なら ImGui + 描画をスキップして元 EndScene を即呼出。
- `GameControl.hpp`: SleepFrame() 内の Sleep(1) を削除。高速モード → 即リターン。通常モード → SyncCoordinator::GetState().currentFrame ポーリング（Sleep+Spin ハイブリッド）。

---

# refactor: TimeSynchronizer/VirtualClock参照を通信スレッドに限定（Phase 6）

## 2026-03-01: DLLスレッドからの旧コンポーネント参照を全削除
SyncCoordinator::GetState() の Read-only 参照に統一。

### 変更内容
- `GameControl.hpp`: IsSynced/IsSyncFailed/UpdateSync/ApplySyncOffset/ResetSync/GetRttUs/GetClockOffsetUs/UpdateLayer1FrameDuration を全削除。SleepFrameをSleep(1)に簡略化。Clock()/Sync()アクセサ削除。
- `MbaaSpeedController.hpp`: VirtualClock include・SetSkipMode() 呼び出し削除。GetCurrentMode() 追加。
- `SceneRunner.cpp`: VirtualClock::Initialize/GC::ResetSync/GC::UpdateSync の呼び出し全削除。旧GC::IsSyncedフォールバック削除。
- `SceneInGame.cpp`: TimeSynchronizer/VirtualClock include 削除。HandleRoundStartSync・ReadAndSend のGC同期メソッド→SyncCoordinator::GetState()。ApplyRelativeCorrection→no-op。フレーム計測動的調整→削除。VClock::QPCNowUs→ローカルQPCNowUs。
- `SceneCharaSelect.cpp`: TimeSynchronizer/VirtualClock include 削除。HandleTimeSyncWait/ReadAndSend→SyncCoordinator::GetState()。UpdateLayer1FrameDuration→削除。
- `SceneLoading.cpp`: TimeSynchronizer include 削除。Phase2同期待ち→SyncCoordinator::GetState()。Phase3ディレイ算出をctx.delayベースに変更。

---

# refactor: 旧同期パスの段階的整理（Phase 5）

## 2026-03-01: SyncCoordinator動作時の旧パス無効化
SyncCoordinator稼働時に旧TimeSynchronizer/VirtualClockの駆動パスをスキップ。

### 変更内容
- `SceneRunner.cpp`:
  - `VirtualClock::Initialize()` → SyncCoordinator未起動時のみ実行
  - `GC::UpdateSync()` ×2箇所 → SyncCoordinator動作時スキップ
  - 旧パスはフォールバックとして残存（完全削除はテスト確認後）

---

# refactor: DLLスレッド SyncCoordinator 統合（Phase 4）

## 2026-03-01: SceneRunner を SyncCoordinator ベースに統合
DLLスレッド（SceneRunner）の同期状態チェックを SyncCoordinator の Read-only 参照に切り替え。

### 変更内容
- `SceneRunner.cpp`:
  - SyncCoordinator.hpp を include
  - 同期完了判定: `syncState.isSynced` を優先、旧 `GC::IsSynced()` はフォールバック
  - 疎通チェック: `syncState.isPeerAlive` を優先、旧10秒タイムアウトも併用
  - 旧 `GC::IsSyncFailed()` チェックを廃止（SyncCoordinator が管理）

---

# refactor: SyncCoordinator 通信スレッドループ実装（Phase 3）

## 2026-03-01: 通信スレッドループ実装
SyncCoordinator の通信スレッドに受信処理・θ推定・疎通チェック・PING送信を実装。

### 変更内容
- `SyncCoordinator.hpp`: ReceivedPacket構造体、OnPacketReceived()、DrainAndProcessPackets()、
  UpdateThetaFromPacket()、SendPacket()、SendPing()、θ推定用リングバッファを追加
- `SyncCoordinator.cpp`: 通信スレッド全処理の実装
  - 受信キューdrain（mutex + vector swap）
  - θ推定: リングバッファ最大値フィルタ + σ安定判定(<1ms → isSynced)
  - 疎通チェック: 10秒タイムアウト → isPeerAlive=false
  - PING送信: 統一ヘッダのみ、200ms間隔
  - パケット送信: NetplayManager::GetUdpSocket()経由
- `PacketRouter.cpp`: OnPacket()先頭にSyncCoordinator.OnPacketReceived()転送を追加

---

# refactor: SyncCoordinator 基盤新設 + WasapiClock 独立抽出

## 2026-03-01: SyncCoordinator 基盤（Phase 2）
通信スレッドがティックマスターとなる新アーキテクチャの基盤を新設。

### 新規ファイル
- `adapter_netplay/SyncCoordinator.hpp/.cpp`
  - SharedSyncState（atomic フィールド群: currentFrame, isSynced, isPeerAlive, remoteInputs[20]）
  - 通信スレッドのティックループスケルトン（WASAPI 16666μs 可変周期）
  - CalcTickDuration(): ディレイ補正（最大18000μs）+ θ補正の2段変動
  - PushLocalInput(): DLLスレッドからのローカル入力送信キュー
- `adapter_netplay/timer/WasapiClock.hpp/.cpp`
  - TimeSynchronizer から WasapiClock クラスを独立ファイルに抽出
  - WASAPI IAudioClock 時刻取得 + QPC フォールバックの統合 GetTimeUs()

### 変更ファイル
- `adapter_netplay/timer/TimeSynchronizer.cpp`
  - 旧 WasapiClock クラス（~140行）を削除、新 WasapiClock.hpp に委譲
- `CMakeLists.txt`
  - WasapiClock.cpp, SyncCoordinator.cpp をビルド対象に追加

---

# refactor: FastBoot をゲームスレッド準拠化（裏スレッド廃止）

## 2026-03-01: FastBoot ゲームスレッド準拠化
裏スレッド（`CreateThread`）で動いていた FastBoot を廃止し、
ゲームスレッド（`SceneRunner::Step()`）で実行する `SceneFastBoot` に置換。

### 変更理由
- 裏スレッドとゲームスレッドのメモリ競合リスクを排除
- フレームスキップを `MbaaSpeedController::HighSpeedSkip_Normal` に統一（`CC_SKIP_FRAMES` 直書き廃止）
- 入力偽造を `GC::WriteInput()` に統一

### 変更ファイル
- [NEW] `SceneFastBoot.hpp/.cpp` — ゲームスレッド上の高速起動 Scene
- [MODIFY] `SceneRunner.cpp` — `phase < CharaSelect` 時に `SceneFastBoot::ProcessFrame()` をディスパッチ
- [MODIFY] `dllmain.cpp` — `FastBootRunner` の include と `Stop()` 呼び出しを削除
- [MODIFY] `CMakeLists.txt` — ソースリスト更新
- [DELETE] `FastBootRunner.hpp/.cpp` — 裏スレッド版を廃止（CMakeから除外、ファイルは残存）

---

# refactor: フレーム処理順序を最適化（入力→送信→Sleep→受信→ゲームロジック）

## 2026-03-01: フレーム処理順序の最適化
全4Sceneの `Update()` を `ReadAndSend()` + `ProcessFrame()` の2フェーズに分割し、
`SceneRunner::Step()` を最適フレーム処理順序に再構成。

### 変更理由
- 入力→送信の間にゼロ遅延で最低レイテンシを実現
- SleepFrameの~16msを受信バッファとして活用
- ゲームロジック実行時点で相手入力の到着確率を最大化

### 変更ファイル
- [MODIFY] `SceneCharaSelect.hpp/.cpp` — `ReadAndSend()` + `ProcessFrame()` に分割
- [MODIFY] `SceneInGame.hpp/.cpp` — `ReadAndSend()` + `ProcessFrame()` に分割
- [MODIFY] `SceneLoading.hpp/.cpp` — `ReadAndSend()` + `ProcessFrame()` に分割
- [MODIFY] `SceneRematch.hpp/.cpp` — `ReadAndSend()` + `ProcessFrame()` に分割
- [MODIFY] `SceneRunner.cpp` — Phase A(ReadAndSend)→SleepFrame→Phase B(ProcessFrame)→状態監視→中断チェック の順に再構成

---

# refactor: FastBoot起動の責務をdllmain.cppからSceneRunner::Init()へ移動

## 2026-03-01: FastBoot 起動責務の移動
- [MODIFY] `SceneRunner.cpp` — `Init()` 末尾 (`s_ready = true` の直前) で `FastBootRunner::Start()` を呼ぶように変更。ゲームロジック層が自らの初期化完了後に FastBoot を起動する設計へ。
- [MODIFY] `dllmain.cpp` — 初期化ステップ(8)の FastBoot 起動コードを削除。`DLL_PROCESS_DETACH` の安全停止 (`FastBootRunner::Stop()`) は維持。

---


## 2026-03-01: GameHooks 責務分割リファクタリング
- [DELETE] `adapter_netplay/GameHooks.hpp`, `GameHooks.cpp` — 5責務混在の巨大クラスを廃止
- [NEW] `game_memory_accessor/MbaaPatcher.hpp/.cpp` — NOP パッチ / キーボードクリア / 非アクティブ判定無効化（起動時1回適用）
- [NEW] `game_memory_accessor/FastBootRunner.hpp/.cpp` — メニュー自動遷移・イントロスキップ（裏スレッドループ）
- [NEW] `adapter_netplay/NetplayManager.hpp/.cpp` — UDP ソケット管理 + SendFunc 提供（GameHooksをリネーム＆絞り込み）
- [MODIFY] `dllmain.cpp` — 初期化シーケンスを MbaaPatcher → TimeHooks → NetplayManager → DxHook → SceneRunner → FastBootRunner の8段階に分離
- [MODIFY] `TimeSynchronizer.cpp` — GameHooks 参照（4箇所）を NetplayManager に置換
- [MODIFY] `CMakeLists.txt` — ソースリスト更新

---


## 2026-03-01: プロジェクトルートのドキュメントと運用環境の最適化
- [ADD] プロジェクトの顔となる `README.md` をルートディレクトリに新設。基本理念やビルド方法への導線を整備。
- [ADD] 開発者間で用語の定義ブレを防ぐためのドメイン用語集 `GLOSSARY.md` を新設（Rollup, CCTR, DummyPeerなど）。
- [ADD] 開発フローやコミットの厳格なルールを定めた参加ガイドライン `CONTRIBUTING.md` を新設。
- [MODIFY] `AI_WORKSPACE_GUIDE.md` が抱え込んでいた責務（概要やコミットルール等）を新設した標準ドキュメントに委譲し、内容をスリム化。
- [CHORE] ルートディレクトリに散乱していた一時ログファイル (`*.log`, `*.txt`) を `build_logs/` 配下へ一括移動し整理。
- [MODIFY] `.gitignore` を更新し、`docs/` および `src/` 配下を除く `*.txt` ファイルがプロジェクトルートにコミット・生成されないように追記。

---


## 2026-03-01: 古い変更履歴の削除・設計資料のアーカイブ化・要件仕様書の同期
- [DELETE] 旧運用ルールに基づいていた `docs/changelogs/` 以下のすべてのディレクトリおよびファイルを削除（全140ファイル）。今後はルートの `changelogs/` で一元管理。
- [MOVE] `docs/design/` 配下に散在していた過去の日付付き意思決定メモや作業方針書 (`2026-*.md`) を `docs/archive/design/` へ一括退避（全27ファイル）。
- [MODIFY] `docs/requirements/` 内に存在していた未登録の仕様書 (`06_packet_specification.md` 〜 `12_packet_field_specification.md`) を精査し、正式な要件であると確認したため、`AI_WORKSPACE_GUIDE.md` の目次ツリー記述を同期して更新。
