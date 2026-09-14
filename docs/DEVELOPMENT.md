# 開発環境と引き継ぎ

## 2026-09-14 移行後の作業入口

現在の作業場所は `I:/work_space/CCCaster_verB`。以下の旧v10パスは移行前の記録。
ルートの `build.bat` で32bit Release構成・ビルド・CTestを実行する。
CMake入力は `src/`、生成物は `build/bin/`。Pythonテストは `python -X utf8 -m unittest discover -s src/src/harness -p test_*.py`。
同期harnessは `powershell -NoProfile -ExecutionPolicy Bypass -File src/src/harness/run_bounded_pair.ps1`。ログは `test/logs/`。
実ゲームの旧スクリプトはまだ旧ディレクトリ前提であり、移行確認前に実行しない。

## メモリ情報を調べるとき

[MBAACCメモリ知識ベース](memory/README.md)にETM参照調査、型・offset・出典付き262フィールド、現行保存表との差を保管する。現行アドレス定義を優先し、参照ソース上の確認と対象版の実機確認を区別する。再生成方法・固定コミット・SHA256も同資料を参照。

## 日常の回帰チェック

`python -X utf8 src/harness/run_regression.py --profile quick`で32bitビルド・CTest・Python判定器・4倍速の同期harnessを一括実行する。入力・同期・フック変更では`--profile real`、再戦・選択を含む変更では`--profile full`を使用する。失敗ケースだけの再実行は`--case retry_0`等を指定する。試験の範囲・チートの条件・結果JSONは[自動回帰チェック](REGRESSION_TESTING.md)、機能の完了判定は[ロードマップ](ROADMAP.md)を参照。

GitHubのpush/PR用ワークフローはLinuxの同期ロジックと判定器を対象にする。Windows実ゲームの自動試験とは別の検証範囲であり、GitHub上での実行はまだ未確認。

## 3つの機能

1. AGENTS.md: 現行ルールを短く固定する。仕様はCURRENT_STATE、未解決事項はOPEN_ISSUESへ分ける。
2. cccaster-verifyスキル: `.agents/skills/cccaster-verify`がGit保管する正本。このPCでは `C:/Users/junna/.codex/skills/cccaster-verify` にも同内容を登録している。正本を更新したら登録先にも反映する。明示的に `$cccaster-verify` と依頼できる。
3. Worktree: 同じGit履歴を使う独立した実験用のソース・ビルド場所。人数指定がなければ単独で作業し、指定された場合はその人数のサブエージェントを使う。実機試験と共有ファイルの変更は担当を分け、競合させない。

## 作業場所

| 用途 | 場所 | 方針 |
|---|---|---|
| 現在の開発・実機環境 | `I:/work_space/CCCaster_v10` | 既存未コミット変更を維持。現在のチャットはこの場所を継続使用 |
| 実験用Worktree | `I:/work_space/CCCaster_v10_experiments` | `codex/experiments-20260911`。作成時の現行ソースを基準コミットとして保存 |

現在のGit HEADは古いため、Worktreeには元フォルダーの追跡済みファイルと無視されていない新規ソース・資料を反映する。元フォルダーのHEAD・indexには触れない。基準コミットはその時点の状態保存であり、全問題解決・安定版認定ではない。Git履歴を巻き戻さず、このコミットとの差分で実験を評価する。

Worktree間でbuild、ログ、ユーザー設定、ゲーム本体を共有しない。実験を本線へ反映する際は基準コミットより後の差分をレビューする。実験ブランチ全体を元ブランチへ一括マージすると既存未コミット変更と重なるため、何を取り込むか先に確認する。

## 実験用の初回ビルド

そのWorktreeを作業ディレクトリとして実行する。

```powershell
$env:PATH='C:/msys64/mingw32/bin;C:/msys64/usr/bin;'+$env:PATH
cmake -S . -B build -G 'MinGW Makefiles' -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=C:/msys64/mingw32/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/msys64/mingw32/bin/g++.exe
cmake --build build -j8
ctest --test-dir build --output-on-failure
```

依存ライブラリの取得を省く場合は、元環境の `build/_deps/asio-src`、`minhook-src`、`imgui-src` を `FETCHCONTENT_SOURCE_DIR_ASIO` 等で参照できる。これは外部依存のソース参照のみ。生成物はWorktree側に作る。依存ソース自体を変更する実験では独立コピーに切り替える。

実験用Worktreeはソース・ビルド・単体テスト用として用意し、ゲーム本体は複製しない。実験版の実機試験が必要になった段階で独立したテストコピーを用意する。元のゲームフォルダーへのジャンクションは作らない。

## 作業の終わり

- 実装変更に見合う検証だけを行い、結果と未確認事項を記録する。
- CURRENT_STATEとOPEN_ISSUESを更新し、根拠ログの所在を残す。
- 変更をコミットする際は対応するchangelogと同じ日本語見出しを使う。
- チャットを切り替える際は、作業場所・ブランチ・基準コミット・次の課題を伝える。過去会話が全文自動継承されることを前提にしない。

## この環境の確認結果（2026-09-11）

- 実験用Worktreeで新規構成したReleaseビルド成功。EXE/DLLはPE Machine 0x014c（32bit）。
- 14テスト成功。ログは実験先の `build/Testing/Temporary/LastTest.log`。
- 専用スキルは公式quick_validateで形式検証成功。正本とこのPCへの登録内容は一致。
- 現行資料の相対リンクを確認済み。元のHEADは4942dca、元indexは変更なし。
- 保存した基準点のタグは `baseline/2026-09-11-codex-setup`。安定版認定ではなく、この時点へ参照できる記録。
- ゲームコードは今回変更していないため実機対戦は繰り返していない。以前の実機結果はCURRENT_STATEを参照。

## Steam版の別プロジェクト

Steam版は独立Gitリポジトリ I:/work_space/CCCaster_Steam（codex/steam-port）へ分離した。ソース・ビルド・設定・ゲームコピーを共有せず、カニファン版とはクロスプレイしない。以後のSteam移植は分離先で継続する。

## 仮想コントローラの実ゲーム自動試験

`src/harness/run_virtual_controller_pair.py` はViGEm DS4を2台作成し、DirectInputの実取得経路で方向・ボタン・左右分離を照合してから対戦・再戦を自動操作する。既存ドライバと隔離Python環境を使用する。実行コマンド、前提条件、結果JSONと試験の制約は[手順書](design/2026-09-11_virtual_controller_test.md)を参照。
