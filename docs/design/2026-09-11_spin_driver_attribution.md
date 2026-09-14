# スピン監視空白をGPU系ISR/DPCへ帰属する

## 結論

新しく同時採取した実ゲームとETWで、監視中の大きな空白を **Windows描画カーネルdxgkrnl.sysのISRとNVIDIA nvlddmkm.sysのDPC** へ帰属できた。対象ゲームスレッドと同じCPU0上で処理され、実行を中断している。単に別CPUのドライバ負荷が同時刻に高かったという相関ではない。

以前の110.7µsの回はETWを同時採取しておらず、その過去の回のドライバを遡って確定はできない。今回の結論は、再現した同種の監視空白に対する直接証拠である。

## 直接証拠

実対戦 `build_logs/bounded_real_20260911_063519/` のホスト131982F、PID18840/TID25812。
SpinOSの区間は60MHz ticksで `[3689184763392, 3689184785652)`、実QPCは10MHzなので `[614864127232, 614864130942)`。長さ371.0µs。

| CPU0上の処理 | 重なる経過 |
|---|---:|
| dxgkrnl.sysのISR 2本 | 98.6 + 5.5 = 104.1µs |
| nvlddmkm.sysのDPC 2本 | 124.9 + 137.7 = 262.6µs |
| 和集合 | **366.7µs（空白の98.84%）** |
| その他の区間 | 4.3µs |

前のCSwitchは614864109683で対象スレッドへ入り、次の614864144795で対象スレッドから出る。途中の別スレッドへの切替は0。OSスケジューラがゲームを他スレッドへ切り替えた時間ではなく、対象CPU上のISR/DPCが占有した時間として確認した。

対象TIDの生存期間は614480563410〜614914646082で、PID18840のStart/Stop両端から確定。記録開始直後に同番号を使っていた別プロセスとは区別した。

独立した最小CLIで元ETLを再読し、イベントpayloadの開始QPCとroutineを直接取り出してJSONLの4イベントとCSwitchに一致することを確認した。根拠は `build_logs/tls_independent/raw_etl_131982.txt`。

moduleのロード範囲とも一致:

- dxgkrnl.sys: base `0xfffff8050e8b0000`, size `0x50d000`, ISR offset `0xbca0`
- nvlddmkm.sys: base `0xfffff8051e5c0000`, size `0x7375000`, DPC offset `0xdf4490` / `0xea7f0`

この最大空白の終了遅れは0.117µsで、途中の空白は締切前の余裕に吸収されている。最大の監視空白をそのまま更新遅れとは呼ばない。

## 複数フレームでの確認

maxGapが100µs以上の25件はすべてスケジューラ境界を確認でき、GPU系ISR/DPCが同じCPU0で重なっていた。うち22件は空白の90%以上を割込みの和集合で説明できる。代表例:

| 対象 | 空白 | ISR/DPC重複 | 別スレッドへの切替 |
|---|---:|---:|---:|
| host 131982 | 371.0 | 366.7 | 0.0 |
| host 132027 | 255.7 | 251.8 | 0.0 |
| client 132170 | 253.8 | 249.7 | 0.0 |
| host 132137 | 253.2 | 248.6 | 0.0 |

単位µs。全1931解析区間のうち1768は前後のスケジューラ境界を確認、163はcoverage不足でunknownのまま残した。全区間へGPU原因を一般化しない。

host132247の181.9µs空白にはGPU系167.3µsに加えportcls.sys/Creative ctoss2k.sysも重なる。driver別の経過はISRの入れ子等で重複する場合があり、合計は和集合177.8µsで扱う。小さい残余までNVIDIAへ一括帰属しない。

## 採取条件・検証

- 同一PC2窓、自動入力45秒、Network `15,25,5`、D2/R4、SpinProbe有効。
- ゲームは通常権限。管理者ヘルパーだけがWPRを開始・終了。独自instance名を使い、150秒上限または停止要求で自分の記録だけを保存する。
- 記録: `build_logs/spin_etw_20260911_063515/scheduler.etl`、抽出 `events.jsonl`、WPR状態 `wpr_status.txt`、機器版 `devices.json`。
- ETW raw QPC、10MHz、イベント/バッファ損失0。CSwitch 2378859件、ISR115459件、DPC212715件。
- GPUはNVIDIA GeForce RTX5070 Ti、nvlddmkm.sys `32.0.16.1088`、dxgkrnl.sys `10.0.26100.9444`。Creative OS Services Driverは `6.0.240.0026-2.40.0000`。
- 32bitビルド・18 C++テスト・12 Python突合テスト成功。1447確定FでREC/FRAME/STATE/MEM一致、different/missing/failuresなし。
- 通常戦闘の集計は906/908F。ホスト131866Fに待機準備時点ですでに2070µs以上遅れた回があり、これは今回特定したスピン内部の空白とは別問題として残す。全更新スパイク解消ではない。

## 実装した採取・解析

- `SpinOS`: PID/TIDと最大空白・最大採取後遅延の絶対QPC区間。識別子は締切前に取得し、毎ループのCPU照会は追加しない。
- `spin_scheduler.wprp`: ProcessThread/Loader/CSwitch/ReadyThread/DPC/Interruptだけを採取。サンプリング周期やドライバ設定を変更しない。
- `run_spin_etw.ps1` / `record_spin_etw_admin.ps1`: ゲーム通常権限＋採取専用の管理者ヘルパー。最初のWindows PowerShell5での文字コード問題は修正し、PowerShell7で実採取成功。
- `etw_extract`: 既存Visual Studio InstallerのTraceEvent2.0.77を使用。NuGet追加・ツールインストールなし。raw QPC、ISR/DPCのpayload開始時刻、module情報をJSONLへ保存。
- `analyze_spin_etw.py`: 対象PID/TIDの生存期間とCSwitchからCPU実行区間を復元。同じCPUの割込みだけを半開区間で重ね、入れ子は和集合。欠落・不完全採取・境界不明はunknown。

再現:

```powershell
./src/harness/run_spin_etw.ps1 -Seconds 45 -Port 17860 -Network '15,25,5'
dotnet build_logs/etw_extract_bin/etw_extract.dll <scheduler.etl> <events.jsonl>
python -X utf8 src/harness/analyze_spin_etw.py <対戦ログディレクトリ> <events.jsonl>
```

採取にはWindows UAC承認が必要。通常権限のWPRは0x80070005で拒否された。既存WPR・既存GUI/CLI待受プロセスは終了していない。記録はローカルだけに保存した。

## 競技用途での判断

対策候補はゲームスレッドとGPU割込みがCPU0で重なる配置である。スピンを長くしても、実行CPU上のISR/DPCによる中断を防げない。まず一時的なスレッド配置の対照試験で、締切直前の空白・入力採取・同期をまとめて評価するのが次段階。配置変更・IRQ移動・ドライバ変更は今回行っていない。

この証拠はドライバ処理の異常やバグ、特定のGPU書込み命令との因果までは示さない。ゲーム自身か同時稼働アプリか、どのGPU仕事が割込みを誘発したかも未分離。ReadyThreadはETLには含めたが、現抽出器はReadyThreadの詳細を出していないので、ready待ちの内訳まで解析済みとはしない。

一次資料: [WPRコマンド・instance名](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/wpr-command-line-options)、[SystemProviderキーワード](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/keyword--in-systemprovider-)。実測の帰属根拠は上記の元ETLと絶対QPC。
