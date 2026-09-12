# refactor(docs): 要件定義と設計層の分離・ディレクトリ構造の適正化

## 2026-03-01: ドキュメント配置の整理と `docs/design` の構造化
- [MOVE] `docs/requirements/` に混入していた設計書・画面フロー図（`07_`〜`11_`）を本来の置き場所である `docs/design/main_app/` へ移動。
- [MOVE] `docs/design/` 直下に散見されたCLIやネットプレイ関連の設計資料群を `docs/design/main_app/` へ整理。
- [MOVE] `docs/design/` 直下のコアDLL・インゲーム関連の設計資料群を `docs/design/core_dll/` へ整理。
- [MOVE] 古い仕様（レガシーエミュレーター関連等）の `legacy_*.md` を `docs/archive/design/` へ退避。
- [MODIFY] 上記ファイル移動に伴い、`AI_WORKSPACE_GUIDE.md` に記載されているディレクトリツリー要素と参照パス一覧を最新状態に更新。
