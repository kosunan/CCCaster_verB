# DummyPeer v2 要件定義書 — 04. パケット使用状況 コード解析レポート

> **文書ID**: REQ-DP2-004  
> **作成日**: 2026-02-27 (更新: 2026-02-27 作業C分追記)  
> **ステータス**: 解析完了 → 対応方針決定済み

---

## 1. 解析目的

ゲーム中に**必須となるパケット**と**現在実際に使われているパケット**を、本番コードから網羅的に特定する。

---

## 2. 解析結果サマリ

### 現在実装済みパケット（★ 実際にコードが動作するもの）

| # | パケット名 | サイズ | 方向 | 送信箇所 | 受信箇所 | フェーズ |
|---|---|---|---|---|---|---|
| ★1 | Negotiation Ping-Pong | 26B | 双方向 | `SessionNegotiator.cpp` L244-268 | `SessionNegotiator.cpp` L156-209 | ゲーム起動前 (CLI側) |
| ★2 | SYNC_REQ (0x10) | 9B | 双方向 | `TimeSynchronizer.cpp` L288-299 | `TimeSynchronizer.cpp` L383-408 | CharaSelect,Loading,InGame |
| ★3 | SYNC_RES (0x11) | 25B | 双方向 | `TimeSynchronizer.cpp` L390-408 | `TimeSynchronizer.cpp` L411-430 | CharaSelect,Loading,InGame |
| ★4 | SYNC_DONE (0x12) | 9B | 双方向 | `TimeSynchronizer.cpp` L323-344 | `TimeSynchronizer.cpp` L432-457 | CharaSelect,Loading,InGame |
| ★5 | 2バイト入力パケット | 2B | **受信のみ** | **未実装** | `PacketRouter.cpp` L26-32 | 全画面 |

### 未実装パケット（TODO状態）

| # | パケット名 | 必要フェーズ | TODO箇所 | 備考 |
|---|---|---|---|---|
| ❌1 | 入力パケット送信 | 全画面 | **送信コードなし** | DLL→相手への入力送信が存在しない |
| ❌2 | リモート入力受信（CharaSelect） | CharaSelect | `SceneCharaSelect.cpp` L200 | `uint16_t remoteInputBtn = 0; // TODO: リモート入力受信実装` |
| ❌3 | Loading入力送信 | Loading | `SceneLoading.cpp` L125 | `// TODO: SendLoadingInput(filteredLocal, s_delaySyncFrames);` |
| ❌4 | Loading入力受信 | Loading | `SceneLoading.cpp` L126 | `// TODO: s_remoteInput = GetLatestRemoteLoadingInput();` |
| ❌5 | Rematchメニュー送信 | Rematch | `SceneRematch.cpp` L279 | `// TODO: SendMenuIndex(ctx.transitionIndex, s_localRetryMenuIndex);` |
| ❌6 | Rematchメニュー受信 | Rematch | `SceneRematch.cpp` L284 | `// TODO: リモート選択受信` |
| ❌7 | Phase 1.5 PCスペック交換 | CharaSelect | `SceneCharaSelect.cpp` L132 | `// TODO [Phase 1.5]: PCスペックベンチマーク実装` |
| ❌8 | Filter C (3Fバッファガード) | CharaSelect | `SceneCharaSelect.cpp` L40,90 | `// TODO: hasButtonInHistory() 未実装` |

---

## 3. パケット経路の完全なフロー図

```
┌──────────────────────────────────────────────────────────────────┐
│ CLI (main_app) — ゲーム起動前                                     │
│                                                                    │
│  SessionNegotiator.cpp                                            │
│    UdpSocket (Port指定)                                           │
│    ├─ Send: 26B Ping-Pong (L244-268)                             │
│    └─ OnReceive: 26B Ping-Pong 解析 (L156-209)                  │
│                                                                    │
│  成功 → NegotiationResult { peerIp, peerPort, localPort }         │
│       → MainController がゲーム起動パラメータとしてDLLに渡す      │
│                                                                    │
│  ★ この UdpSocket はゲーム起動後に破棄される                      │
│    DLL側は同じポートで新しい UdpSocket を作成する                 │
└──────────────────────────────────────────────────────────────────┘
                              ↓ (localPort 引き継ぎ)
┌──────────────────────────────────────────────────────────────────┐
│ DLL (core_dll) — ゲーム中                                         │
│                                                                    │
│  GameHooks.cpp                                                    │
│    UdpSocket (localPort で再バインド, L167)                       │
│    └─ OnReceive → PacketRouter::OnPacket() (L168-169)            │
│                                                                    │
│  PacketRouter.cpp (L14-38)                                        │
│    受信ルーティング:                                               │
│    ├─ data[0] == 0x10-0x12 → TimeSynchronizer.OnReceiveSyncPacket │
│    ├─ data.size() == 2     → OnRemoteInputPacket (SceneRunner)    │
│    └─ それ以外             → "[PacketRouter] UNKNOWN" ログ出力    │
│                                                                    │
│  TimeSynchronizer.cpp — 送信 (DLL内で唯一のSend呼び出し)          │
│    ├─ SYNC_REQ (0x10): [type 1B][T1 8B] = 9B   (L288-299)       │
│    ├─ SYNC_RES (0x11): [type 1B][T1+T2+T3 24B] = 25B (L390-408) │
│    └─ SYNC_DONE (0x12): [type 1B][startTime 8B] = 9B (L323-344)  │
│                                                                    │
│  SceneCharaSelect.cpp — 入力送受信 ❌ 未実装                      │
│    L200: remoteInputBtn = 0; // TODO                              │
│                                                                    │
│  SceneLoading.cpp — 入力送受信 ❌ 未実装                          │
│    L125: // TODO: SendLoadingInput                                │
│    L126: // TODO: GetLatestRemoteLoadingInput                     │
│                                                                    │
│  SceneInGame.cpp — 入力送受信                                      │
│    L161-162: remoteInput/remoteFrame は atomic 経由で PacketRouter │
│             から受信（★ 受信のみ実装済み）                        │
│    送信: ❌ 未実装（DLLからの入力送信コードがない）                │
│                                                                    │
│  SceneRematch.cpp — メニュー送受信 ❌ 未実装                      │
│    L279: // TODO: SendMenuIndex                                   │
│    L284: // TODO: リモート選択受信                                │
└──────────────────────────────────────────────────────────────────┘
```

---

## 4. 本番で必須なパケット一覧（実装済み + 未実装）

### 4.1 ゲーム起動前 (CLI)

| パケット | サイズ | 状態 | 必須度 |
|---|---|---|---|
| Negotiation Ping-Pong | 26B | ★ 実装済み | 必須 |

### 4.2 CharaSelect 画面

| パケット | サイズ | 状態 | 必須度 |
|---|---|---|---|
| SYNC_REQ | 9B | ★ 実装済み | 必須 |
| SYNC_RES | 25B | ★ 実装済み | 必須 |
| SYNC_DONE | 9B | ★ 実装済み | 必須 |
| CS_INPUT（キャラセレ入力送信） | 2B+ | ❌ 未実装 | **必須** |
| CS_INPUT（キャラセレ入力受信） | 2B | △ 部分実装 | **必須** |
| PCスペック交換 | 未定 | ❌ 未実装 | 中（Phase 1.5） |

### 4.3 Loading 画面

| パケット | サイズ | 状態 | 必須度 |
|---|---|---|---|
| SYNC_REQ/RES/DONE | 9/25/9B | ★ 実装済み | 必須 |
| LOADING_INPUT（送信） | 未定 | ❌ 未実装 | **必須** |
| LOADING_INPUT（受信） | 未定 | ❌ 未実装 | **必須** |

### 4.4 InGame 画面

| パケット | サイズ | 状態 | 必須度 |
|---|---|---|---|
| SYNC_REQ/RES/DONE | 9/25/9B | ★ 実装済み | 必須 |
| GAME_INPUT（送信） | 2B+ | ❌ 未実装 | **必須** |
| GAME_INPUT（受信） | 2B | ★ 実装済み（atomic経由） | 必須 |

### 4.5 Rematch 画面

| パケット | サイズ | 状態 | 必須度 |
|---|---|---|---|
| REMATCH_MENU（送信） | 未定 | ❌ 未実装 | **必須** |
| REMATCH_MENU（受信） | 未定 | ❌ 未実装 | **必須** |

---

## 5. 重大な発見事項

> [!CAUTION]
> **DLLから相手への入力パケット送信が一切実装されていない**。
> PacketRouter は 2バイト入力パケットの受信処理を持っているが、
> DLL側に対応する送信コードが存在しない。
> これは現在のCCCaster v10がまだ「入力交換」の段階に到達していないことを意味する。

> [!WARNING]
> **PacketRouter は現在 type バイト判定 + サイズ判定の2種しか対応していない**。
> 統一ヘッダ(20B)方式を導入する場合、PacketRouter の分岐ロジックを大幅に改修が必要。
> もしくは、DummyPeerが送る2バイト入力パケットを旧形式のまま維持し、
> 新パケット種別はヘッダ付きで送るデュアル対応が必要。

> [!IMPORTANT]
> **現行DummyPeerの `--test-mode game` は正しく動作している**。
> DummyPeer が 2バイト入力パケットを送信 → DLL の PacketRouter が受信 → 
> `OnRemoteInputPacket()` → `s_latestRemoteInput` (atomic) に格納 →
> `SceneInGame::Update()` の `ProcessRollbackFrame()` で参照。
> この経路は実際に動作するが、**DLL→DummyPeer方向の入力送信がない**ため
> 「片方向通信」状態である。

---

## 6. DummyPeerへの影響

現行DummyPeerとの通信で**実際に成立しているパケット交換**:

```
DummyPeer                     DLL    
                                      
  --- Negotiation 26B ------>         ★ CLI側（ゲーム起動前）
  <-- Negotiation 26B ------          ★ CLI側（ゲーム起動前）
                                      
  --- 2B Input ------------->         ★ PacketRouter→OnRemoteInputPacket
  (DLLからの送信なし)                  ❌ UdpSocket.Send()が入力用に使われていない  
                                      
  <-- SYNC_REQ (9B) --------          ★ TimeSynchronizer.Update()
  --- SYNC_RES (25B) ------->         ★ DummyPeer SyncResponder
  <-- SYNC_DONE (9B) -------          ★ TimeSynchronizer.Update()
  --- SYNC_DONE (9B) ------->         ★ DummyPeer SyncResponder
```

DummyPeer v2 設計への影響:
1. **DLL側の入力送信が未実装**のため、DummyPeer は相手からの入力パケットを受信できない
2. DummyPeer の E2E テストモードでは **DummyPeer同士のテスト** で検証する or **DLL側にも入力送信を実装する**必要がある
3. 統一ヘッダ(20B)を導入する場合、PacketRouter の分岐ロジック改修が前提条件となる

---

## 7. 仕様書・コード乖離の追加調査結果 (2026-02-27)

### 7.1 発見された乖離

| # | 乖離内容 | 仕様書 | 実コード | 状態 |
|---|---|---|---|---|
| 乖離1 | PacketRouterヘッダ判定 | 統一20B (CC10) | 生data[0]判定 | 旧互換モード維持 |
| 乖離2 | SYNC type値 | 0x31/0x32 | 0x10/0x11 | 旧互換モード維持 |
| 乖離3 | RedundantProtocolマジック | 'CC10' (0x30314343) | 'CCTR' (0x42525443) | [DEAD CODE]化 |
| 乖離4 | RedundantProtocol名前空間 | `cccaster::core::network` | `cccaster::network` | Phase4で修正 |
| 乖離5 | Negotiation管轄 | PacketType列挙に含む | CLI側専用, DLL経由なし | 仕様書に注記済み |

### 7.2 対応方針 (マスター決定済み)

> [!IMPORTANT]
> **PacketRouter 統一ヘッダ(20B)対応**: 作業C（SceneCharaSelect等の入力送受信TODO実装）と
> **同一スコープで実施**。作業Cが入力送信TODOに着手するタイミングで合流すること。
> それまでは旧互換モード(0x10/0x11/0x12 + 2B入力)を維持。

> [!NOTE]
> **RedundantProtocol**: 削除せず保留。`[DEAD CODE]` コメントを全ファイルに追記済み。
> Phase 4 (GAME_INPUT冗長化実装) 時に CC10/20B 準拠で全面再設計すること。
> 名前空間統一 (`cccaster::network` → `cccaster::core::network`) も同時に実施。

### 7.3 作業Aで実施した変更 (2026-02-27)

| ファイル | 変更内容 |
|---|---|
| `src/core/network/PacketRouter.cpp` | 拡張ポイントコメント追加（統一ヘッダ移行時の変更内容を明記） |
| `src/core/network/PacketRouter.hpp` | 仕様書リファレンス・現対応範囲・保留理由のコメント追加 |
| `src/core/network/RedundantProtocol.hpp` | `[DEAD CODE]` マーク、3問題点と再設計方針を明記 |
| `src/core/network/RedundantProtocol.cpp` | `[DEAD CODE]` マーク、関数内に未使用理由コメント追加 |
| 本ドキュメント | 乖離調査結果と対応方針を追記 |
| **`src/core/network/PacketRouter.cpp`** | **[by 作業C] 3Bシーン同期パケットルーティング追加: CS_INPUT(0x20), LOADING_INPUT(0x21), REMATCH_MENU(0x22) → 各SceneのSetRemote*()** |
