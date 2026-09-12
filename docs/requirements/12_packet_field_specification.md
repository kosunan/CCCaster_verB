# 12 Packet Field Specification (パケット項目仕様書)

> **最終更新**: 2026-02-27  
> **親文書**: `docs/requirements/06_packet_specification.md`  
> 本書は、各パケット内部のフィールドを**ビット単位・バイト単位**で詳細に定義します。

---

## 1. 入力ビットマスク定義 (`uint16_t remoteInput`)

ゲーム (MBAACC) の入力は 16bit のビットマスクで表現されます。
以下はゲーム内メモリから観測されるボタン割り当ての定義です。

| Bit | Mask | Name | 説明 |
|-----|------|------|------|
| 0 | `0x0001` | UP | 上方向 (8方向入力) |
| 1 | `0x0002` | DOWN | 下方向 |
| 2 | `0x0004` | LEFT | 左方向 |
| 3 | `0x0008` | RIGHT | 右方向 |
| 4 | `0x0010` | A | ボタンA (弱攻撃) |
| 5 | `0x0020` | B | ボタンB (中攻撃) |
| 6 | `0x0040` | C | ボタンC (強攻撃) |
| 7 | `0x0080` | D | ボタンD (シールド) |
| 8 | `0x0100` | E | ボタンE (特殊) |
| 9 | `0x0200` | FN1 | ファンクション1 (マクロ等) |
| 10 | `0x0400` | FN2 | ファンクション2 |
| 11 | `0x0800` | START | スタートボタン |
| 12-15 | `0xF000` | Reserved | 予約 (未使用) |

> [!NOTE]
> ビット配置はゲーム本体のメモリレイアウトに依存します。変更があった場合は `memory_and_hook_design.md` と同期して更新してください。

---

## 2. Negotiation Ping-Pong (26B) フィールド詳細

| Offset | Size | Type | Field Name | 詳細 |
|--------|------|------|------------|------|
| 0 | 8 | `int64_t` | `timestamp` | `std::chrono::steady_clock` ベースのマイクロ秒。RTT計算に使用。 |
| 8 | 1 | `uint8_t` | `hostFlag` | `0x00` = Client から送信 / `0x01` = Host から送信。受信側はこれで相手の役割を判別。 |
| 9 | 2 | `uint16_t` | `protocolVersion` | ツールのバージョン番号 (例: `1000` = v1.0.0)。不一致時は互換性警告を出力。 |
| 11 | 15 | `uint8_t[15]` | `reserved` | 将来の拡張用。現在はゼロ埋め。 |
| **計** | **26** | | | |

**送信コード**: `SessionNegotiator.cpp` L244-268  
**受信コード**: `SessionNegotiator.cpp` L156-209

---

## 3. SYNC_REQ (0x10 / 9B) フィールド詳細

| Offset | Size | Type | Field Name | 詳細 |
|--------|------|------|------------|------|
| 0 | 1 | `uint8_t` | `type` | 固定値 `0x10`。PacketRouter の分岐条件。 |
| 1 | 8 | `int64_t` | `T1` | 送信元の送信時刻 (μs)。NTP式計算の起点。 |

---

## 4. SYNC_RES (0x11 / 25B) フィールド詳細

| Offset | Size | Type | Field Name | 詳細 |
|--------|------|------|------------|------|
| 0 | 1 | `uint8_t` | `type` | 固定値 `0x11` |
| 1 | 8 | `int64_t` | `T1` | 元の SYNC_REQ から受け取った T1 のエコーバック |
| 9 | 8 | `int64_t` | `T2` | 受信側が SYNC_REQ を受け取った時刻 |
| 17 | 8 | `int64_t` | `T3` | 受信側が SYNC_RES を送り返す時刻 |

**RTT / θ の計算式** (受信側で T4 = 受信時刻 として):
```
RTT = (T4 - T1) - (T3 - T2)
θ   = ((T2 - T1) + (T3 - T4)) / 2
```

---

## 5. SYNC_DONE (0x12 / 9B) フィールド詳細

| Offset | Size | Type | Field Name | 詳細 |
|--------|------|------|------------|------|
| 0 | 1 | `uint8_t` | `type` | 固定値 `0x12` |
| 1 | 8 | `int64_t` | `startTime` | 確定したθから算出した「両者が同時にゲームフレームの進行を開始する時刻」(μs)。Big Bang Sync の基準。 |

---

## 6. 2バイト生入力パケット (現行) フィールド詳細

| Offset | Size | Type | Field Name | 詳細 |
|--------|------|------|------------|------|
| 0 | 2 | `uint16_t` | `remoteInput` | §1 で定義した入力ビットマスク。リトルエンディアン。 |

**判定条件**: `data.size() == 2` (PacketRouter.cpp L26-32)

> [!CAUTION]
> ヘッダレス。フレーム番号やタイムスタンプを持たないため、パケットの順序保証がなく、ロールバック判定には**不十分**。将来的にCCTR形式(§7)への移行が必須。

---

## 7. RedundantProtocol CCTR パケット (将来/51B) フィールド詳細

| Offset | Size | Type | Field Name | 詳細 |
|--------|------|------|------------|------|
| 0 | 4 | `uint32_t` | `MAGIC_NUMBER` | 固定値 `0x42525443` (ASCII: "CCTR")。パケット識別。 |
| 4 | 1 | `uint8_t` | `TYPE_INPUT` | 固定値 `0x01`。入力パケットを示す。 |
| 5 | 4 | `uint32_t` | `frameId` | **★必須**: 送信元の現在フレーム番号。ロールバック判定の基準値。 |
| 9 | 2 | `uint16_t` | `currentInput` | 現在フレームの入力ビットマスク (§1 参照)。 |
| 11 | 4 | `uint32_t` | `roundTimer` | ゲーム内のラウンドタイマー。デバッグ・Desync検知の照合用。 |
| 15 | 8 | `uint64_t` | `wasapiClock` | 送信側PCの高精度QPC値。Phase 2 ドリフト補正に使用。 |
| 23 | 8 | `uint64_t` | `creationTimeUs` | パケット作成時の絶対時刻 (μs)。現在はモック(0固定)。 |
| 31 | 20 | `uint16_t[10]` | `historyInputs` | **★パケロス復旧用**: 過去10フレーム分の入力。1パケ消失時に欠落フレームを復旧可能。 |
| **計** | **51** | | | |

**エンコード**: `RedundantProtocol::EncodeInput()` (RedundantProtocol.cpp L10-49)  
**デコード**: `RedundantProtocol::DecodeInput()` (RedundantProtocol.cpp L50-93)

> [!IMPORTANT]
> このパケットを有効化するには、PacketRouter に `data[0..3] == MAGIC_NUMBER` の分岐を追加し、各Scene(InGame, CharaSelect等)の送信処理に `EncodeInput()` の呼び出しを組み込む必要がある。

---

## 8. 未定義パケット (今後の設計が必要)

以下のパケットはTODOとして存在するが、フォーマットが未定です。

| パケット名 | 想定フェーズ | 最低限含むべきフィールド |
|---|---|---|
| **LOADING_INPUT** | Loading | `type`, `filteredInput`, `delaySyncFrames` |
| **REMATCH_MENU** | Rematch | `type`, `transitionIndex`, `retryMenuIndex` |
| **PC_SPEC_EXCHANGE** | CharaSelect (Phase 1.5) | `type`, `benchmarkScore`, `cpuFrequency` |

これらは実装時に本仕様書を更新して正式に定義すること。
