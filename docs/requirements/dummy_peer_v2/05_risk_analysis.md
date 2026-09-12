# DummyPeer v2 要件定義書 — 05. 実務リスク分析（不安判断材料 10項目）

> **文書ID**: REQ-DP2-005  
> **作成日**: 2026-02-27  
> **入力ソース**:  
> - `packet_analysis.md` (会話 244e6033: RedundantProtocol解析含む)  
> - `04_packet_analysis_report.md` (会話 c61f1715: DLL全コード解析)

---

## 不安判断材料 10項目

---

### 🔴 #1: DLLから入力パケットを一切送信していない — 対戦が片方向通信

**事実**  
DLL内で `UdpSocket::Send()` を呼ぶのは `TimeSynchronizer.cpp` の3箇所のみ（SYNC_REQ/RES/DONE）。
ゲーム入力（ボタン・方向キー）をネットワーク越しに送信するコードが**存在しない**。

**不安**  
DummyPeerが2B入力パケットを送り、DLLの `PacketRouter` が受信して `s_latestRemoteInput` に格納する経路は動作するが、**逆方向（DLL→相手）が空振り**。
現状のテストは「DummyPeerからDLLへの一方通行」でしか検証できず、**本番のネット対戦に必要な双方向入力交換が全く成り立たない**。

**対策案**  
SceneInGame / SceneCharaSelect から `UdpSocket::Send()` で GAME_INPUT / CS_INPUT を60fps送信するコードの実装が**DummyPeer改修以前の前提条件**。

---

### 🔴 #2: RedundantProtocol (CCTR 51B) が完全孤立 — せっかくの冗長化が使われない

**事実**  
`RedundantProtocol.hpp/.cpp` に `EncodeInput()` / `DecodeInput()` が実装済み。  
51バイト固定パケットに `frameId`, `currentInput`, `roundTimer`, `wasapiClock`, `historyInputs[10]` を格納する。  
しかし**どの Scene からも呼び出されておらず**、PacketRouter にもデコード処理がない。

**不安**  
ロールバックエンジンは `frameId` で「どのフレームの入力か」を特定する必要がある。
現在の2B入力パケットには `frameId` がなく、RollbackEngineが正しく機能する保証がない。
さらにパケロス時に `historyInputs` で復元する仕組みも死蔵している。

**対策案**  
RedundantProtocol を PacketRouter に接続するか、または UnifiedProtocol の GAME_INPUT ペイロードとして再設計し、Scene から呼び出すパスを確立する。

---

### 🟠 #3: PacketRouter の分岐が貧弱 — 新パケット種別を追加できない

**事実**  
PacketRouter は2つの分岐しか持たない:  
1. `data[0] == 0x10-0x12` → TimeSynchronizer  
2. `data.size() == 2` → OnRemoteInputPacket  
3. それ以外 → `UNKNOWN` ログ

**不安**  
DummyPeer v2 が統一ヘッダ(20B)付きパケットや、LOADING_INPUT, REMATCH_MENU, STATE_REPORT を送信しても、**PacketRouter が全て「UNKNOWN」として破棄**する。
新パケット種別を追加するには PacketRouter を改修する必要があるが、これを行うと**既存のTimeSyncやDummyPeerの2B入力との後方互換**が崩れるリスクがある。

**対策案**  
PacketRouterにマジックナンバー(`CC10`)判定を追加し、統一ヘッダが付いたパケットは新パスで処理、旧形式(typeバイト先頭 or 2Bサイズ)は既存パスで処理する二重ディスパッチ方式。

---

### 🟠 #4: Phase 2 相対補正の入力データが未接続 — 長時間プレイでクロックが乖離

**事実**  
`TimeSynchronizer::OnReceivePeerClockReport()` は定義済みだが、入力パケットに `wasapiClock` を載せて送る仕組みが**繋がっていない**。
RedundantProtocol の `wasapiClock` フィールドも孤立。

**不安**  
ラウンド進行中にCPU温度変化等でQPC/WASAPIクロックの微少ドリフトが蓄積する。
5分以上のセッションで数ms〜数十msのフレームズレが発生しうるが、**相対補正がデータなしで動作しない**。
結果として長時間プレイ時に「自分だけ遅れる」or「カクつく」症状が発生する可能性。

**対策案**  
GAME_INPUT パケットに `wasapiClock` (または `localDelayUs`) を付加し、`OnReceivePeerClockReport()` に渡す経路を実装する。

---

### 🟠 #5: キャラセレで相手の入力が見えない — 選択が同期しない

**事実**  
`SceneCharaSelect.cpp` L200: `uint16_t remoteInputBtn = 0; // TODO: リモート入力受信実装`  
相手のキャラ選択・ムーン選択がローカルに反映されず、**常に相手が「何も押していない」状態**。

**不安**  
キャラセレ画面で両者が独立にキャラを選ぶため、Loading遷移タイミングがバラバラになる。
片方がキャラ確定済みでも相手側は未確定と認識し、**デッドロック（永久待ち）**に陥る可能性。
3種フィルタ(A/B/C)も相手入力なしでは正しく動作検証できない。

**対策案**  
CS_INPUT の送受信実装 + PacketRouter に CS_INPUT ルーティング追加。DummyPeerはこの新パケットに対応する受信・応答を実装。

---

### 🟠 #6: Loading画面のディレイ計算が相手不在 — ラグを正しく算出できない

**事実**  
`SceneLoading.cpp` L125-126: `SendLoadingInput` / `GetLatestRemoteLoadingInput` が共にTODO。  
ディレイ値は RTT ベースで自動算出する設計だが、**ディレイ入力を相手と交換する手段がない**。

**不安**  
ディレイ値（`s_delaySyncFrames`）は片方のローカル計算だけで決まり、**相手と値が食い違う**リスク。
ディレイが不一致の状態でInGameに遷移すると、入力のリングバッファ位置がずれてDesyncに直結する。

**対策案**  
LOADING_INPUT パケットで delay 値を交換し、`max(local, remote)` で統一する処理が必要。

---

### 🟡 #7: Rematchのメニュー同期が未実装 — リマッチ後の遷移先が不一致

**事実**  
`SceneRematch.cpp` L279: `// TODO: SendMenuIndex`, L284: `// TODO: リモート選択受信`。  
`max(local, remote)` で遷移先を決定するロジックは実装済みだが、相手の選択が届かない。

**不安**  
ローカルが「リマッチ(0)」を選んでも相手が「キャラセレ(1)」を選んだ場合、`max` により本来キャラセレに行くべき。
しかし `s_remoteRetryMenuIndex` が `-1`(未選択) のまま永久待ちになるか、片方だけが遷移して**画面不一致→デスシンク**。

**対策案**  
REMATCH_MENU パケットの送受信実装。DummyPeer側は `SetRemoteRetryMenuIndex()` 相当のパケットハンドリングが必要。

---

### 🟡 #8: UdpSocketの再バインド時「ポート競合」未処理 — 起動失敗

**事実**  
CLI側の `SessionNegotiator` で使ったUdpSocketは破棄され、DLL側 `GameHooks.cpp` L167で同じポートを再バインドする。
しかしOSのソケットクローズは非同期のため、**TIME_WAIT状態のソケットが残っている可能性**がある。

**不安**  
再バインド失敗時のログは出るが（L173）、**リトライもフォールバックもない**。
ユーザーから見ると「接続成功→ゲーム起動→通信不通」という最悪のUXになる。

**対策案**  
`SO_REUSEADDR` オプションの適用、またはDLL側でのバインドリトライ（3回×500ms間隔等）の実装。

---

### 🟡 #9: DummyPeerで統一ヘッダ(20B)を先行導入すると現行DLLと通信不能

**事実**  
DummyPeer v2 の設計では UnifiedPacketHeader (20B, magic='CC10') を採用する方針。  
しかし現行DLLの PacketRouter は `data[0]` で SYNC 判定、`data.size() == 2` で入力判定しかしない。

**不安**  
DummyPeerが20Bヘッダ付きパケットを送ると、PacketRouter は `data[0]` = `'C'` (0x43) を見て「UNKNOWNパケット」としてログに捨てる。
DLL改修なしにDummyPeer v2を先行リリースすると、**既存の `--test-mode game` が壊れる**。

**対策案**  
DummyPeer v2は `--test-mode game`（旧互換）と `--test-mode e2e`（新プロトコル）を完全分離し、旧モードでは従来の2B/9B/25Bパケットをそのまま使用する。新プロトコルはDLL側PacketRouter改修後に有効化する。

---

### 🟡 #10: Phase 1.5 (PCスペック交換) 未実装 — maxRollback が最適化されない

**事実**  
`SceneCharaSelect.cpp` L132: `// TODO [Phase 1.5]: PCスペックベンチマーク実装`。  
maxRollback は現在 `SessionContext` の初期値（固定値）のまま。

**不安**  
低スペックPC同士では、maxRollback が大きすぎると1F以内に巻き戻し再生が完了せず**カクつく**。
逆に高スペックPC同士では小さすぎて、パケロス1回で**フリーズ（入力待ちストール）**が発生する。
特にクロスプラットフォーム（高スペック vs 低スペック）対戦で深刻な品質劣化のリスク。

**対策案**  
Phase 1.5 でゲームフレーム処理のベンチマーク結果をパケットで交換し、`min(自分, 相手) - マージン` で maxRollback を自動決定する。当面は CLI の設定画面で手動設定できるUIを提供。
