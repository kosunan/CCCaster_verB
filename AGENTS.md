# CCCaster_verB 作業入口

- 作業ルートは `I:/work_space/CCCaster_verB`。
- 現行仕様は `docs/CURRENT_STATE.md`、課題は `docs/OPEN_ISSUES.md`。
- 製品ソースと追加開発ルールは `src/src/` と `src/AGENTS.md`。
- CMakeの入力は `src/`、生成先はルートの `build/`。ルートの `build.bat` でRelease 32bitビルドとCTestを実行する。
- Python解析テスト: `python -X utf8 -m unittest discover -s src/src/harness -p test_*.py`。
- `src/build.bat` と旧回帰スクリプトには旧配置の前提がある。新配置への対応を確認せず実行しない。
- 旧環境のゲーム・設定・ログは保全する。テスト用ゲームの配置先は `test/runtime/`、ログは `test/logs/`。
- 一人で作業し、応答と変更履歴は日本語。通信版10や接続コードのソルトを名称変更で変更しない。
- GitHub CIはルート `.github/workflows/`。製品バージョンは `VERSION` のみを変更し、手順は `docs/VERSIONING.md`。固定名はCCCaster_B.exeとCCCaster_B_GUI.exe。
