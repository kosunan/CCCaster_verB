# fix: GAME_INPUT 送受信を11F冗長化 (E-11)

## 変更概要
GAME_INPUT パケットを簡易版(6B) → 仕様書準拠の完全版(61B)に拡張。
11フレーム分の入力履歴 + roundTimer + wasapiClock を毎フレーム送信。

## 変更ファイル

### PacketRouter.cpp
- `GameInputHeader` → `GameInputEntry` + `GameInputPayload` 完全版に差し替え
  - DummyPeer の `UnifiedProtocol.hpp` と同一構造体定義
  - `history[11]` の全フィールドを受信・解析
- 受信ログに `hist=11` を追加（冗長化ペイロード受信の確認用）

### SceneInGame.cpp
- 11F入力履歴バッファ追加（static配列 `s_inputHistory[11]`）
  - DummyPeer の `PushInputHistory` と同一パターン（シフト挿入）
  - direction: 上位16bit, buttons: 下位16bit
- `Reset()` でバッファクリア追加
- 送信処理を 26B(20+6) → 81B(20+61) に拡張
  - `GameInputPayload` 全フィールドを構築・送信
  - latestFrame, inputDelay, roundTimer, wasapiClock, history[11]
