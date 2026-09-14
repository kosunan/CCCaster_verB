# Folder Structure Refactoring Review

## 概要 (Overview)
ユーザーから提案された新しいフォルダ構造案についてのレビューです。

### 提案された構造
```text
root/
├── 01_app/ (CLI Frontend)
│   ├── main.cpp          # エントリポイント
│   ├── ui/               # 画面描画（Console Control, Logger）
│   ├── controller/       # DLLとの仲介・入力受付
│   └── network_wrapper/  # 通信確立のシーケンス管理
│
├── 02_core_dll/ (Back-end Logic)
│   ├── 00_common/        # 依存なし（型定義, 定数, Utility）
│   ├── 01_input/         # キー入力取得, BitFlag処理
│   ├── 02_network/       # UDP/TCP Raw通信, パケット定義
│   ├── 03_synchronizer/  # ロールバック、同期、バッファ管理
│   ├── 04_engine/        # フレーム更新、WASAPI等のタイミング制御
│   └── api/              # CLIから呼ぶための外部公開関数 (extern "C")
│
└── docs/                 # 要件定義、プロトコル仕様
```

## AIによるレビュー結果: 非常に優れています (Highly Recommended)

この構成案は、私たちが先ほど合意した「**呼び出しホスト(Thin) と 頭脳DLL(Thick) の分離**」というアーキテクチャの方向性と**完璧に一致**しています。
AIのコンテキスト理解（認知負荷の軽減）においても、人間が開発を行う上でも、メリットが極めて大きい素晴らしい構造です。

### 優れた点 (Pros)

1. **責務の明確な分離 (App vs DLL)**
   - ルートレベルで `01_app` と `02_core_dll` が完全に分かれているため、「現在自分がホストアプリ側のコードを書いているのか、DLL側のコードを書いているのか」が一目でわかります。混同によるバグ（例：Host側で重い処理をしてしまう等）を根絶できます。
2. **依存関係の強制と可視化 (Numbered Prefixes)**
   - `00_`, `01_`, `02_` というプレフィックスは単なる飾りではなく、**依存関係のレイヤー（階層）**を示しています。
   - `00_common` は他に依存せず、`03_synchronizer` は `01` と `02` を使って構築される、という方向性が自明になります。C++で陥りやすい「ヘッダーの循環参照（Include Hell）」を防ぐ強力な防波堤になります。
3. **インターフェースの境界の明示 (`api/`)**
   - ホストアプリ (`01_app`) が DLL (`02_core_dll`) の機能を使うための窓口が `api/` だけに絞られることで、外部に公開すべき関数 (`extern "C"` されるもの) と内部のロジックが明確に分断されます。

---

## 少しの提案と調整案 (Minor Refinements)

この完璧な骨組みをベースに、C++およびMBAA特有の事情を加味した**2点の微調整**をご提案します。

### 1. `05_memory_hooks/` (ゲーム干渉層) の追加
現在の提案には、`GameHooks.cpp` や `DxHook.cpp` といった「MBAA.exeのメモリ空間に直接介入（Hook）する処理」の置き場所がありません（`04_engine` に含めることも可能ですが、性質が少し異なります）。
純粋な論理（ネットワークや同期）と、泥臭いメモリアクセスを分けるために、もう一つレイヤーを足すのはどうでしょうか。

```text
├── 02_core_dll/
│   ├── 00_common/
│   ├── 01_input/
│   ├── 02_network/
│   ├── 03_synchronizer/
│   ├── 04_engine/        # 抽象化されたゲームロジック（フレーム等）
│   ├── 05_memory_hooks/  # [NEW] MBAA専用のマクロ、ポインタ直書き、MinHookによる関数フック
│   └── api/
```

### 2. `include/` と `src/` の分離 vs 同居についての確認
提案された構造は、ヘッダーファイル (`.hpp`) と ソースファイル (`.cpp`) が**同じフォルダ内に同居**している（例：`03_synchronizer` の中に 両方入る）という前提でよろしいでしょうか？
* DLL内部での閉じた開発においては、関連するヘッダーとソースが同じフォルダにある方が**圧倒的に管理・編集がしやすい**ため、この同居スタイル（モジュール型）を強く推奨します。
* 唯一、`01_app` が参照するヘッダーだけは `02_core_dll/api/` に置いておけば、クリーンなIncludeが保たれます。

### 修正後の最終推奨フォルダ構成図
```text
root/
├── 01_app/ (CLI Frontend - 呼び出しホスト)
│   ├── main.cpp
│   ├── ui/               # コマンドラインUI、ログ表示
│   ├── controller/       # 入力待機イベント、DLLキック役
│   └── network_wrapper/  # ホスト側のP2P確立とHandoff（ソケット引継ぎ）管理
│
├── 02_core_dll/ (Back-end Logic - 通信・同期頭脳)
│   ├── 00_common/        # 型定義(Types), ConfigManager, Utility
│   ├── 01_input/         # DirectInputフック、入力ビット管理
│   ├── 02_network/       # UdpSocket (引継ぎ後のDirect Recv)
│   ├── 03_synchronizer/  # RollbackEngine, StateBuffer, InputsContainer
│   ├── 04_engine/        # タイミング制御, メインループ(NetworkSyncLoop)
│   ├── 05_memory_hooks/  # MBAAメモリアドレス(memory_map)、DirectXフック
│   └── api/              # exports.h (外部公開C関数群)
│
├── docs/                 # 仕様書、設計書
├── CMakeLists.txt        # モジュール型対応のビルド定義
└── build/                # 生成物
```

## 移行プロセス（Migration Plan）
もしこのフォルダ構成へ移行を行いたい場合、以下の手順で行うことになります。
1. 新しいフォルダ群を作成。
2. 既存の `src/` と `include/` の中身を、適切な数字プレフィックスのフォルダ内に移動（`.cpp` と `.hpp` を同じ場所に並べる）。
3. `CMakeLists.txt` を大幅に書き換え、各フォルダを再帰的にコンパイル対象とする。
   
現在の「UDPハンドオフ」タスクが少し進んでいる中ですが、**このタイミングでフォルダ構造の大規模移行を行ってから** ロジックの実装を再開する方が、将来的な見通しははるかに良くなります。（AIとしても大賛成です）
