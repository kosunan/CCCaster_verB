# CCCaster_verB 作業領域

現在の作業先は `I:/work_space/CCCaster_verB`。ルートの `build.bat` で32bit ReleaseビルドとCTestを実行する。成果物は `build/bin/`。
CLIは `CCCaster_B.exe`、GUIは `CCCaster_B_GUI.exe`。バージョンはルートの `VERSION` で管理する。[更新手順](docs/VERSIONING.md)。実ゲーム環境の配置と専用実ゲームスクリプトの移行は継続する。

## 構成

| フォルダー | 用途 | Git管理 |
|---|---|---|
| `src/` | C++ソース、CMake、CI、ビルド定義 | 管理する |
| `docs/` | 現行仕様、運用手順、設計、変更履歴 | 管理する |
| `test/` | テストコード、実行スクリプト、隔離したゲーム実体、テストログ | コード・手順のみ管理する |
| `release/` | 配布マニフェスト、配布物のステージング、ハッシュ | マニフェスト・手順のみ管理する |
| `archive/` | 旧環境から保全する読み取り専用スナップショットと移行記録 | 原則管理しない |
