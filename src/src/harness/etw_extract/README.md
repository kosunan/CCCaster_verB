# ETL抽出CLI

ETLを読み取り、CSwitch・DPC/ISR・スレッド・イメージ情報をJSONLへ保存する。採取や権限昇格は行わない。出力は新規ファイル限定。

このPCでは.NET SDK 8/9とVisual Studio Installer同梱TraceEvent 2.0.77.0を使用できる。WPTのxperf/wpaexporterは確認できなかった。NuGetパッケージ追加やインストールは不要。

```powershell
dotnet build src/harness/etw_extract/etw_extract.csproj -o build_logs/etw_extract_bin --ignore-failed-sources
dotnet build_logs/etw_extract_bin/etw_extract.dll input.etl output.jsonl
```

別環境では `-p:TraceEventDir=...` で既存TraceEvent DLLと依存DLLのディレクトリを指定する。現既定パスは `C:/Program Files (x86)/Microsoft Visual Studio/Installer/Feedback`。ビルド成果物・依存DLLは配布物へ含めない。

## 記録

- `meta`: 最初と末尾。末尾の `final:true`, `complete:true`, `clock:"qpc"` を確認して解析する。`qpc_hz`, `start_qpc`, `end_qpc`, `lost_events`, `lost_buffers` と件数も記録。初期metaは解析完了の根拠にしない。
- `header`: ETLのclock type・QPC周波数・損失。clock type 1以外や未確認は異常終了する。SystemTimeをQPCとして結合しない。
- `cswitch`: 絶対 `qpc`, `cpu`, `old_tid`, `new_tid`, `old_pid`, `new_pid`, `wait_reason`, `state`, 優先度。
- `dpc` / `isr`: 絶対整数 `begin_qpc`, `end_qpc`, `cpu`, `routine`, `routine_hex`, `event`。ISRはvectorも記録。
- `thread`: `qpc`, `pid`, `tid`, `event`（Start/Stop/DCStart/DCStop）, 名前。
- `image`: `qpc`, `pid`, `base`, `size`, `path`, `event`（Load/Unload/DCStart/DCStop）。module帰属はアドレス範囲と生存期間を解析側で照合する。

DPC/ISR開始はTraceEventのprivate `InitialTimeQPC` getterから取得する。公式parserではpayload先頭Int64であり、`ElapsedTimeMSec`からの逆算と丸めは使わない。TraceEvent版変更でgetterが失われた場合は失敗させる。`TimeStampQPC` の非推奨警告は整数絶対時間の突合に必要なため、このCLIでのみ意図して使用する。

一次ソース:
- https://github.com/microsoft/perfview/blob/main/src/TraceEvent/Parsers/KernelTraceEventParser.cs （DPCTraceData / ISRTraceData）
- https://github.com/microsoft/perfview/blob/main/src/TraceEvent/ETWTraceEventSource.cs （ReservedFlags 1=QPC, 2=SystemTime）

## 検証範囲

2026-09-11: コンパイル・help成功。既存Windows CBS ETLを読取り、header処理・SystemTime拒否・失敗meta保存を確認。ゲームのDPC/ISR/CSwitch実採取ログによる抽出成功は未確認。

追補: `build_logs/spin_etw_20260911_063515/scheduler.etl` のゲーム実機ETL抽出を確認。host f131982のISR/DPC区間は、別の最小CLIからETLのpayload先頭Int64とroutineアドレスを直接読み直し、JSONLと一致。イベント損失0。独立検証結果は `build_logs/tls_independent/raw_etl_131982.txt`。
