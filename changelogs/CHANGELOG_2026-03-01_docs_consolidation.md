
# refactor(docs): ネットワーク・アプリケーション設計書の結合とアーカイブ化

## 2026-03-01: 散在する設計ドキュメントの包括的ファイルへの統合化
- [CHORE] 不要になった「実装計画書」や古い作業メモ類（`_plan.md` 等）を `docs/archive/design/` へ一括退避。
- [REFACTOR] `docs/design/main_app/` に散在していた外部UI・内部構造・画面遷移の設計書（4ファイル）を `CLI_Architecture_and_UI.md` へ一新・統合。
- [REFACTOR] `docs/design/network/l7_application/` に散在していた各種対戦フローと同期フェーズ仕様（4ファイル）を `Netplay_Session_Lifecycle.md` へ統合。
- [REFACTOR] 時間同期・片道遅延・クロックドリフト補正のアルゴリズム資料を `TimeSync_Algorithm.md` に統合。
- [REFACTOR] `core_dll` における60FPSタイマーとFastForwardの制御資料を `Frame_Pacing_Control.md` に統合。
- [MODIFY] 各ファイルの消滅・結合に伴い、`AI_WORKSPACE_GUIDE.md` の設計書インデックスを新しい包括ファイルへとリンクを張り直し更新。

