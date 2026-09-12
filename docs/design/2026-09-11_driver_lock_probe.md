# ドライバのロック検出と処理コスト削減の比較

処理コスト自体の解消は未達。提示余裕5msをさらに増やす変更は行っていない。追加したのは汎用ロック診断と、提示余裕に隠れた処理超過を落とす自動判定。効果を確認できなかった描画変更は製品コードから撤去した。

## 計測の範囲

`CCCASTER_DRIVER_LOCK_PROBE=1` のときだけ、対象プロセスの `RtlEnterCriticalSection`、`RtlAcquireSRWLockExclusive`、`RtlAcquireSRWLockShared` をMinHookで計測する。特定のD3D9版のRVAには依存しない。

- 実QPCで取得APIの入口・出口を測り、100µs以上を記録する。取得をtry-lockや別の待機方式へ置き換えない。
- スレッドID、ロックアドレス、呼出し元アドレス、CSの取得前所有者、進行中フレームを残す。SRWの所有者は不明として0。
- ロック内では文字列整形・ファイル出力・ヒープ確保・C++ `thread_local` を使わない。Win32 TLSの再入防止と固定4096件バッファへ数値だけ書き、Present入口で回収する。満杯なら待たず欠落数を記録し、解析を不合格にする。
- 呼出し元は設置時のDLL配置と照合。描画との帰属は同一スレッドかつ絶対時刻の包含を条件とし、別スレッドの同時刻イベントを同一描画の待ちと断定しない。

取得時間にはスケジューラによる中断や内部処理も含む。CPU実行時間や所有者の保持時間ではない。取得前のCS所有者も瞬間値であり、待機全期間の所有権を保証しない。フレーム番号は進行中ゲームの目印で、非ゲームスレッドの論理フレームを示すものではない。独自のスピンロック、GPU待ち、カーネル内部のロック、設置後に読み込まれたDLLの分類は未対応。診断ONの速度を通常時の性能として採用しない。

## 実機で増えた観測範囲

初回ログ: `build_logs/driver_cost_locks_20260911/driver_locks.json`。双方3フック設置成功、251／227件、欠落0。1538確定Fで入力・代表状態・保存対象メモリの比較一致。初回だけフレーム目印が直前Presentの値になっていたため、最終版ではBeginSimulationで設定するよう修正済み。絶対時刻と描画による照合には影響しない。

| 呼出し元 | 検出した相対アドレス | 取得時間の例（試験全体、起動中を含む） |
|---|---|---|
| D3D9 | 0x140935（既知のAcquireSynchronization） | 最大2.858／3.721ms |
| D3D9 | 0x48A2C、0x62888など | 最大2.366ms、1.180ms |
| NVIDIA nvd3dum | 0x1599764 | 最大128.8µs |
| NVIDIA nvd3dum | 0x144D953 | 最大377.7µs |
| NVIDIA nvd3dum | 0x144DB09 | 最大275.3µs |

逆アセンブルとDLL SHA256・PE timestampは `build_logs/driver_lock_disassembly_20260911/`。NVIDIAの3箇所ともIAT RVA `0x183B170` 経由のcall直後で、PEインポート表から `KERNEL32!EnterCriticalSection` と確認した。後二者は同じ静的ロック（イメージRVA `0x405FEAC`）を取得する。周辺命令だけからドライバ内の機能名や削除可能性は断定していない。逆アセンブルは採取した呼出し元に対応するディスク上DLLのもの。前回のデバッガによる実メモリ命令採取とは区別する。

## コスト削減の実験

すべて0↔300%、双方体力11400固定、タイマー4752へ復元、表示予算1.2ms、45秒の自動実対戦。最初の満タンは131732F。値はゲーム更新＋描画の実測で、GPU完了時間ではない。

| 比較 | 最初の満タン（両側） | 判断・根拠 |
|---|---|---|
| ハードウェア頂点処理 | 4.137／4.316ms | flags=68適用、改善なし。`driver_cost_hardware_20260911`、1598確定F一致 |
| 条件を絞った明示ピクセルシェーダー | 片側4.394ms、CS待ち約2.49ms残存 | 作成成功だけでは全描画の置換を証明しない。改善を確認できず撤去。`driver_cost_shader_20260911`、1536F一致 |
| 48個のDEFAULT/DYNAMIC画像へ転送 | 4.582／4.633ms | 通常処理中央値も約1.37msへ増加し不採用。対応は3サイズに限定、全資源を置換した試験ではない。`driver_cost_mirror_20260911`、1596F一致 |

シェーダー・画像転送の試作は `build_logs/driver_shader_experiment.hpp` と `build_logs/driver_texture_experiment.hpp` に記録し、本線から削除した。ドライバ全体の設定・レジストリ・配布DLLは変更していない。

## 自動判定

```powershell
# ゲーム不要、CS/SRWを実際に競合させて検出・再帰・LastError・満杯を確認
ctest --test-dir build -R driver_lock_probe --output-on-failure

# ゲージ実験、同期、体力・タイマー固定、設置、欠落、INI保全を一括検査
python -X utf8 src/harness/run_regression.py --profile real --case driver_locks

# 診断なしで処理時間自体に上限を課す。現状は未達として失敗する
python -X utf8 src/harness/run_regression.py --profile real --case gauge --gauge-max-work-us 1200
```

通常の提示判定とコスト判定は別。`analyze_gauge_stress.py --maximum-work-us` は切替前1F〜後2Fの更新＋描画の最大値を検査する。5ms予算で提示が間に合っても処理が1.2msを超えれば不合格。

最終検証は `build_logs/regression_20260911_183027_148038/summary.json`。23 CTest（ネイティブ競合試験を含む）・53 Pythonテスト・4倍速同期harnessに加え、ゲージとロック診断を実行。ゲージは1612確定F一致。診断OFFでも初回満タン5.037／1.624ms、提示誤差最大633／1.7µsを観測。通常の提示基準には合格したが、同じログへ1.2msのコスト基準を適用すると失敗終了1。結果は `gauge_0/root_cost_gate.json` に別保存し、根治未達を隠さない。

最終ロック診断は1530確定F一致、両側273／283件、欠落0、3フック設置成功。両ケースのINI保全・WASAPI使用を一括検査して合格。今回起動した検証プロセスは終了済み。最終DLL SHA256は `3de14c9e7237fbe4fa1451ce8d528d67cd6167044c2e1610a5f4b6626574d2d9`。
