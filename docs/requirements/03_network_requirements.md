# 03 Network Requirements (ネットワーク通信要件)

## 1. 概要
CCCaster_v10 における UDP (P2P) 通信の方式、接続ネゴシエーション、およびパケットの冗長化仕様について定義します。

## 2. P2P 接続ネゴシエーション (`SessionNegotiator`)

### 2.1 PING-PONG ハンドシェイク
本格的なゲーム同期パケットの送受を開始する前に、まず両端間で 26バイト固定の Ping-Pong パケットを交換し、NATルーターへの穴あけ（UDP Hole Punching）と接続確認を実施します。

- **パケットサイズ**: 26バイト
- **ペイロード**: 現行時刻 (Timestamp, 8B) + クライアント情報やバージョンハッシュ等 (18B)
- **タイムアウト**: 返答がないまま 3.5秒 経過した場合（またはタイムアウト閾値を超えた場合）、接続失敗とみなす。

### 2.2 Hash Connect 方式の仕様
ホスト側のグローバルIPアドレス（IPv4/IPv6）とポート番号を、Base62エンコード等で短縮された文字列（ハッシュ）に圧縮し、相手に伝える方式です。

- ホスト起動時に外部API（ipify等）を利用して自身のグローバルIPを取得。
- 生成されたハッシュ文字列は自動でクリップボードへコピーされる。
- クライアントがハッシュを入力するとデコードされ、IPv4とIPv6の両方に対して並行して PING パケットを送信し、先に応答があった経路を採用する（Dual Stack Happy Eyeballs 的アプローチ）。

## 3. インゲーム通信パケット (UnifiedProtocol)

ゲーム本体へのフック完了後、対戦中の入力をやり取りするためのパケット仕様です。（※詳細は DummyPeer v2 の設計書も参照のこと）

### 3.1 冗長化の要件（Redundancy）
UDPのパケット到達非保証を補うため、送信するパケットには**「最新フレームの入力データ」だけでなく、「過去 N フレーム分の入力データ」を常に同梱**して送ります。
現在の仕様では N = 10 フレームと定義しています。

### 3.2 パケット構造
すべてのインゲーム通信は `cccaster::main_app::unified::Header` (20B) を先頭に持ちます。

```cpp
struct Header {
    uint8_t magic[2];      // "CC"
    uint16_t version;      // ex: 1000 for v1.0.0
    Phase phase;           // uint8_t: CharaSelect, InGame, Loading 等
    PacketType type;       // uint8_t: SyncReq, Input, Chat 等
    uint32_t sequenceId;   // パケットの連番
    uint64_t timestamp;    // 送信元PCの送信時刻(us)
    uint16_t payloadSize;  // 後続するペイロードのバイト数
};
```

## 4. 通信ステータス監視要件
通信中は、以下のメトリクスを常に算出し、UI（CLI または ImGuiオーバーレイ）に表示できるようにメモリ上に保持します。

1. **Ping (Round Trip Time)**: パケット往復にかかる時間（ミリ秒）
2. **Jitter**: Ping値の揺らぎ（標準偏差や直近変動幅）
3. **Packet Loss**: シーケンス番号の欠落から計算されるロス率（％）
4. **Tx / Rx Rate**: 秒間に送受信されているパケット数

※ 目安: 60FPSのゲームであるため、遅延がなければTx/Rxはそれぞれ約 60〜62 packets/sec となる。
