# fix: GAME_INPUT(0x40) 送受信実装 + OnRemoteInputPacketにフレーム番号追加 (Fix-2)

## 変更概要
OnRemoteInputPacket にフレーム番号を追加し、s_latestRemoteFrame への書込みを修正。
同時に GAME_INPUT パケットの送受信を実装。

## 変更ファイル

### PacketRouter.cpp
- `TYPE_GAME_INPUT = 0x40` 定数追加
- `GameInputHeader` 構造体追加（ペイロード先頭の必要フィールドのみ抽出）
- `switch(type)` に `case TYPE_GAME_INPUT` 追加（latestFrame + history[0].buttons を読取り）
- 前方宣言を `OnRemoteInputPacket(uint32_t, uint16_t)` に更新
- 旧2Bパケットはフレーム0で後方互換維持

### SceneRunner.cpp
- `OnRemoteInputPacket(uint16_t)` → `OnRemoteInputPacket(uint32_t frame, uint16_t remoteInput)` に変更
- `s_latestRemoteFrame.store(frame)` を追加
- `SceneInGame::Update` 呼出しに `s_send` を追加

### SceneInGame.hpp
- `Update` シグネチャに `const session::SceneRunner::SendFunc& send` を追加
- `SceneRunner.hpp` の include 追加

### SceneInGame.cpp
- `Update` / `ProcessRollbackFrame` のシグネチャに `send` 引数追加
- ローカル入力フィルタ後・RE.UpdateFrame前に GAME_INPUT パケット送信処理を挿入
  - 簡易パケット: 20Bヘッダ + 6Bペイロード (frame + input)
  - RE.UpdateFrame前に送信することで ~1ms のレイテンシ削減
- ファイル先頭コメントから「4. SleepFrame()」を削除（E-8集約済み反映）
