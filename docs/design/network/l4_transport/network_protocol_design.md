# 通信プロトコル詳細設計

## 1. 統一パケットヘッダ (Unified Packet Header)
全フェーズ共通の16バイトヘッダ。

```cpp
#pragma pack(push, 1)
struct UnifiedPacketHeader {
    uint32_t magic;       // 0x30314343 ('CC10')
    uint8_t  phase;       // Phase enum (後述)
    uint8_t  type;        // PacketType enum (後述)
    uint16_t sequence;    // シーケンス番号
    uint64_t timestamp;   // 送信時刻 (us, system_clock)
};
#pragma pack(pop)
// sizeof = 16 bytes
```

### 1.1 フェーズ定義
```cpp
enum Phase : uint8_t {
    NEGOTIATION   = 0x00, // 接続確立 (SessionNegotiator互換)
    CHARA_SELECT  = 0x01, // キャラセレ (Phase 1/1.5/2)
    LOADING       = 0x02, // ロード中
    PRE_GAME_SYNC = 0x03, // 対戦前同期 (Phase 4)
    IN_GAME       = 0x04, // 対戦中 (Phase 6)
    REMATCH       = 0x05, // 再戦メニュー
};
```

### 1.2 パケット種別
```cpp
enum PacketType : uint8_t {
    // Negotiation (Phase 0) — SessionNegotiator互換26バイト
    NEGO_PING       = 0x00,

    // CharaSelect Sync (Phase 1.5)
    CS_SYNC_READY   = 0x08, // 200F到達通知 (キャラセレ同期ポイント)

    // CharaSelect Input (Phase 2)
    CS_INPUT        = 0x10, // キャラセレ入力 (16bit input)

    // Pre-Game Sync (Phase 4)
    RNG_SYNC        = 0x30, // 乱数シード共有 (Host→Client)
    TIME_SYNC_REQ   = 0x31, // 時刻同期リクエスト (NTP方式)
    TIME_SYNC_RES   = 0x32, // 時刻同期レスポンス
    READY           = 0x33, // 準備完了通知

    // InGame (Phase 6)
    GAME_INPUT      = 0x40, // 冗長化入力パケット (11F分)

    // Common
    HEARTBEAT       = 0xF0,
    DISCONNECT      = 0xFF,
};
```

## 2. フェーズ別ペイロード

### 2.1 CS_SYNC_READY (Phase 1.5)
200F到達通知。相手と同時に動き出すための同期ポイント。
```cpp
struct CsSyncReadyPayload { uint32_t frameCount; };
```

### 2.2 CS_INPUT (Phase 2)
キャラセレ入力パケット。ランダムボタン連打によるズレ耐性テスト用。
```cpp
struct CsInputPayload { uint32_t frame; uint16_t input; };
```

### 2.3 TIME_SYNC_REQ/RES (Phase 1.5/4)
NTP方式の時刻同期。Client→Host→Client の往復で θ (オフセット) を推定。
```cpp
struct TimeSyncReqPayload { int64_t t1; };
struct TimeSyncResPayload { int64_t t1; int64_t t2; int64_t t3; };
```

### 2.4 RNG_SYNC (Phase 4)
乱数シード同期。Hostが読み取り、Clientに送信・適用。
```cpp
struct RngSyncPayload {
    uint32_t rngState0, rngState1, rngState2;
    uint8_t rngArray[220];
};
```

### 2.5 GAME_INPUT (Phase 6)
冗長化対戦入力。パケットロス耐性のため過去10F分を同梱。
```cpp
struct GameInputPayload {
    uint32_t latestFrame;
    uint8_t  inputDelay;
    uint32_t roundTimer;     // Desync検出用
    uint64_t wasapiClock;
    GameInputEntry history[11]; // [0]=最新, [10]=10F前
};
```

> [!TIP]
> **パケットロス耐性**: 10連続でパケットが消失しない限り、後続パケットで過去入力が補完される。

## 3. 通信シーケンス

### 3.1 セッション確立 (Phase 0: Negotiation)
SessionNegotiator互換の26バイトPing-Pong。接続確立後にPhase 1へ遷移。

### 3.2 キャラセレ同期 (Phase 1.5)
1. 200F到達 → `CS_SYNC_READY` を相互送信
2. 双方到達を確認 → `TIME_SYNC_REQ/RES` × 10往復で θ 推定
3. 原子的ポーズ解除で同時にフレーム進行再開

### 3.3 対戦前同期 (Phase 4)
1. Host が `RNG_SYNC` を送信
2. 双方が `TIME_SYNC` で θ を再測定 (ロード時間のドリフト補正)
3. `READY` を相互送信して対戦開始

### 3.4 対戦中 (Phase 6)
- 各パケット受信時に `ack_number` と `ack_bitmask` を更新 (GAME_INPUT内)
- 相手の最新フレーム番号を確認し、必要であればロールバック処理をトリガー
