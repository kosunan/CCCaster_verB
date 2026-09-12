# 05 Test Specification (テスト仕様・合否判定基準)

## 1. 概要
CCCaster_v10 におけるテスト体系の全容と、AI自動テスト時のインデックス、各モードの合否判定基準を定義します。テストは4つのフェーズで段階的に構成されます。

## 2. 自動テストワークフローの実行方法
日常的な自動テスト実行は、ショートカットコマンドを用いて行います。
> `/.agents/workflows/test.md` を **/test** コマンドで呼び出す

## 3. テストフェーズ構成と合否判定基準

### Phase 1: ビルド確認
- **目的**: コンパイラによる構文・リンクエラー、Missing Headerなどの静的検証。
- **実行方法**: `/.agents/workflows/build.md` を `/build` で呼び出す。
- **成功判定**: コンパイルエラーゼロで `build/bin/` に成果物が出力されること。

### Phase 2: UI動作確認
- **目的**: ユーザーが直接操作する CLI UI の遷移とメニュー描画の検証。
- **実行方法**: 引数なしで `build/bin/CCCaster_v10.exe` を手動起動。
- **成功判定**:
  - メインメニューが意図した項目構成で表示されること。
  - 各階層（Netplay/Offline/Spectate等）への遷移およびESCによるキャンセルが正しく動作すること。
  - "Coming soon" などのStub実装が意図通り表示されること。

### Phase 3: DummyPeer連携テスト (CLIヘルスチェック)
- **目的**: 1台のPC内で、DLL側の通信・同期コードをゲーム不要で疑似テストする。主にSessionNegotiator（UDP接続の確立）と高精度タイマー、RollbackEngineなどを検証。
- **実行方法**: `tools/dummy_peer/TEST_PROCEDURE.md` を参照して手動実行またはスクリプト化。
- **テストシナリオ群**:
  - DI-1: `CCCaster(Host)` ← `DummyPeer(Client)`
  - DI-2: `DummyPeer(Host)` ← `CCCaster(Client)`
  - DI-3: InGame層同期テスト (SyncTest: `DummyPeer Client` → `CCCaster Host`)
- **成功判定**: DummyPeerおよびCCCasterのログに `[ SUCCESS ] Connection established` や `Lock-in: SUCCESS` が表示され、パケットの往復が実証されること。

### Phase 4: 回帰テスト（ローカルループバック統合確認）
- **目的**: UI入力をバイパスするヘッダレスモードを用い、ホスト・クライアント両方のプロセスを同一PC上で立ち上げ、通信確立からゲームプロセス（MBAA.exe）の起動・DLLインジェクトまでのフロー全体を結合確認する。
- **実行方法**: `/test` コマンドで実行可能。
- **成功判定**: 
  - 相互に `[ SUCCESS ] Connection established` と出力。
  - 接続から2秒待機後、`Auto-Locking connection` からIPCへの書き込みが発生し、`GameLauncher`が呼び出されること。（※ MBAA.exeが存在しない環境ではここでExitとなるが、接続テストとしては成功とみなす）

## 4. ヘッドレスモードの引数仕様
自動テスト向けに定義されたCLI引数リファレンスです。
| 引数 | 説明 | 例 |
|---|---|---|
| `--headless` | 【必須】メニュー表示を取りやめNetplayへ直行 | `--headless` |
| `--host` | ホストとして待受開始 | `--host` |
| `--ip` | クライアント時に接続するIPアドレス | `--ip 127.0.0.1` |
| `--port` | 自身がバインド(Host)または接続(Client)するポート | `--port 10800` |
| `--ipv6` | IPv6ソケットモードを有効にする | `--ipv6` |

> [!WARNING]
> テスト実行中は必ずプロセスがゾンビ化しないよう、事前・事後のクリーンアップ処理（`Stop-Process -Force` 等）を組み込むこと。
