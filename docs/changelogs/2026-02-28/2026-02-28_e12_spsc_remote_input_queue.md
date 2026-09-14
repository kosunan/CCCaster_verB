# fix: SPSC リングバッファ導入 — リモート入力上書き問題解消 (E-12)

## 変更概要
リモート入力の受渡しを `atomic<uint16_t>` 単一値 → SPSC ロックフリーリングバッファに変更。
パケットロス時にも11F冗長化の恩恵を最大化し、全リモート入力を RE に漏れなく渡す。

## 新規ファイル

### RemoteInputQueue.hpp
- SPSC (Single-Producer/Single-Consumer) ロックフリーリングバッファ
- 256エントリ固定、alignas(64) で false sharing 防止
- Producer: UDPスレッド (OnRemoteInputPacket → Push)
- Consumer: メインスレッド (ProcessRollbackFrame → Pop)

## 変更ファイル

### PacketRouter.cpp
- `OnRemoteInputPacket` の前方宣言を `(uint32_t, const void*, int)` に変更
- GAME_INPUT 受信で `gi.history, 11` を配列ごと渡す
- 旧2Bレガシーパケットも単一エントリ配列経由に変更

### SceneRunner.cpp / SceneRunner.hpp
- `s_latestRemoteInput` (atomic) を削除
- `RemoteInputQueue s_remoteInputQueue` を追加
- `OnRemoteInputPacket` を history 全展開→push に書換え
- `GetRemoteInputQueue()` アクセサを追加

### SceneInGame.hpp / SceneInGame.cpp
- `Update` / `ProcessRollbackFrame` から atomic 引数2本を削除
- リモート入力受渡しを queue drain に差替え
