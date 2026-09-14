
# refactor(docs): 大規模アーキテクチャ設計書のマスター統合と過去資料のアーカイブ

## 2026-03-01: システム全体の可読性を高めるためのマスタードキュメント化
- [REFACTOR] `docs/design/` 直下のメモリやIPCに関する基本設計を統合し、`System_Architecture_Overview.md`（システム根幹アーキテクチャ）を作成。不要になった元ファイルを削除。
- [REFACTOR] `docs/design/core_dll/` 配下のフック・シーン・入力関連設計を統合し、`Core_Engine_Architecture.md`（DLL層アーキテクチャ）を作成。不要になった元ファイルを削除。
- [REFACTOR] `docs/design/main_app/` 配下の内部機能および設定、`launcher`の設計を統合し、`Application_Layer_Architecture.md`（アプリ層アーキテクチャ）を作成。不要になった元ファイルを削除。
- [CHORE] 仕様が確定し過去の遺物となった細かい検討メモ群（`architecture_cross_review_anxieties.md`, `asm_hacks_inventory.md`等）を `docs/archive/design/` へ完全退避。
- [MODIFY] `AI_WORKSPACE_GUIDE.md` のファイルインデックスおよび「新規AI向けのフロー」記述を更新し、上記3つのマスタードキュメントを起点とするよう誘導パスを修正。

