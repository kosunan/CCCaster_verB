# 接続ハッシュ設計書 (Connection Hash Design)

## 目的
ホストがIPv4/IPv6の**両方**のグローバルIPアドレスを自動取得し、Base32エンコードしたハッシュ文字列として公開する。
クライアントはハッシュを入力するだけで、IPv4/IPv6の選択を意識せずに接続できる。

## ハッシュフォーマット

### 出力例
```
K7MXAIQD2KBLGR4DKNL2
```

### 構造
```
[公開鍵4文字][Base32(ペイロード)]
```

### ペイロード（バイナリ構造）
```
Offset  Size   Field
0x00    1      flags       (bit0=has_ipv4, bit1=has_ipv6)
0x01    4      ipv4_addr   (ネットワークバイトオーダー、flagsのbit0が立つ場合のみ)
0x05    16     ipv6_addr   (128bit、flagsのbit1が立つ場合のみ)
0x15    2      port        (ネットワークバイトオーダー)
```
- IPv4のみの場合: 1 + 4 + 2 = 7バイト → Base32で12文字
- IPv4+IPv6の場合: 1 + 4 + 16 + 2 = 23バイト → Base32で37文字

### 公開鍵プレフィックス
- Windowsのコンピュータ名をFNV-1aでハッシュし、Base32で4文字にエンコード
- 暗号学的な認証目的ではなく、ホストの視覚的識別用

### Base32エンコーディング
- RFC 4648準拠（A-Z, 2-7）
- パディング (`=`) は省略

## 接続フロー

### ホスト側
```
1. ユーザーがポート番号を入力
2. api.ipify.org (IPv4) と api6.ipify.org (IPv6) を同時取得
3. ConnectionHash::Encode(ipv4, ipv6, port) でハッシュ生成（ハイフンなし連結）
4. "[公開鍵][ハッシュ]" をクリップボードにコピー＆画面表示（ダブルクリックで全選択可能）
5. UDPソケットでリッスン開始
```

### クライアント側
```
1. ユーザーがハッシュ文字列を入力（ペースト可能）
2. ConnectionHash::Decode() でIPv4/IPv6/ポートを復元（先頭4文字=公開鍵、残り=ペイロード）
3. IPv4を優先して接続試行、失敗時にIPv6でリトライ
```

## UIフロー変更
```
旧: MainMenu → NetplayMenu(IPv4/IPv6選択) → HostOrClient → Connection
新: MainMenu → HostOrClient → Connection (ハッシュ自動判定)
```

## ヘッドレスモード
```
# ホスト
CCCaster_v10.exe --headless --host --port 10800

# クライアント（ハッシュ指定）
CCCaster_v10.exe --headless --hash K7MXAIQD2KBLGR4DKNL2
```

## 依存関係
- `WinINet` (ipify API呼び出し、既存)
- `ws2_32` (inet_pton/inet_ntop、既存)
- 新規外部ライブラリ追加なし

## セキュリティ (Phase 2)

### XOR暗号化
- **鍵生成**: `FNV-1a_64(floor(UTC秒 / 21600))` + ソルト `"CCCaster_v10_Session_2026"` → バイトストリーム展開
- ペイロード全体を鍵ストリームでXOR暗号化（全く同じ時間帯にいる両者のみ復号可能）
- デコード時は現在＋前ウィンドウの2つの鍵を試行（境界対策）

### ワンタイムセッショントークン
- `std::random_device` で4バイト乱数を生成
- 毎回異なるハッシュ出力を保証（同じIP+ポートでも異なるハッシュ）

### 有効期限
- ペイロードにUNIXタイムスタンプ（下位32bit）を埋め込み
- デコード時に生成時刻から6時間以内かを検証
- 期限切れ時は専用エラーメッセージを表示

### ペイロード構造 (暗号化版)
```
[flags 1B] [IPv4 4B] [IPv6 16B] [Port 2B] [Token 4B] [Timestamp 4B]
↓ XOR暗号化 → Base32エンコード → [公開鍵][Base32]  (ハイフンなし)
```
