# docs(network): PacketRouterとRedundantProtocolの仕様乖離を明記し、対応方針を確定

## 変更内容

### src/core/network/PacketRouter.cpp
- **拡張ポイントコメント追加**: 現在の旧互換モード (SYNC 0x10/0x11/0x12 + 2B入力) の説明
- 統一ヘッダ (20B, CC10) 対応時の変更方針を明記
  - 対応 PacketType 一覧 (CS_INPUT, LOADING_INPUT, GAME_INPUT 等)
  - 作業C（Domain層入力送受信実装）との合流タイミングで同時実施すること

### src/core/network/PacketRouter.hpp
- 仕様書リファレンス (`02_packet_specification.md`) を追記
- 現在の対応範囲と統一ヘッダ対応保留の理由をヘッダコメントに明記

### src/core/network/RedundantProtocol.hpp
- `[DEAD CODE]` ステータスマークを追加
- 未接続である3つの問題点を明記:
  1. PacketRouter から参照されていない（受信経路なし）
  2. マジック `CCTR` が仕様書の `CC10` と不一致
  3. 名前空間 `cccaster::network` が `cccaster::core::network` と不一致
- Phase 4 (GAME_INPUT 冗長化実装) 時の再設計方針を TODO コメントで明記

### src/core/network/RedundantProtocol.cpp
- `[DEAD CODE]` ステータスマークを追加
- 各関数内に「PacketRouter 未接続のため実行されない」旨のコメント追加

### docs/requirements/dummy_peer_v2/04_packet_analysis_report.md
- ステータスを「解析完了 → 対応方針決定済み」に更新
- §7 として「仕様書・コード乖離の追加調査結果 (2026-02-27)」を追記:
  - 発見した乖離5件の一覧表
  - マスター決定済み対応方針
  - 作業Aで実施した変更一覧

## 変更しなかったもの（理由）
- PacketRouter のルーティングロジック: 旧互換モードは正常動作中
- RedundantProtocol の実装コード: Phase 4 再設計まで温存
- Domain 層ファイル (SceneCharaSelect 等): 作業A の対象外
