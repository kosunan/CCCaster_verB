# App Directory Reorganization Plan (CLI Logic, UI, Network, and Launcher Extraction)

ユーザーからの「`src/app` の中を CLIメニューロジック、CLI_UI、通信に関する括りで分割し、ゲーム起動関係は別の場所（`src/app` の外）に移動させる」という指示に基づき、以下の内部再構築を行います。

## 1. 隔離されるゲームランチャー (Host Process Launcher)
ゲーム（MBAA.exe）の起動やプロセスの監視を行うコード（`GameLauncher`等）は、CLIのUIやネットワーク通信とは責務が全く異なります。これらはアプリケーションの土台（プラットフォーム・ホストレイヤー）として、`src/launcher/` または トップレベルの `src/host/` のような別モジュールへ隔離します。

```text
src/
├── launcher/                   # [NEW] ゲームプロセスの起動・監視・終了管理
│   ├── GameLauncher.cpp
│   └── GameProcessManager.cpp  # (必要に応じて)
```

## 2. アプリケーション層の分割概要 (`src/app/`)
純粋なCLIアプリケーション本体として、入力・描画・通信シーケンスを3つの層に分けます。これにより、`CliMenu.cpp` という巨大な神クラス（God Class）を物理的に解体します。

```text
src/app/
├── main.cpp                    # アプリケーションのエントリポイント
│
├── ui/                         # [NEW] CLI_UI: 画面描画、メニューのテキストレイアウト
│   ├── ConsoleRenderer.cpp     # NativeClearScreen, PrintHeader, DrawMenuAndGetSelection
│   └── Logger.cpp              # コンソールやファイルへのログ出力
│
├── controller/                 # [NEW] CLIメニューロジック: ユーザー入力受付と状態遷移
│   └── MainController.cpp      # 昔の CliMenu.cpp の Run() ループや入力受付ロジック
│
└── network_wrapper/            # [NEW] 通信確立のシーケンス管理 (P2Pネゴシエーション)
    ├── SessionNegotiator.cpp   # IP/Port入力受付〜Ping交換〜Handoff直前までの通信確立ロジック
    └── NetworkUtils.cpp        # GetGlobalIp() などのユーティリティ
```

## 3. リファクタリングの手順 (Execution Steps)
ディレクトリのみを作ってもコードが巨大なままでは意味がないため、物理ディレクトリの作成と同時に、`CliMenu.cpp` 内に混在している機能群を上記の新しいクラス・ファイル群へと分割（抽出）していくコード改修をセットで行う必要があります。

現在のフェーズ（フォルダ構成の大規模移行）の仕上げとして、この内部再編とランチャーの隔離を実施し、CMakeLists.txt に新しいディレクトリツリーを登録します。
