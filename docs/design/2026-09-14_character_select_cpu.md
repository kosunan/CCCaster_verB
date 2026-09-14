# Trainingキャラセレ待機の画像読込方式とCPU負荷の比較

## 比較の対象

利用者から「対戦時は2%未満、キャラセレでは6%以上。起動高速化の無圧縮画像が原因ではないか」と報告を受領。依頼どおりTrainingのキャラクターセレクト待機だけを測定し、戦闘へは進めない。

現行 `StartupAssets.cpp` の `DirectDds` はDXT1～5形式の圧縮DDSを受け付け、同じ圧縮形式のMANAGEDテクスチャへブロック転送する。無圧縮テクスチャへ置き換える実装ではない。標準APIが行う復号・再圧縮を省く方式であり、画像素材ファイルも変更しない。

- fast: 現行の直接転送。ログで296枚の直接転送を確認。
- baseline: `CCCASTER_STARTUP_ASSETS_BASELINE=1` のみを指定し、画像フックを無効化。ゲーム本来のD3DX画像読込を使用。他の起動高速化・入力時計・描画・待機処理は同じ。
- `CCCASTER_STARTUP_BASELINE`（高速化全体の無効化）や `CCCASTER_STARTUP_FIRST_BASELINE`（旧画像キャッシュ）は使用しない。

## 測定方法

同一PC、Ryzen 7 5800X3D（8コア16論理CPU）、GeForce RTX 5070 Ti。`test/runtime/MBAACC_1` の既存テストコピーと `cccaster_B` 内のGUI worker／DLLを使用。DLLは `build/bin` とSHA-256一致。製品ソース・ビルド成果物・画像・設定は変更しない。

1窓ずつ fast→baseline→baseline→fast→fast→baseline の順に各3回起動。Trainingモード、入力受付、Present到達をログ照合し、既存の2秒確認に加えて10秒安定化。以後1秒待機ごとに30区間のCPU時間を採取する。採取処理の時間も含む実経過時間で正規化するため、各試行の区間は約31.5秒。

CPU率は `(user CPU秒 + kernel CPU秒) / 実経過秒 / 16 × 100`。全論理CPUを100%とするプロセスCPU時間率であり、瞬間表示やクロック補正を含むタスクマネージャー表示との厳密一致は要求しない。読取り専用のゲームモード／world timerとスレッド別CPU時間も採取。両方式の画面をComputer Useで確認し、PNGを保存する。

根拠ディレクトリ: `test/logs/charselect_cpu_20260914/`。各試行に `result.json`、`idle_cpu.json`、ゲーム／workerログ、使用バイナリのSHA-256を保存。`summary.json` と `summarize.py` に集計と検証を保存する。

## 結果

| 方式 | 1回目 | 2回目 | 3回目 | 中央値 | 平均 |
|---|---:|---:|---:|---:|---:|
| 圧縮DDS直接転送 | 6.732% | 6.740% | 6.736% | 6.736% | 6.736% |
| ゲーム本来の画像読込 | 6.679% | 6.724% | 6.807% | 6.724% | 6.737% |

中央値差は0.012ポイント。画像読込を戻しても6%以上の負荷が残り、今回のキャラセレ高CPU負荷の主因としてDXT直接転送は支持されない。全6試行で約59.88～60.02Hz、全サンプルがキャラセレ。バイナリは全試行同一、対象13 INIは不変（正確な件数は `ini_check.json`）。ゲーム／workerは全て測定器が終了済み。

いずれも1本のスレッドだけでプロセスCPU率の約6.24ポイント（16論理CPU上のほぼ1論理CPU分）を消費。関数への帰属はこの測定では行っていない。ホットプラグ有効／無効の追加比較も実施し、停止しても6%以上の負荷が残った（`test/logs/hotplug_cpu_20260914/`）。その後、利用者から完全標準のゲームでも5%台になるとの確認とクローズ指示を受領。CPU課題をクローズし、原因の追加追跡と待機処理の変更は行わない。ホットプラグ停止用の診断スイッチは製品ソースから撤去した。

測定器に新配置の `--caster-dir` とGUI workerフォールバック、`--idle-cpu-seconds` を追加。構文チェックと6回の実行成功。製品コード変更がないため、この画像比較のためだけの再ビルド／対戦試験は行っていない。

## 再現手順

```powershell
python -X utf8 src/src/harness/bench_startup.py --root test/runtime --caster-dir cccaster_B --mode training --variant fast --comparison assets --idle-cpu-seconds 30 --label fast_new --out test/logs/charselect_cpu_new_fast
```

標準側は `--variant baseline` と新しい出力先を指定する。測定器は同じコピーの起動中には拒否し、終了時は今回のworkerとそのゲームだけを終了する。`--idle-cpu-seconds` 使用時はpsutilが必要。

## 確認範囲

キャラセレの同一初期選択・ウィンドウ表示・同一PCの比較。対戦時2%未満という報告との同条件比較、全キャラ・画面条件・別GPU、CPUを消費する関数／待機ループの特定は今回の対象外。高負荷の原因修正は行っていない。
