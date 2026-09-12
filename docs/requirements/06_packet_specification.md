# 06 Packet Specification (パケット仕様書)

> **最終更新**: 2026-02-27  
> **根拠資料**:  
> - `packet_analysis.md.resolved` (コードベース全体解析)  
> - `docs/requirements/dummy_peer_v2/04_packet_analysis_report.md` (DummyPeer E2E検証)

---

## 1. パケット体系の全体像

CCCaster_v10 のパケット通信は「CLI層（ゲーム起動前）」と「DLL層（ゲーム中）」で完全に分離しています。CLI側の `UdpSocket` はゲーム起動時に破棄され、DLL側は同じポートで新しい `UdpSocket` を再バインドします。

```
┌─────────────────────────┐    ┌─────────────────────────┐
│  CLI層 (main_app)        │    │  DLL層 (core_dll)        │
│  ・Negotiation Ping-Pong │    │  ・SYNC_REQ/RES/DONE     │
│    (26B, SessionNeg.)    │    │  ・入力パケット (2B〜51B) │
│                          │    │  ・メニュー同期 (未実装)  │
│  UdpSocket A (破棄)      │    │  UdpSocket B (再バインド) │
└─────────────────────────┘    └─────────────────────────┘
```

---

## 2. CLI層: Negotiation Ping-Pong パケット (26 Bytes)

ゲーム起動前に `SessionNegotiator` が相互に送り合う接続確認パケット。

| Offset | Size | Field | 説明 |
|--------|------|-------|------|
| 0 | 8 | `timestamp` (int64_t) | 送信元PCの高精度時刻 (μs) |
| 8 | 1 | `hostFlag` (uint8_t) | 0=Client, 1=Host |
| 9 | 17 | `versionHash` etc. | バージョンやクライアント情報 |
| **計** | **26** | | |

- **送信箇所**: `SessionNegotiator.cpp` L244-268
- **受信箇所**: `SessionNegotiator.cpp` L156-209
- **タイムアウト**: 3.5秒間返答なしで接続失敗

---

## 3. DLL層: TimeSync パケット群

DLL内で **唯一稼働中の送信コード** を持つパケット群。
`TimeSynchronizer.cpp` が送受信を担当し、NTPライクなクロックオフセット(θ)を推定します。

### 3.1 SYNC_REQ (9 Bytes)

| Offset | Size | Field | 説明 |
|--------|------|-------|------|
| 0 | 1 | `type` = `0x10` | パケット種別識別子 |
| 1 | 8 | `T1` (int64_t) | 送信側の送信時刻 (μs) |
| **計** | **9** | | |

- **送信間隔**: 50ms × 最大10回
- **送信箇所**: `TimeSynchronizer.cpp` L288-299

### 3.2 SYNC_RES (25 Bytes)

| Offset | Size | Field | 説明 |
|--------|------|-------|------|
| 0 | 1 | `type` = `0x11` | パケット種別識別子 |
| 1 | 8 | `T1` (int64_t) | 元の SYNC_REQ の T1 をエコーバック |
| 9 | 8 | `T2` (int64_t) | SYNC_REQ を受信した時刻 |
| 17 | 8 | `T3` (int64_t) | SYNC_RES を送信する時刻 |
| **計** | **25** | | |

- **送信条件**: SYNC_REQ 受信時に即応答
- **受信処理**: RTT = (T4 - T1) - (T3 - T2), θ = ((T2 - T1) + (T3 - T4)) / 2
- **送信箇所**: `TimeSynchronizer.cpp` L390-408

### 3.3 SYNC_DONE (9 Bytes)

| Offset | Size | Field | 説明 |
|--------|------|-------|------|
| 0 | 1 | `type` = `0x12` | パケット種別識別子 |
| 1 | 8 | `startTime` (int64_t) | 確定したθから算出した同期開始時刻 (μs) |
| **計** | **9** | | |

- **送信条件**: θ確定後＋150ms待機後
- **受信処理**: `_peerDoneReceived = true` → 双方完了で同期成立
- **送信箇所**: `TimeSynchronizer.cpp` L323-344

---

## 4. DLL層: 入力パケット (現行 / 将来)

### 4.1 現行: 2バイト生入力パケット (受信のみ稼働)

| Offset | Size | Field | 説明 |
|--------|------|-------|------|
| 0 | 2 | `remoteInput` (uint16_t) | ボタンビットマスク (方向+ボタン合成値) |
| **計** | **2** | | |

- **受信経路**: `PacketRouter` が `data.size() == 2` で判定 → `OnRemoteInputPacket()` → `s_latestRemoteInput` (atomic)
- **送信**: **⚠️ DLL側に送信コードが存在しない**。DummyPeerのみが送信可能。

> [!CAUTION]
> 現状は **片方向通信** 状態。本番ネット対戦では DLL→DLL の双方向入力送信を実装する必要あり。

### 4.2 将来: RedundantProtocol (CCTR) 冗長入力パケット (51 Bytes)

定義済みだが **どの Scene からも呼び出されていない** 孤立コード。
ロールバック対戦に必要な情報がすべて含まれており、将来の本番パケットとなる想定。

| Offset | Size | Field | 型 | 説明 |
|--------|------|-------|----|------|
| 0 | 4 | `MAGIC_NUMBER` | fixed | `0x42525443` ("CCTR") |
| 4 | 1 | `TYPE_INPUT` | uint8_t | `0x01` |
| 5 | 4 | `frameId` | uint32_t | フレーム番号（ロールバック位置特定） |
| 9 | 2 | `currentInput` | uint16_t | 現在フレームの入力ビットマスク |
| 11 | 4 | `roundTimer` | uint32_t | ゲーム内ラウンドタイマー (確認用) |
| 15 | 8 | `wasapiClock` | uint64_t | 高精度QPC時刻 (ドリフト補正用) |
| 23 | 8 | `creationTimeUs` | uint64_t | パケット作成時の絶対時刻 (現在0固定モック) |
| 31 | 20 | `historyInputs[10]` | uint16_t×10 | 過去10F分の入力(パケロス復旧用冗長化) |
| **計** | **51** | | | |

- **エンコード**: `RedundantProtocol::EncodeInput()` (L10-49)
- **デコード**: `RedundantProtocol::DecodeInput()` (L50-93)
- **接続状況**: PacketRouter にデコード分岐なし。完全孤立。

---

## 5. 未実装パケット一覧 (TODO)

| # | パケット名 | フェーズ | コード上の所在 | 必須度 |
|---|---|---|---|---|
| ❌1 | **入力パケット送信** (DLL→相手) | 全画面 | 送信コード自体が不在 | ★★★ 必須 |
| ❌2 | **CharaSelect入力受信** | CharaSelect | `SceneCharaSelect.cpp` L200 | ★★★ 必須 |
| ❌3 | **Loading入力送信** | Loading | `SceneLoading.cpp` L125 | ★★☆ 必須 |
| ❌4 | **Loading入力受信** | Loading | `SceneLoading.cpp` L126 | ★★☆ 必須 |
| ❌5 | **Rematchメニュー送信** | Rematch | `SceneRematch.cpp` L279 | ★★☆ 必須 |
| ❌6 | **Rematchメニュー受信** | Rematch | `SceneRematch.cpp` L284 | ★★☆ 必須 |
| ❌7 | **Phase 1.5 PCスペック交換** | CharaSelect | `SceneCharaSelect.cpp` L132 | ★☆☆ 中 |

---

## 6. PacketRouter の分岐ルール (現行)

```cpp
// PacketRouter.cpp L14-38
if (data[0] >= 0x10 && data[0] <= 0x12) {
    // → TimeSynchronizer::OnReceiveSyncPacket
} else if (data.size() == 2) {
    // → OnRemoteInputPacket (2B生入力)
} else {
    // → "[PacketRouter] UNKNOWN" ログ出力
}
```

> [!WARNING]
> RedundantProtocol (CCTR/51B) を有効化する場合、data[0]==0x01 かつ先頭4B がマジックナンバーの分岐を追加する必要あり。
> 統一ヘッダ (20B) 方式へ移行する場合は、PacketRouter の分岐ロジック全体の改修が前提となる。
