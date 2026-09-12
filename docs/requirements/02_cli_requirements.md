# 02 CLI Requirements (CLI UI 機能要件)

## 1. 概要
CLI (コマンドラインインターフェース) は、メインアプリ (CCCaster_v10.exe) がゲーム本体 (MBAA.exe) に干渉可能になるまでの「対戦ルーター・ランチャー」として機能します。

## 2. メインメニュー構成
メニューは旧CCCasterの構成を踏襲した9項目 + Exitで構成されます。上/下カーソルで選択し、Enterで決定します。

| # | 項目名 | 選択時の動作 |
|---|---|---|
| 0 | **Netplay** | サブメニューを表示し、Hash Connect または Direct IP Connect を選択させる。 |
| 1 | **Spectate** | 観戦接続モードへ移行する。 |
| 2 | **Broadcast** | (Stub) "Coming soon..." と表示しメニューへ戻る。 |
| 3 | **Offline** | ローカルのトレーニングモードやVS CPU用としてMBAA.exeを即時起動する。 |
| 4 | **Server** | (Stub) "Coming soon..." と表示しメニューへ戻る。 |
| 5 | **Controls** | (Stub) "Coming soon..." と表示しメニューへ戻る。 |
| 6 | **Settings** | (Stub) "Coming soon..." と表示しメニューへ戻る。 |
| 7 | **Update** | (Stub) "Coming soon..." と表示しメニューへ戻る。 |
| - | **Exit** | アプリケーションを終了する。 |

> [!NOTE]
> コントローラー設定等（Controls, Settings）の実装は、P2P接続確立後にゲーム内オーバーレイ（ImGui）上で行う設計としたため、CLI上での実装は当面見送り(Stub)としています。

## 3. Netplay メニューの動作仕様

Netplay選択時、以下のサブメニューが表示されます。
1. **Hash Connect**:
   - `[ Host / Client ]` を選択。
   - Host時はバインドするポートを入力し、接続用ハッシュ文字列を生成・表示（クリップボードにもコピー）。
   - Client時は提供されたハッシュ文字列を入力（ペースト）し、IP/Portをデコードして接続。
2. **Direct IP Connect**:
   - P2Pの直接接続モード。
   - `ip:port` 形式（例: `192.168.1.5:10800`）が入力された場合は**Client**として接続を試行。
   - `port` 番号のみ（例: `10800`）が入力された場合はそのポートでバインドする**Host**として待受開始。

## 4. ヘッドレスモード (`--headless`) 仕様
AIによる自動テストや外部ランチャーからの起動を想定した**UIスキップモード**です。
起動と同時に指定パラメーターを使用して自動的にNetplay接続（またはエラー終了）へ直行します。

- 必須引数: `--headless`
- モード指定: `--host` または `--ip <address>`
- ポート指定: `--port <number>`

**動作フロー**:
CLIメニューの描画関数を一切呼ばず、即座に `SessionNegotiator::RunNegotiation()` または `RunNegotiationFromHash()` を実行し、接続結果によって `GameRunning` ステートへ直行、または直ちにプロセスを `Exit` します。

## 5. UIの描画更新ルール
- 画面クリアは `system("cls")` ではなく、コンソールバッファを直接上書きする `NativeClearScreen()` を使用してチラつき（フリッカー）を防止します。
- メニュー表示中でも、バックグラウンドでのPing監視等は非同期で行われる設計とします。
