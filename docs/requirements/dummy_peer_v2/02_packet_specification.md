# DummyPeer v2 要件定義書 — 02. パケット仕様書

> **文書ID**: REQ-DP2-002  
> **作成日**: 2026-02-27  
> **ステータス**: レビュー待ち

---

## 1. パケットヘッダ（統一ヘッダ v2: 20バイト）

### 1.1 レイアウト

```
Offset  Size  Field         Description
------  ----  -----         -----------
0x00    4     magic         0x30314343 ('CC10') — マジックナンバー
0x04    1     phase         Phase enum — 通信フェーズ
0x05    1     type          PacketType enum — パケット種別
0x06    2     sequence      シーケンス番号 (uint16_t)
0x08    8     timestamp     送信時刻 (us, system_clock, uint64_t)
0x10    1     peerState     PeerState enum — 送信側の現在の業務状態
0x11    3     reserved      アライメント用予約 (将来拡張)
------
Total: 20 bytes
```

### 1.2 C++ 定義

```cpp
#pragma pack(push, 1)
struct UnifiedPacketHeader {
    uint32_t magic;         // PACKET_MAGIC ('CC10')
    uint8_t  phase;         // Phase enum
    uint8_t  type;          // PacketType enum
    uint16_t sequence;      // シーケンス番号
    uint64_t timestamp;     // 送信時刻 (us)
    uint8_t  peerState;     // [NEW] PeerState enum
    uint8_t  reserved[3];   // アライメント予約
};
static_assert(sizeof(UnifiedPacketHeader) == 20, "Header must be 20 bytes");
#pragma pack(pop)
```

### 1.3 後方互換

- 旧16バイトヘッダからの受信: `pkt.size() >= 16` で受理、`peerState = 0x00` として処理
- 新20バイトヘッダ → 旧プロトコル対応DLL: DLL側は追加4バイトを無視（`recvfrom` のサイズ不一致は許容）

---

## 2. Phase 列挙（互換維持）

| Phase | 値 | 説明 |
|---|---|---|
| `NEGOTIATION` | `0x00` | 接続確立 (SessionNegotiator互換26B) |
| `CHARA_SELECT` | `0x01` | キャラセレ (Phase 1/1.5/2) |
| `LOADING` | `0x02` | ロード中 |
| `PRE_GAME_SYNC` | `0x03` | 対戦前同期 (Phase 4) |
| `IN_GAME` | `0x04` | 対戦中 (Phase 6) |
| `REMATCH` | `0x05` | 再戦メニュー |

---

## 3. PacketType 列挙（全量）

### 3.1 既存（互換維持）

| PacketType | 値 | Phase | Payload | 説明 |
|---|---|---|---|---|
| `NEGO_PING` | `0x00` | NEGOTIATION | (26Bフルパケット) | SessionNegotiator Ping-Pong |
| `CS_SYNC_READY` | `0x08` | CHARA_SELECT | `CsSyncReadyPayload` | 200F到達通知 |
| `CS_INPUT` | `0x10` | CHARA_SELECT | `CsInputPayload` | キャラセレ入力 |
| `TIME_SYNC_REQ` | `0x31` | any | `TimeSyncReqPayload` | 時刻同期リクエスト |
| `TIME_SYNC_RES` | `0x32` | any | `TimeSyncResPayload` | 時刻同期レスポンス |
| `RNG_SYNC` | `0x30` | PRE_GAME_SYNC | `RngSyncPayload` | 乱数シード同期 |
| `READY` | `0x33` | PRE_GAME_SYNC | `ReadyPayload` | 準備完了 |
| `GAME_INPUT` | `0x40` | IN_GAME | `GameInputPayload` | 冗長化対戦入力(11F) |
| `HEARTBEAT` | `0xF0` | any | (なし) | 生存確認 |
| `DISCONNECT` | `0xFF` | any | (なし) | 切断通知 |

### 3.2 新規追加

| PacketType | 値 | Phase | Payload | 説明 |
|---|---|---|---|---|
| `LOADING_INPUT` | `0x20` | LOADING | `LoadingInputPayload` | Loading画面ディレイ入力 |
| `REMATCH_MENU` | `0x50` | REMATCH | `RematchMenuPayload` | Rematch メニュー選択 |
| `STATE_REPORT` | `0xE0` | any | `StateReportPayload` | 業務状態レポート（定期） |

---

## 4. ペイロード定義

### 4.1 既存ペイロード（変更なし）

```cpp
// CS_SYNC_READY: 200F到達通知
struct CsSyncReadyPayload {
    uint32_t frameCount;     // 到達フレーム数 (通常200)
};

// CS_INPUT: キャラセレ入力
struct CsInputPayload {
    uint32_t frame;          // フレーム番号
    uint16_t input;          // 入力値 (方向+ボタン)
};

// TIME_SYNC_REQ: NTP方式時刻同期リクエスト
struct TimeSyncReqPayload {
    int64_t t1;              // リクエスタ送信時刻
};

// TIME_SYNC_RES: NTP方式時刻同期レスポンス
struct TimeSyncResPayload {
    int64_t t1;              // エコーバック
    int64_t t2;              // レスポンダ受信時刻
    int64_t t3;              // レスポンダ送信時刻
};

// RNG_SYNC: 乱数シード同期
struct RngSyncPayload {
    uint32_t rngState0;
    uint32_t rngState1;
    uint32_t rngState2;
    uint8_t  rngArray[220];
};

// READY: 準備完了
struct ReadyPayload {
    uint8_t ready;           // 1 = ready
};

// GAME_INPUT: 冗長化対戦入力
struct GameInputEntry {
    uint16_t direction;      // 方向キー (numpad形式)
    uint16_t buttons;        // ボタンビットマスク
};
struct GameInputPayload {
    uint32_t   latestFrame;
    uint8_t    inputDelay;
    uint32_t   roundTimer;   // Desync検出用
    uint64_t   wasapiClock;  // WASAPI時刻
    GameInputEntry history[11]; // [0]=最新, [10]=10F前
};
```

### 4.2 新規ペイロード

```cpp
// LOADING_INPUT: Loading画面ディレイ入力
struct LoadingInputPayload {
    uint32_t frame;          // フレーム番号
    uint16_t input;          // 入力値
    int32_t  delay;          // 算出済みディレイ値
};

// REMATCH_MENU: Rematch メニュー選択
struct RematchMenuPayload {
    int8_t  menuIndex;       // 0=Rematch(再戦), 1=CharaSelect(キャラセレ戻り)
    uint8_t confirmed;       // 1=確定済み
};

// STATE_REPORT: 業務状態レポート
struct StateReportPayload {
    uint8_t  peerState;          // 現在の PeerState
    uint32_t framesInState;      // 現ステートの経過フレーム数
    uint16_t configDelay;        // 設定ディレイ値
    uint8_t  configMaxRollback;  // 設定ロールバック値
    int64_t  clockOffsetUs;      // 現在のクロックオフセット (μs)
    int64_t  rttUs;              // 最新RTT (μs)
};
```

---

## 5. パケット送受信シーケンス（PeerState別）

### 5.1 BOOTING → CS_SYNC_WAIT

```
DummyPeer                              本体DLL
    |                                     |
    |--- Negotiation (26B Ping-Pong) ---->|
    |<-- Negotiation (26B Ping-Pong) -----|
    |   (繰り返し、lock-in まで)           |
    |                                     |
    |  [BOOTING: 50F 高速スキップ相当]    |
    |--- STATE_REPORT(BOOTING) ---------> |
    |                                     |
    |  [CS_SYNC_WAIT: 同期待ち]          |
    |<-- SYNC_REQ ----------------------- |
    |--- SYNC_RES ----------------------->|
    |   (10往復)                          |
    |<-- SYNC_DONE -----------------------|
    |--- SYNC_DONE ---------------------->|
    |                                     |
    |  [CS_SYNC_DONE → CS_SELECTING]     |
```

### 5.2 CS_SELECTING

```
DummyPeer                              本体DLL
    |                                     |
    |--- CS_INPUT(frame,input) ---------> |
    |<-- CS_INPUT(frame,input) ---------- |
    |   (キャラ選択→ムーン選択→確定)      |
    |                                     |
    |  [CS_STAGE_SELECT]                  |
    |--- CS_INPUT(confirm) -------------> |
    |                                     |
    |  [LOADING に遷移]                   |
```

### 5.3 LOADING

```
DummyPeer                              本体DLL
    |                                     |
    |  [Phase 1: 30F 安定待ち]           |
    |--- STATE_REPORT(LOADING) ---------> |
    |                                     |
    |  [Phase 2: TimeSync]               |
    |<-- SYNC_REQ ----------------------- |
    |--- SYNC_RES ----------------------->|
    |<-- SYNC_DONE -----------------------|
    |--- SYNC_DONE ---------------------->|
    |                                     |
    |  [Phase 3: ディレイ入力交換]        |
    |--- LOADING_INPUT(frame,delay) ----> |
    |<-- LOADING_INPUT(frame,delay) ----- |
```

### 5.4 IN_GAME

```
DummyPeer                              本体DLL
    |                                     |
    |  [intro=2 同期]                     |
    |<-- SYNC_REQ ----------------------- |
    |--- SYNC_RES ----------------------->|
    |<-- SYNC_DONE -----------------------|
    |--- SYNC_DONE ---------------------->|
    |                                     |
    |  [対戦入力 60fps]                   |
    |--- GAME_INPUT(11F冗長化) ---------> |
    |<-- GAME_INPUT(11F冗長化) ---------- |
    |   (ラウンド終了まで)                |
```

### 5.5 REMATCH

```
DummyPeer                              本体DLL
    |                                     |
    |  [メニュー待ち]                     |
    |--- STATE_REPORT(REMATCH) ---------> |
    |                                     |
    |  [メニュー選択]                     |
    |--- REMATCH_MENU(idx, confirmed) --> |
    |<-- REMATCH_MENU(idx, confirmed) --- |
    |                                     |
    |  [max(local,remote) で遷移先決定]  |
    |  → CS_SELECTING or LOADING         |
```

---

## 6. 旧プロトコル互換性

### 6.1 SyncResponder 互換パケット（9バイト / 25バイト形式）

| タイプ | 値 | サイズ | フォーマット |
|---|---|---|---|
| `SYNC_REQ` | `0x10` | 9B | `[type(1)][T1(8)]` |
| `SYNC_RES` | `0x11` | 25B | `[type(1)][T1(8)][T2(8)][T3(8)]` |
| `SYNC_DONE` | `0x12` | 9B | `[type(1)][startTimeUs(8)]` |

これらは `--test-mode game` (旧互換モード) で引き続き使用される。`--test-mode e2e` では統一ヘッダ (20B) + ペイロード形式を使用する。

### 6.2 Negotiation 互換パケット（26バイト形式）

SessionNegotiator 互換の 26バイト Ping-Pong は一切変更しない。

```
Offset  Size  Field             Description
0x00    2     seq               シーケンス番号
0x02    8     timestamp         送信時刻 (us)
0x0A    8     echoedTime        エコーバック
0x12    8     processingDelay   処理遅延 (us)
Total: 26 bytes
```
