# ETL抽出CLI

ETLを読み取り、CSwitch・DPC/ISR・スレッド・イメージ・CPU標本・スタック情報をJSONLへ保存する。採取や権限昇格は行わない。出力は新規ファイル限定。

このPCでは.NET SDK 8/9とVisual Studio Installer同梱TraceEvent 2.0.77.0を使用できる。WPTのxperf/wpaexporterは確認できなかった。NuGetパッケージ追加やインストールは不要。

```powershell
dotnet build src/src/harness/etw_extract/etw_extract.csproj -o test/logs/etw_extract_bin --ignore-failed-sources
dotnet test/logs/etw_extract_bin/etw_extract.dll input.etl output.jsonl
```

別環境では `-p:TraceEventDir=...` で既存TraceEvent DLLと依存DLLのディレクトリを指定する。現既定パスは `C:/Program Files (x86)/Microsoft Visual Studio/Installer/Feedback`。ビルド成果物・依存DLLは配布物へ含めない。

## 記録

- `meta`: 最初と末尾。末尾の `final:true`, `complete:true`, `clock:"qpc"` を確認して解析する。`qpc_hz`, `start_qpc`, `end_qpc`, `lost_events`, `lost_buffers` と件数も記録。初期metaは解析完了の根拠にしない。
- `header`: ETLのclock type・QPC周波数・損失。clock type 1以外や未確認は異常終了する。SystemTimeをQPCとして結合しない。
- `cswitch`: 絶対 `qpc`, `cpu`, `old_tid`, `new_tid`, `old_pid`, `new_pid`, `wait_reason`, `state`, 優先度。
- `dpc` / `isr`: 絶対整数 `begin_qpc`, `end_qpc`, `cpu`, `routine`, `routine_hex`, `event`。ISRはvectorも記録。
- `thread`: `qpc`, `pid`, `tid`, `event`（Start/Stop/DCStart/DCStop）, 名前。
- `sample`: `qpc`, `pid`, `tid`, `cpu`, `ip`, `count`, `dpc`, `isr`。CPU標本の点であり関数全体の所要時間ではない。
- `stack`: 発行時の`qpc`と、対応するイベントの`event_qpc`を区別。`frames`は未シンボル化アドレス。戻り番地からの関数境界対応では呼出命令を逆アセンブリでも確認する。
- `image`: `qpc`, `pid`, `base`, `size`, `path`, `event`（Load/Unload/DCStart/DCStop）。module帰属はアドレス範囲と生存期間を解析側で照合する。

DPC/ISR開始はTraceEventのprivate `InitialTimeQPC` getterから取得する。公式parserではpayload先頭Int64であり、`ElapsedTimeMSec`からの逆算と丸めは使わない。TraceEvent版変更でgetterが失われた場合は失敗させる。`TimeStampQPC` の非推奨警告は整数絶対時間の突合に必要なため、このCLIでのみ意図して使用する。

一次ソース:
- https://github.com/microsoft/perfview/blob/main/src/TraceEvent/Parsers/KernelTraceEventParser.cs （DPCTraceData / ISRTraceData）
- https://github.com/microsoft/perfview/blob/main/src/TraceEvent/ETWTraceEventSource.cs （ReservedFlags 1=QPC, 2=SystemTime）

## 検証範囲

2026-10-06: 現配置の`run_spin_etw.ps1`で無遅延・損失なしのP2Pを採取。CLI/DLLの配置SHA-256、同期比較1000F以上、INI/ゲームEXE保全、通常権限でのゲーム起動を診断用Pythonラッパーで確認する。採取ヘルパーだけUACを使い、ユニークなWPR instanceだけを停止する。

```powershell
pwsh -NoProfile -File src/src/harness/run_spin_etw.ps1 -Seconds 75
# サンプリング割込み・スタック採取の影響を除く対照条件
pwsh -NoProfile -File src/src/harness/run_spin_etw.ps1 -Seconds 75 -SchedulerOnly
python -X utf8 src/src/harness/analyze_deadline_diagnostics.py <出力先/pair> --etw <抽出JSONL>
python -X utf8 src/src/harness/analyze_spin_probe.py <出力先/pair> --all-play
```

CPU標本・CSwitchスタック付きの実採取ではイベント/バッファ損失0、ゲーム内QPCとの結合、DPCと最終解放遅れの重なりを確認。一方、サンプリング自体が数µsの遅れに重なったため、採取中の最大値を診断なしの性能と扱わない。`--all-play`はSpinProbeのplay条件（保存・巻戻し可能）を使う。厳密なイントロ除外・連続通常更新は`analyze_deadline_diagnostics.py`のUpdateCadenceで判定する。

2026-09-11: コンパイル・help成功。既存Windows CBS ETLを読取り、header処理・SystemTime拒否・失敗meta保存を確認。ゲームのDPC/ISR/CSwitch実採取ログによる抽出成功は未確認。

追補: `build_logs/spin_etw_20260911_063515/scheduler.etl` のゲーム実機ETL抽出を確認。host f131982のISR/DPC区間は、別の最小CLIからETLのpayload先頭Int64とroutineアドレスを直接読み直し、JSONLと一致。イベント損失0。独立検証結果は `build_logs/tls_independent/raw_etl_131982.txt`。
