---
description: CCCaster_v10 のAI向け自動テスト実行手順（dual_test.bat 使用）
---

# CCCaster_v10 テスト実行手順

## 概要
`_TEST_MBAACC/dual_test.bat` を使い、ローカル2窓（Host/Client）でネット対戦統合テストを実行する。
MBAA.exe × 2 + CCCaster_v10.exe × 2 が起動し、DLLインジェクト→キャラセレ→InGame を自動で通過する。

---

## フォルダ構成

```
_TEST_MBAACC/
├── dual_test.bat            ← テスト起動バッチ
├── MBAACC_1/                ← HOST 側 ゲーム本体
│   ├── MBAA.exe
│   └── cccaster/
│       ├── CCCaster_v10.exe     ← ビルド成果物
│       ├── libcccaster_hook.dll ← ビルド成果物
│       └── cccaster_hook_log.txt ← DLL ログ出力
└── MBAACC_2/                ← CLIENT 側 ゲーム本体
    ├── MBAA.exe
    └── cccaster/
        ├── CCCaster_v10.exe
        ├── libcccaster_hook.dll
        └── cccaster_hook_log.txt
```

---

## 実行手順

### ステップ 1: ビルド成果物のデプロイ
// turbo
```powershell
Copy-Item "I:\work_space\CCCaster_v10\build\bin\CCCaster_v10.exe" "I:\work_space\CCCaster_v10\_TEST_MBAACC\MBAACC_1\cccaster\" -Force; Copy-Item "I:\work_space\CCCaster_v10\build\bin\libcccaster_hook.dll" "I:\work_space\CCCaster_v10\_TEST_MBAACC\MBAACC_1\cccaster\" -Force; Copy-Item "I:\work_space\CCCaster_v10\build\bin\CCCaster_v10.exe" "I:\work_space\CCCaster_v10\_TEST_MBAACC\MBAACC_2\cccaster\" -Force; Copy-Item "I:\work_space\CCCaster_v10\build\bin\libcccaster_hook.dll" "I:\work_space\CCCaster_v10\_TEST_MBAACC\MBAACC_2\cccaster\" -Force; echo "Deploy done"
```

### ステップ 2: 既存プロセスのクリーンアップ
// turbo
```powershell
Get-Process -Name "CCCaster_v10","MBAA" -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Seconds 2; echo "Cleanup done"
```

### ステップ 3: dual_test.bat を実行
```powershell
Start-Process "cmd.exe" -ArgumentList '/c "I:\work_space\CCCaster_v10\_TEST_MBAACC\dual_test.bat"' -WorkingDirectory "I:\work_space\CCCaster_v10\_TEST_MBAACC"
```
バッチが行う処理:
1. 既存プロセス kill
2. 古ログ削除
3. HOST 起動 (MBAACC_1, port 7500, ネットワークシミュレーション付き)
4. 3秒待機
5. CLIENT 起動 (MBAACC_2, 127.0.0.1:7500, ネットワークシミュレーション付き)
6. 20秒待機
7. 両方のログ末尾20行を表示

### ステップ 4: ログ確認（bat 完了後 or 手動）

HOST ログ確認:
// turbo
```powershell
if (Test-Path "I:\work_space\CCCaster_v10\_TEST_MBAACC\MBAACC_1\cccaster\cccaster_hook_log.txt") { Get-Content "I:\work_space\CCCaster_v10\_TEST_MBAACC\MBAACC_1\cccaster\cccaster_hook_log.txt" | Select-Object -Last 30 } else { echo "[HOST] No log file" }
```

CLIENT ログ確認:
// turbo
```powershell
if (Test-Path "I:\work_space\CCCaster_v10\_TEST_MBAACC\MBAACC_2\cccaster\cccaster_hook_log.txt") { Get-Content "I:\work_space\CCCaster_v10\_TEST_MBAACC\MBAACC_2\cccaster\cccaster_hook_log.txt" | Select-Object -Last 30 } else { echo "[CLIENT] No log file" }
```

---

## 成功判定

### 接続確立 (CCCaster_v10.exe stdout)
- `[ SUCCESS ] Connection established with [127.0.0.1:xxxxx]` が両窓に表示
- `Auto-Locking connection` が表示

### DLL 注入成功 (cccaster_hook_log.txt)
- `[SceneRunner] Init complete. Ready for Step() calls.` が両方のログに出現
- `[NetplaySession] Mode -> Counting` が出現（NTP 同期完了）

### ゲーム進行 (cccaster_hook_log.txt)
- `[SceneRunner] phase=2` (CharaSelect) のログが出現
- `[SceneRunner] phase=3` (Loading) のログが出現
- `[IntroBarrier] Both peers at intro=2!` が出現 → InGame 開始

### InGame フレーム同期 (cccaster_hook_log.txt)
- `wh=`, `rp=`, `crf=` の値が両者で近い値を示す
- `phaseBaseFrame=` の値が記録されている

---

## トラブルシューティング

| 症状 | 原因 | 対策 |
|------|------|------|
| ログファイルが生成されない | DLL 注入失敗 or MBAA.exe が起動していない | `_TEST_MBAACC/MBAACC_1/` に MBAA.exe が存在するか確認 |
| `Connection established` が出ない | ポート競合 or ファイアウォール | `netstat -ano | findstr 7500` で確認 |
| `Mode -> Counting` に到達しない | NTP 同期に時間がかかっている | 20秒以上待機して再確認 |
| ゲーム画面が黒い/フリーズ | dual_launch が未使用 | MBAA は同時2窓起動にパッチが必要 |

---

## テスト後クリーンアップ
// turbo
```powershell
Get-Process -Name "CCCaster_v10","MBAA" -ErrorAction SilentlyContinue | Stop-Process -Force; echo "All processes stopped"
```
