# CCCaster_v10からCCCaster_vB1への初期移行記録

2026-09-14に、旧環境を変更せず、新しい `I:/work_space/CCCaster_vB1` を作成した。

## 複製したもの

- `src/`: 旧環境のCMake、C++ソース、サーバーソース、ビルド補助、CI、開発ルール。
- `docs/`: 現行仕様、設計、課題、リリース資料、changelog。
- `test/source/`: テストコードと補助スクリプト。
- `test/fixtures/System/`: テストで必要な固定設定。

## 持ち込んでいないもの

- `build/` と各種の生成済みバイナリ。
- `build_logs/`（約41GiB）。
- `_TEST_MBAACC/`（約30.5GiB）および `_TEST_CLOSE_MBAACC/`（約4.4GiB）のゲーム実体。
- 旧配布ステージング、AIのローカルキャッシュ、旧Git作業ツリーそのもの。

これらは旧環境に保全する。必要になったものだけを `test/runtime/`、`test/logs/`、`release/packages/`、`archive/` へ目的別に移す。

## Git履歴

新環境は独立したGitリポジトリとして初期化し、旧環境を `legacy` remoteに登録した。旧履歴は `legacy/master`、`legacy/backup-full-history`、`legacy/codex/experiments-20260911` として参照できる。新しいGitHubリポジトリの作成・push・remote切替はまだ実施していない。

## 未実施

- `CCCaster_v10` から `CCCaster_vB1` へのソース・生成物名の改名。
- 新環境での32bitビルド・テスト。
- 通常テスト用ゲームコピーの新規作成と、強制差し替えBATの配置。
- 旧環境のアーカイブ化、ログ・旧テストコピーの整理。
