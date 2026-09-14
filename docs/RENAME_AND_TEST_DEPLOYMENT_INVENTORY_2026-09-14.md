# CCCaster_vB1 移行・テスト差し替え棚卸し

作成日: 2026-09-14

## 確定事項

- 開発フォルダーを `I:/work_space/CCCaster_v10` から `I:/work_space/CCCaster_vB1` へ移す。
- 旧フォルダーは削除せず、Git履歴と未コミット変更を含むアーカイブとして保全する。
- 製品名、実行ファイル、ビルドターゲット、文書、スクリプト、CI、Git remote を `CCCaster_vB1` に統一する。
- 通常テスト環境は、当該コピーから起動したプロセスを終了後、最新ビルドから強制同期する。

## 識別子の移行表

| 区分 | 現在 | 移行先 |
|---|---|---|
| 開発フォルダー | `CCCaster_v10` | `CCCaster_vB1` |
| CLI | `CCCaster_v10.exe` | `CCCaster_vB1.exe` |
| GUI | `CCCaster_v10_GUI.exe` | `CCCaster_vB1_GUI.exe` |
| GUI診断版 | `CCCaster_v10_GUI_startup.exe` | `CCCaster_vB1_GUI_startup.exe` |
| 設定 | `cccaster_v10.ini` | `cccaster_vB1.ini` |
| CMakeターゲット | `CCCaster_v10`, `CCCaster_v10_GUI` | `CCCaster_vB1`, `CCCaster_vB1_GUI` |
| 配置フォルダー | `cccaster_B` | 維持（すでに配布名として使用） |
| フックDLL | `libcccaster_hook.dll` | 維持（注入・配布互換性に関わるため別判断） |
| Git remote | `kosunan/CCCaster_v10_1` | 新リポジトリ作成後に変更 |

公開名 `CCCaster_verB` は既にGitHubリポジトリ／リリースで使われている。`CCCaster_vB1` へ揃えるか、公開名を互換名として残すかはGitHub変更時に判断する。

## 通常テストと最新ビルドの定義

通常の実ゲーム試験は次の2コピーだけである。

| 役割 | パス | 同期対象 |
|---|---|---|
| P1 | `_TEST_MBAACC/MBAACC_1/cccaster_B` | CLI、GUI、フックDLL、採用済み補助EXE |
| P2 | `_TEST_MBAACC/MBAACC_2/cccaster_B` | 同上 |

コピー元の正本は、成功した32bitビルドの `build/bin/` とする。更新日時ではなく、BAT内の配布マニフェストとハッシュで最新版を決める。現時点の `build/bin/` には通常CLI・通常GUI・DLLに加え、過去の `*_startup.exe`、`*_updated.exe` が残るため、マニフェスト外の成果物を自動採用しない。

## 差し替えBATの契約

1. コピー元を `build/bin`、コピー先を上記の2 `cccaster_B` に固定する。
2. マニフェスト上の全成果物と32bit PEを確認できない場合、削除もコピーもせず失敗する。
3. コピー先パスから起動された `MBAA.exe`、CLI、GUI、起動補助EXEだけをCIMで特定し、強制終了する。名前だけで全PCの同名プロセスを終了しない。
4. 各 `cccaster_B` をコピー元マニフェストと完全一致させる。旧EXE、DLL、INI、ログを含むマニフェスト外のファイルは削除する。
5. 削除予定一覧、終了PID、コピー後SHA-256を日時付きログへ保存する。
6. `MBAA.exe`、`Information/`、`System/`、テストルート外のファイルには一切触れない。

「コピー元にないものを全削除」は、ゲーム直下ではなく `cccaster_B` に限定する。ゲーム直下まで同期するとゲーム本体・公式INIを失うため採用しない。コントローラー設定も削除対象となるので、残す方針に変える場合だけ明示的な除外規則を追加する。

## 通常同期から除外する独立環境

- `_TEST_CLOSE_MBAACC`: 仮想コントローラー／UI検証用。
- `_TEST_MBAACC/CommunityRegression`、`CommunityMixedRegression`: community EXE比較用。
- `_TEST_MBAACC/LegacyBenchmark`: 旧CCCaster比較用。
- `_TEST_MBAACC/SpectatorRegression`: 観戦3窓試験用。
- `_TEST_MBAACC/Steam_Analysis`、`SteamPortRegression`: Steam解析用。Steam本体は別リポジトリ `I:/work_space/CCCaster_Steam` で管理する。

## 改名対象の分類

| 優先度 | 対象 |
|---|---|
| 必須 | CMake、出力名、リソース、`build.bat` |
| 必須 | `src/harness` のPython／PowerShell、BAT、テストの固定名 |
| 必須 | README、CURRENT_STATE、OPEN_ISSUES、DEVELOPMENT、回帰手順、releases、requirements |
| 必須 | `.github` のワークフロー、成果物名、URL |
| 必須 | 新リポジトリ、remote、ローカルフォルダー、実験Worktree参照 |
| 要判断 | 旧EXE名を短期間残すか、設定・接続コード互換性をどう扱うか |
| 保全 | 日付付き設計・changelog・過去ログは当時の名称を原則改変しない |

## 容量整理候補（未削除）

| 場所 | 概算容量 | 方針 |
|---|---:|---|
| `build_logs/` | 約41.0 GiB | 証拠ログ。保持期限とアーカイブ先を決めてから整理する。 |
| `_TEST_MBAACC/` | 約30.5 GiB | 通常と比較環境が混在。分類後に不要な独立コピーだけ整理する。 |
| `_TEST_CLOSE_MBAACC/` | 約4.4 GiB | 隔離環境。通常同期対象外。 |
| `build/` | 約332 MiB | 再構成可能だが、移行完了まで差し替え元として保持する。 |

## 実施順序

1. 配布マニフェストと同期BATを作成し、ドライランで削除対象を記録する。
2. 通常テスト環境だけで差し替え、ハッシュと起動を確認する。
3. `CCCaster_vB1` を作成し、旧フォルダーをアーカイブとして保全する。
4. 現行ソース・スクリプト・文書・CIを改名し、ビルド・テストと旧名検索で確認する。
5. GitHub新リポジトリとremoteを切り替える（外部公開変更のため実行前に確認する）。
6. 証拠ログと独立テストコピーを保持規則に沿ってアーカイブ／削除する。
