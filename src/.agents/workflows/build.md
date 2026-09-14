---
description: CCCaster_v10 のビルド手順（クリーンビルド / インクリメンタルビルド）
---

# CCCaster_v10 ビルド手順

## 前提条件
- **MSYS2/MinGW (32bit)** がインストール済み（パス: `C:\msys64\mingw32\bin`）
- **CMake 4.0+** がインストール済み
- ジェネレータ: `MinGW Makefiles`
- コンパイラ: GCC 15.x (mingw32)

## 環境変数の設定
ビルド前に必ずコンパイラのパスを通すこと:
```powershell
$env:PATH = "C:\msys64\mingw32\bin;C:\msys64\usr\bin;" + $env:PATH
```

---

## インクリメンタルビルド（通常作業時）
ソースを変更した後、差分のみ再コンパイルする場合:
```powershell
// turbo
cmake --build build -j12
```

> **注意**: ビルドが不安定な場合（Makefileが壊れた等）はクリーンビルドを行うこと。

---

## クリーンビルド（キャッシュ破損時・CMakeLists変更時）

### 1. buildフォルダを完全削除
```powershell
Remove-Item -Recurse -Force I:\work_space\CCCaster_v10\build\*
```

### 2. CMake Configure（プロジェクト設定の再生成）
```powershell
cmake -B build -S . -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
```
- **Debug**: ログ・アサート有効（開発用）
- **Release**: 最適化`-O3`、ログ無効化（配布用）

### 3. ビルド実行
```powershell
// turbo
cmake --build build -j12
```

---

## 成果物の場所
| ファイル | パス |
|---|---|
| メインEXE | `build/bin/CCCaster_v10.exe` |
| フックDLL | `build/bin/libcccaster_hook.dll` |

---

## テスト環境へのデプロイ
ビルド成功後、テスト用 MBAACC フォルダ（2窓分）に成果物をコピー:
```powershell
// turbo
foreach ($i in 1,2) { Copy-Item -Force "I:\work_space\CCCaster_v10\build\bin\CCCaster_v10.exe","I:\work_space\CCCaster_v10\build\bin\libcccaster_hook.dll" "I:\work_space\CCCaster_v10\_TEST_MBAACC\MBAACC_$i\cccaster\" }
```

> `build.bat` を使えば configure → build → 2窓デプロイまで一括で行われる。

> **注意**: デプロイ先のゲームが起動中の場合、DLL がロックされてコピーに失敗します。先にゲームを終了してください。

---

## ビルドログの管理
- ビルドログは **`build_logs/`** フォルダに保存すること（`.gitignore`済み）
- **プロジェクトルートにログファイルを放置しないこと**
- ログ出力例:
```powershell
// turbo
cmake --build build -j12 2>&1 | Tee-Object -FilePath build_logs\build_YYYYMMDD.log
```

---

## トラブルシューティング

### Makefileが見つからない / ビルドキャッシュ破損
```
mingw32-make: Makefile: No such file or directory
```
→ 上記「クリーンビルド」の手順を実行

### エディタのCMDがハングしている場合
ビルド前にハングプロセスを確認・終了:
```powershell
Get-Process -Name "cmake","mingw32-make","cc1plus","g++","gcc","as","ld" -ErrorAction SilentlyContinue | Stop-Process -Force
```

### FetchContent警告（CMP0169）
```
Calling FetchContent_Populate() is deprecated...
```
→ 動作に影響なし。将来的に `FetchContent_MakeAvailable()` へ移行推奨。
