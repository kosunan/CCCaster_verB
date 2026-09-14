
# refactor(docs): ネットワーク通信関連設計書のレイヤー別整理

## 2026-03-01: ネットワーク設計書の適切な区分け（L4/L5/L7）
- [ADD] `docs/design/` 配下に `network/` ディレクトリを新設し、OSI参照モデル等に基づき `l4_transport` / `l5_session` / `l7_application` のレイヤーごとの整理フォルダを作成。
- [MOVE] トランスポート層（UDP・冗長化等）に該当する `network_protocol_design.md` を `network/l4_transport/` へ移動。
- [MOVE] セッション確立や接続管理に該当する `connection_hash_design.md`, `main_app_SessionNegotiator.md` を `network/l5_session/` へ移動。
- [MOVE] 同期アルゴリズムやアプリケーションフローに該当する設計書類（`owd_sync_algorithm_plan.md`, `09_netplay_workflow.md` 等）を `network/l7_application/` へ移動。
- [MODIFY] `AI_WORKSPACE_GUIDE.md` の参照パスおよびインデックスツリーを、上記の新ディレクトリ構造に従って更新。

