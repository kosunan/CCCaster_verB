# CCCaster_v10

開発の入口: [現行仕様](docs/CURRENT_STATE.md) · [未解決事項](docs/OPEN_ISSUES.md) · [開発手順](docs/DEVELOPMENT.md)。以下の日付付き説明より現行仕様を優先してください。

## 2026-09-11 の現行実装

ランクマッチとマッチングサーバー構築は凍結。試作コードは保存し、旧CCCasterの機能復元と使いやすさを優先する。ネット対戦の観戦を実装し、確定入力中継・途中参加・V10共通の高速スキップ・再戦追従を追加した。[観戦の実装と確認範囲](docs/design/2026-09-12_spectator_stream.md)。

ユーザー向けGUIは `build/bin/CCCaster_v10_GUI.exe`。英語を既定とし「日本語 / English」で画面全体を切り替える。メルティブラッドの参考画像に合わせた黒・深紫・紅色・青白い文字と太字を使ったアーケード調の画面で、対戦・観戦・トレーニング・操作ガイドを選ぶ。対戦募集、接続コード参加、コピー、キャンセルに対応。実行時は `MBAA.exe` の隣の `cccaster/` 内にGUI・`libcccaster_hook.dll`・`cccaster_v10.ini` を配置する。GUIからの実対戦は未確認。トレーニングは `TRAINING` → `START TRAINING`（日本語では「トレーニング」→「トレーニングを開始」）。観戦は募集側のS-コードを観戦画面へ貼り付けて開始する。最大8人、募集側のTCPポートへの到達が必要。CLIは `CCCaster_v10.exe --spectate --hash S-...`。ローカルBroadcast・別PC実回線は未確認。

テストは引き続き `CCCaster_v10.exe --headless ...` と既存harnessを使用する。GUIは別EXEで、ゲームへの `headlessMode` はfalse。既存CLIの引数と対話メニューは維持。[GUIの実装・確認範囲](docs/design/2026-09-11_gui_launcher.md)。

入力経路と表示待機短縮は `docs/design/2026-09-11_input_latency.md`。完成画像をPresentで提示してから次フレームを準備する。旧InputInjectorの直接書込みは禁止。

英語HUD・設定変更の同期仕様は `docs/design/2026-09-11_netplay_hud.md`。

相手時計の推定・締切同期・ゲーム限定IAT・高精度待機は `docs/design/2026-09-11_clock_sync.md`。両端とも通信版10を使用する。

入力時計・フレーム差補正・保存最適化は `docs/design/2026-09-11_input_clock.md`。ゲーム更新と入力採取を分離し、旧世代の入力は画面境界で停止する。

全体のリファクタリング内容は `docs/design/2026-09-10_modernization.md`。書式は `.clang-format` に統一する。旧MatchSceneと旧16bitロールバックエンジンは撤去済み。

再戦は双方のワンスでのみ成立し、片側キャラセレでキャラクター選択へ進む。ネット対戦中のリプレイ保存は無効。再戦設計は `docs/design/2026-09-10_rematch.md`。

戦闘中の入力予測、状態保存・復元、ロールアップを接続済み。通常フレームでは入力を早期採取して中央バッファへ公開し、相手入力が未達なら前回値を維持する。不一致はゲームスレッドで最古のフレームから再計算する。途中画像のPresent転送と通常のフレーム待機を止める。巻き戻し中も独立したWASAPI入力時計が採取を続け、ゲームは蓄積した入力を順番に消費する。

通信版10。Dは入力遅延、Rは予測上限。D>=0、R>=0、D+R<=8、既定D=2/R=4。キャラセレのCtrl/Alt+数字で次戦のD/Rを変更できる。対戦進行中は固定。KO・時間切れ・画面境界は入力確定後に進む。通常3秒、起動・合流30秒の待機はユーザー承認済みで、過去の待機禁止の例外。

現行設計は `docs/design/2026-09-10_rollback.md`、検証結果は `docs/design/2026-09-10_rollback_results.md`。以下の2026-08-13監査は当時の記録であり、現行実装の説明には使わない。サブエージェントは使用しない。


CCCaster_v10 は、旧来の解析難易度が高く密結合だったツール構造（旧CCCaster）を見直し、「開発・拡張が容易な近代化された構造」へと再設計した次世代の格闘ゲーム用通信同期（ロールバック）ツールです。

本プロジェクトは **「対戦にかかわる処理はすべてゲーム内（インゲーム）で行う」** および **「妥協なきパフォーマンスチューニングの徹底」** を基本設計理念としています。

## 主な機能とモジュール構成

1. **`Network`**: UDP通信、冗長化パケット（Redundancy）、セッション管理
2. **`Sync (Rollback Engine)`**: GGPO由来のState保存・巻き戻し・再計算、高精度フレームタイマー
3. **`GameInterface`**: API（DirectX, Input）フック、シーン監視、メモリアドレスの直接操作
4. **`App / UI`**: アプリケーションライフサイクル、コマンドラインUI、およびDirectXオーバーレイ描画

## 開発に参加する方へ

大規模な変更を加える前には、本プロジェクトの設計理念および実装ルールに必ず従ってください。
開発への参加方法、ブランチ・コミットのルールについては [CONTRIBUTING.md](CONTRIBUTING.md) を参照してください。
また、ソースコード中の専門用語に関しては [GLOSSARY.md](GLOSSARY.md) で定義されています。

## プロジェクト構造

* `src/`: ソースコード（`core_dll`, `cli_launcher`, `launcher` に分離）
* `docs/`: 過去の要件定義・設計資料（現行仕様との乖離あり。[AGENTS.md](AGENTS.md) 参照）
* `tests/`: ユニットテストおよびベンチマークコード
* `changelogs/`: バージョンおよび月別の変更履歴

## ビルド方法

ビルドは CMake と C++20 を使用して行います。
詳細なビルド手順は `.agents/workflows/build.md`、または `docs/` 配下のアーキテクチャ資料を参照してください。

```bash
# 依存ライブラリの自動取得とビルド
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j12
```

## Steam版

Steam版は[独立プロジェクト](../CCCaster_Steam/README.md)で開発します。カニファン版と微差があるためクロスプレイは対象外です。
