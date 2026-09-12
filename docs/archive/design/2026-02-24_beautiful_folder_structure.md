# The Most Beautiful Folder Structure for CLI & DLL Separation

C++の大規模開発やモダンなCMakeプロジェクトにおいて、「呼び出しホスト(CLI)」と「インジェクトDLL(Core)」を分離する上で**最も美しく、保守性が高く、バグを物理的に防げる完璧なフォルダ構成**をご提案します。

結論から言うと、美しさの絶対条件は **「AppとDLLを完全に別プロジェクトとして扱い、唯一の架け橋(Public API)だけで繋ぐ」** ことです。

---

## 💎 最も美しい理想のフォルダ構成 (Modern CMake Multi-Project Layout)

```text
CCCaster_v10/
├── CMakeLists.txt              # 全体を束ねるマスタースクリプト
│
├── app/                        # 🏠 呼び出しホスト (CCCaster_v10.exe)
│   ├── CMakeLists.txt          # App専用のビルド手順
│   ├── src/                    # Appの実装 (main.cpp, CliMenu.cpp)
│   │   ├── ui/
│   │   ├── controller/
│   │   └── network_wrapper/
│   └── include/                # App内「だけ」で使うヘッダー
│
├── core_dll/                   # 🧠 頭脳部 (cccaster_hook.dll)
│   ├── CMakeLists.txt          # DLL専用のビルド手順
│   ├── src/                    # 隠蔽された内部実装 (.cpp)
│   │   ├── common/
│   │   ├── input/
│   │   ├── network/
│   │   ├── sync/
│   │   └── memory_hooks/
│   └── include/cccaster/       # DLL内部の各モジュールが教え合うためのヘッダー
│       └── ...
│
├── public_api/                 # 🌉 【重要】AppとDLLを結ぶ唯一の架け橋
│   └── include/
│       └── cccaster_api.h      # extern "C" の関数宣言や、両者が知るべき定数のみを配置
│
└── docs/                       # 設計書など
```

---

## 🌟 なぜこの構造が「最も美しい」のか？

この構造がC++開発において至高とされる理由は、**「やってはいけないこと」をファイルシステムとビルドシステム(CMake)のレベルで物理的に禁止できる**からです。

### 1. 物理的なカプセル化（Includeの完全遮断）
App（CLI側）の開発をしているとき、プログラマは `core_dll/include/` の中身を `#include` することは **CMakeによってアクセス拒否** されます。
App側が知ることができるのは、`public_api/include/` にある少数の公開インターフェースだけです。これにより、「Appが誤ってDLL専用の複雑な内部クラス（RollbackEngineなど）を直接触ってしまい、依存関係がスパゲティになる」というC++特有の悲劇を**100%未然に防ぎます**。

### 2. CMakeListsの自己完結 (モジュール化)
巨大な一つの `CMakeLists.txt` に全てのソースファイルを書くのではなく、各々のフォルダに小さな `CMakeLists.txt` を置きます。
- `app/CMakeLists.txt` は「exeを作る」ことだけを記述。
- `core_dll/CMakeLists.txt` は「dllを作る」ことだけを記述。
トップレベルの `CMakeLists.txt` は `add_subdirectory(app)` と書くだけで済み、ビルドの保守性が劇的に向上します。

### 3. 名前空間 (Namespace) との完璧な一致
C++における最も美しい名前空間マッピングが自然に行えます。
- DLL内部のコード: `namespace cccaster::network { ... }`
- App内部のコード: `namespace cccaster::app { ... }`
フォルダ構造がそのまま名前空間を表すため、IDEでの検索性が極限まで高まり、開発者の「今どこを書いているのか」という認知負荷がゼロになります。

---

## 結論

以前の「連番(01_app, 02_core_dll)」アプローチは、種類ごとに層を分けるという意図は良かったのですが、C++のインクルード解決の仕組みと相性が悪く、パスが汚くなる欠点がありました。

今回提案した **「独立したサブディレクトリ型（App, DLL, API）」** こそが、現代のC++オープンソースプロジェクト（LLVM, Google系ライブラリ, 昨今のゲームエンジン等）で採用されている、**最も拡張性が高く美しい完全な分離構造**です。

現在の設計（Appを単なるランチャーにし、DLLを頭脳とする）を最大限に活かすなら、この構成が間違いなくベストです。
