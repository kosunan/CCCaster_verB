# CCCaster_v10 バグ・問題管理リスト

> 最終更新: 2026-02-27  
> 本リストはコードベース調査・テスト実行結果・ロードマップ照合により作成。

---

## 凡例

| ラベル | 意味 |
|---|---|
| 🔴 **Critical** | 対戦機能に直結。早期対応必須 |
| 🟠 **High** | 品質や安定性に影響大 |
| 🟡 **Medium** | 機能欠損だが回避手段あり |
| 🟢 **Low** | 改善推奨。緊急性なし |
| ⚪ **Info** | 仕様制約・参考情報 |

---

## A. コードベース TODO (未実装機能)

### A-1. 🔴 キャラセレ同期 — リモート入力受信未実装
- **ファイル**: `src/domain/scene/SceneCharaSelect.cpp` (L200)
- **内容**: `remoteInputBtn = 0` のまま固定。リモート入力受信処理が未実装。
- **影響**: Phase 4-1 完了のブロッカー。対戦相手のキャラセレ操作が反映されない。
- **対応**: ネットワークからのリモート入力パケット受信→反映パイプラインの実装。

### A-2. 🔴 キャラセレ同期 — Phase 1.5 PCスペック交渉未実装
- **ファイル**: `src/domain/scene/SceneCharaSelect.cpp` (L12, L16, L132)
- **内容**: PC性能ベンチマーク実行→maxRollback値の決定交渉が未実装。
- **影響**: ロールバック深度がハードコーディングされ、低スペックPCでフレーム落ちのリスク。
- **対応**: ベンチマーク→ネゴシエーション→共有の3ステップ実装。

### A-3. 🟡 キャラセレ同期 — 3Fバッファガード（連決定スキップ防止）未実装
- **ファイル**: `src/domain/scene/SceneCharaSelect.cpp` (L40, L90)
- **内容**: `hasButtonInHistory()` 未実装。高速連打によるキャラ確定スキップを防止するフィルタが欠如。
- **影響**: 高遅延環境でキャラ選択がスキップされる可能性。
- **対応**: `InputFilter` へ3Fバッファガードロジック追加。

### A-4. 🟡 InputFilter — 旧仕様の決定・キャンセル封印ロジック未移植
- **ファイル**: `src/domain/sync/InputFilter.cpp` (L27)
- **内容**: カーソル移動直後2F間の決定・キャンセル封印（旧CCCaster仕様）が未移植。
- **影響**: カーソル操作直後の誤入力が発生する可能性。
- **対応**: 旧CCCasterの `historyCheck` ロジックを `InputFilter` に移植。

### A-5. 🟠 Loading画面 — 入力送受信パイプライン未実装
- **ファイル**: `src/domain/scene/SceneLoading.cpp` (L125-126)
- **内容**: `SendLoadingInput()` / `GetLatestRemoteLoadingInput()` がコメントアウト状態。
- **影響**: Loading→InGame遷移の同期が不完全。
- **対応**: Phase 4-3（ラウンド開始時同期）の一部として実装。

### A-6. 🟠 Rematch画面 — メニュー選択同期未実装
- **ファイル**: `src/domain/scene/SceneRematch.cpp` (L37, L279, L284)
- **内容**: `SendMenuIndex()` / リモート選択受信がコメントアウト状態。AsmHacksモジュール未統合。
- **影響**: 再戦/キャラセレ戻り等のメニュー選択が同期されない。
- **対応**: AsmHacks統合後にメニュー同期パイプラインを実装。

---

## B. テスト実行で検出された問題

### B-1. 🟢 DummyPeer — WASAPI初期化失敗 (QPC fallback)
- **ログ**: `[SyncResponder] WASAPI: FAILED (QPC fallback)` — 全シナリオで発生
- **内容**: DummyPeer単体ではWASAPIオーディオデバイスが利用不可のため、QPCにフォールバック。
- **影響**: テスト精度への実質的影響は小さい。DLL側（ゲーム内プロセス）では正常動作の可能性が高い。
- **対応**: DummyPeer側はQPCフォールバックで問題なし。必要に応じて警告メッセージを抑制。

### B-2. ⚪ DummyPeer — Frame Received = 0 / Rollback Count = 0
- **ログ**: `Total Received: 0` / `Rollback Count: 0` — 全シナリオ共通
- **内容**: DummyPeer単体テストでは相手DLLが存在しないため、フレーム受信・ロールバックが発生しない。
- **影響**: なし（想定通りの動作）。
- **対応**: DLL結合テスト時に再検証。項目 C-3 参照。

### B-3. 🟢 VirtualClock — WASAPI異常ジャンプ警告
- **ファイル**: `src/core/timer/VirtualClock.cpp` (L125)
- **内容**: WASAPIクロックが異常ジャンプした場合の `WARNING` ログ出力が実装済み。
- **影響**: 実機テスト中に稀に発生する可能性がある。検出はできるが回復処理の妥当性は未検証。
- **対応**: ゲーム統合テストで実際の発生頻度と回復動作を検証。

---

## C. DummyPeerテスト基盤 未実装項目

> ソース: `tools/dummy_peer/TESTING_IMPROVEMENTS.md`

### C-1. 🟠 双方向同時通信テスト未対応 (#1)
- **内容**: `SyncResponder` が受動応答のみ。能動的にSYNC_REQを送信する `InitiateSync()` が未実装。
- **影響**: 本体DLL←→DummyPeer間の双方向同期の品質検証ができない。

### C-2. 🟡 WASAPIクロック実機異常テスト不可 (#3)
- **内容**: DLL側の `VirtualClock` にテスト用のクロックジャンプ注入フック (`InjectTestJump()`) がない。
- **影響**: クロック異常時の耐性を自動テストできない。

### C-3. 🔴 RollbackEngine結合テスト皆無 (#4)
- **内容**: RollbackEngine本体が未完成のため、ロールバック動作の結合テストが不可能。
- **前提条件**: Phase 4-4（高精度ロールバック同期）の実装完了。

### C-4. 🟡 切断・再接続のランタイム実装 (#6)
- **内容**: `--disconnect-at` / `--reconnect-after` のCLI引数は追加済みだが、実際の `closesocket` → 再バインド処理が未実装。
- **影響**: ネットワーク断→復帰シナリオのテストができない。

### C-5. 🟡 ロングランテストシナリオ未整備 (#10)
- **内容**: ドリフト蓄積検証には30秒では不十分。数分～数十分レベルのテストシナリオが必要。
- **影響**: 長時間対戦でのドリフト補正精度が未検証。

### C-6. ⚪ NAT越え・ファイアウォール環境テスト (#9)
- **内容**: LAN内テストのみ。NAT越え(STUN/TURN)やファイアウォール環境は未テスト。
- **影響**: DummyPeerの責務外。別途テスト環境構築が必要。

---

## D. ロードマップ進行に伴う未着手項目

### D-1. 🔴 Phase 4-1: キャラクターセレクト同期 (進行中)
- A-1, A-2, A-3 が未完了。

### D-2. ⬜ Phase 4-2: オーバーレイ機能実装 (未着手)
- ImGuiオーバーレイでの設定変更・通信ステータス表示。

### D-3. 🟠 Phase 4-3: ラウンド開始時同期 (進行中)
- A-5（Loading入力同期）、A-6（Rematchメニュー同期）が未完了。

### D-4. 🟠 Phase 4-4: 高精度ロールバック同期 (進行中)
- RollbackEngine本体の実装・チューニング。C-3（結合テスト）のブロッカー。

---

## E. ユーザー報告の問題・要望

### E-1. 🔴 ローカルテストが行えない（ハッシュ接続方式の制約）
- **内容**: 現在の接続方式はグローバルIPベースのハッシュで待ち受けるため、ローカル環境（127.0.0.1）での手動テストができない。ヘッドレスモード（`--ip 127.0.0.1`）は機能するが、対話モードではハッシュ生成がグローバルIP前提となり、LAN/ループバック接続を手動で開始できない。
- **影響**: 開発中の手動デバッグ・動作確認が困難。AI自動テスト（ヘッドレス）以外のテスト手段がない。
- **対応案**:
  - 対話モードに「Direct IP接続」メニューを追加（ハッシュを介さず直接IP:Port指定）
  - または `--local` フラグで 127.0.0.1 ベースのハッシュ生成に切り替え

### E-2. 🔴 クリップボードにハッシュではなくグローバルIPがコピーされる
- **内容**: ホスト起動時、クリップボードにコピーされるのがConnection HashではなくグローバルIP（例: `92.203.30.69:10800`）になっている。ログ上は `Hash copied to clipboard` と表示された後に `Global IP ... copied to clipboard` で上書きされている。
- **影響**: ユーザーがハッシュを相手に共有しようとしてもIPが貼り付けられる。ハッシュ接続の運用が成り立たない。
- **対応**: クリップボードへのコピー処理を整理し、ハッシュのみをコピーするか、IP取得後の上書きを防止する。

### E-3. 🟠 自動アップデート機能の追加
- **内容**: 対戦接続時にバージョン不一致を検出し、遅れている側がGitリポジトリから最新版を自動ダウンロード・更新する機能が必要。
- **影響**: バージョン不一致による対戦不可・デシンクの防止。ユーザーの手動更新負荷の軽減。
- **対応案**:
  - SessionNegotiator のハンドシェイクにバージョン情報（ビルドハッシュ or セマンティックバージョン）を含める
  - バージョン不一致検出時にGit pull → 再ビルド or バイナリダウンロードを自動実行
  - 更新完了後に自動再起動→再接続
- **設計要件**: 更新中の進捗表示、更新失敗時のロールバック、セキュリティ（署名検証）

### E-4. 🟠 パケットフォーマットの統一・改善
- **内容**: 現在は画面（CharaSelect / Loading / InGame / Rematch）ごとに異なるパケットフォーマットを使用しており、DummyPeerとの整合性確保が煩雑。パケット定義を統一プロトコルに集約し、DummyPeer側の対応も簡素化する。
- **影響**: DummyPeerのテストモード追加時に毎回パケット対応が必要。コードの見通しが悪い。
- **対応案**:
  - 統一パケットヘッダ（PacketType + Phase + Sequence）を定義
  - 各画面のペイロードはヘッダ配下のunion/variantで切り替え
  - `tools/dummy_peer/include/UnifiedProtocol.hpp` の既存定義をベースに本体DLL側へも導入
  - DummyPeerの `PhaseHandler` と本体DLLの各Sceneで同一プロトコル定義を共有

---

### E-5. ✅ ~~FPS異常 — 常時加速状態（高精度スリープ不動作）~~
- **発見**: 2026-02-27 E2Eテスト目視確認
- **現象**: ゲームが常時高速動作し、フレームレートが異常値になる。描画は正常。
- **原因**: `VirtualClock::WaitForNextFrame` が `_lastFrameTimeUs` に実時刻を代入していたため、処理時間が毎フレーム加算されFPS低下。
- **修正**: `_lastFrameTimeUs` に目標時刻(targetTime)を代入するよう変更。コミット `2913531`。
- **ステータス**: ✅ 修正済み (2026-02-27)

### E-6. 🟠 ダミー入力が正常に反映されていない
- **発見**: 2026-02-27 E2Eテスト目視確認
- **現象**: DummyPeerからCS_INPUTを送信しているが、ゲーム内カーソルが動かない。
- **推定原因**: `SceneCharaSelect::SetRemoteInput()` で受信した値を `DirectInputWriter` 経由でゲームに書き込む処理、またはゲームメモリへの書き込みアドレスが不正。
- **影響**: 🟠 リモート入力同期が機能しない（キャラセレ同期の本質部分）。
- **調査先**: `src/domain/scene/SceneCharaSelect.cpp`、`src/core/memory/DirectInputWriter`
- **対応**: 受信値のゲームメモリ書き込みパスを実機デバッグで確認。
- **ステータス**: 🆕 未対応

### E-7. 🔴 F4オーバーレイ起動 → キー設定でクラッシュ
- **発見**: 2026-02-27 目視確認
- **現象**: インゲームでF4を押してImGuiオーバーレイを起動後、キー設定操作を行うとゲームがクラッシュする。
- **推定原因**: `ControllerMapper::Draw()` 内のキー入力処理中に不正アクセスまたはスレッド競合が発生している可能性。`GameHooks.cpp` のImGui入力ハンドラとの競合も疑われる。
- **影響**: 🔴 オーバーレイUI機能が実質使用不可。設定変更ができない。
- **調査先**: `src/core/overlay/ControllerMapper.cpp`、`src/core/hooks/GameHooks.cpp`
- **対応**: クラッシュ時のスタックトレースを取得し、原因箇所を特定。
- **ステータス**: 🆕 未対応

### E-8. ✅ ~~SleepFrame が各 Scene::Update() に分散 — フレームタイミング制御が二重構造~~
- **発見**: 2026-02-27 コード調査
- **現象**: SceneRunner のメインループ内で各 Scene::Update() にディスパッチした後、Scene 内部でも `GC::SleepFrame()` を呼んでいる。SceneRunner 側にも Gate1/Gate2/default の3箇所に SleepFrame があり、フレーム待機が散在。
- **影響**:
  - フレームタイミングの制御責任が SceneRunner と各 Scene に分散し、見通しが悪い
  - Scene 内部ループで複数回 SleepFrame を呼ぶケースがあり、1回の Update() で複数F消費する
  - 今後の高速モード/通常モード切替の一元管理が困難
- **対応**: SceneRunner ループの先頭1箇所に「高速 or 通常」の分岐付き SleepFrame を集約し、各 Scene::Update() からは SleepFrame を除去。
- **ステータス**: ✅ 修正済み (2026-02-27)

### E-9. ✅ ~~SceneInGame::Update が SendFunc を受け取っていない~~
- **発見**: 2026-02-27 フレームパイプライン調査
- **現象**: 他の全Scene（CharaSelect, Loading, Rematch）は `SendFunc` を受け取っているが、SceneInGame のみ未対応。対戦中の入力パケット送信が不可能。
- **対応**: `SceneInGame::Update` / `ProcessRollbackFrame` に `SendFunc` 引数を追加。
- **ステータス**: ✅ 修正済み (2026-02-28, Fix-2, `67b644e`)

### E-10. ✅ ~~OnRemoteInputPacket にフレーム番号がない — s_latestRemoteFrame 書込み未実装~~
- **発見**: 2026-02-27 フレームパイプライン調査
- **現象**: `OnRemoteInputPacket(uint16_t)` が入力値のみ受け取り、フレーム番号を `s_latestRemoteFrame` に書き込んでいない。RollbackEngine がリモート入力を正しいフレームスロットに格納できない。
- **影響**: 🔴 ロールバック判定が不正確になる（常に lastConfirmed+1 に格納）。
- **対応**: シグネチャを `OnRemoteInputPacket(uint32_t frame, uint16_t input)` に変更し、`s_latestRemoteFrame.store()` を追加。
- **ステータス**: ✅ 修正済み (2026-02-28, Fix-2, `67b644e`)

### E-11. ✅ ~~GAME_INPUT 送信が簡易版（6B） — 11F冗長化未実装~~
- **発見**: 2026-02-28 Fix-2 実装時
- **現象**: DLL側の GAME_INPUT 送信は `{ frame(4B), input(2B) }` = 6B 簡易ペイロードのみ。DummyPeer 側は 61B の冗長化ペイロード（11F 入力履歴 + roundTimer + wasapiClock）を送信しており、DLL↔DummyPeer 間でペイロードが非対称。
- **対応**: DLL側にも入力リングバッファ（11F分）を追加し、`GameInputPayload` 完全版(61B)を送信するよう拡張。`PacketRouter.cpp` の受信構造体も完全版に差し替え。
- **ステータス**: ✅ 修正済み (2026-02-28, E-11, `8407006`)

### E-12. ✅ ~~リモート入力の atomic 単一値保持 — 複数フレーム分の入力上書き問題~~
- **発見**: 2026-02-27 フレームパイプライン調査
- **現象**: `s_latestRemoteInput` / `s_latestRemoteFrame` が `atomic<uint16_t>` の単一値。PacketRouter スレッドが高速に複数パケットを受信した場合、メインスレッドが読み取る前に古い入力が上書きされる。
- **対応**: SPSC ロックフリーリングバッファ（`RemoteInputQueue`、256エントリ）を導入。PacketRouter で history[11] 全展開→push、メインスレッドで queue drain → RE 入力。`s_latestRemoteInput` を廃止。
- **ステータス**: ✅ 修正済み (2026-02-28, E-12, `f2534f1`)

### E-13. 🟡 GAME_INPUT に lastConfirmed / checksum フィールドがない
- **発見**: 2026-02-28 Fix-2 実装計画策定時
- **現象**: 現在の GAME_INPUT パケットに `lastConfirmed`（相手の確認済みフレーム番号）と `checksum`（Desync検出用チェックサム）が含まれていない。
- **影響**:
  - `lastConfirmed` がないと、相手側で古い状態の破棄ができず、StateRingBuffer のメモリ使用量が増加する
  - `checksum` がないと、Desync（同期ずれ）の検出ができない
- **対応**: ロールバック基盤の安定後（Phase 4-4）に、`lastConfirmed(4B)` + `checksum(4B)` をペイロードに追加。
- **ステータス**: 🆕 未対応

---

## 統計サマリ

| 重要度 | 件数 |
|---|---|
| 🔴 Critical | 7 (E-5,E-10 修正済み) |
| 🟠 High | 10 (E-9 修正済み) |
| 🟡 Medium | 6 |
| 🟢 Low | 2 |
| ⚪ Info | 2 |
| ✅ 修正済み | 4 (E-5, E-8, E-9, E-10) |
| **合計** | **28** |
