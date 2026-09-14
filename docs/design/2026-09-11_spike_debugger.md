# スパイクを命令アドレスへたどるデバッグ診断

2026-09-11。MBAACC Ver1.07 Rev1.4.0、32bit、単独作業。

## 起動と解析

通常CLIの起動引数へ `--debug-spikes` を加える。例：

```powershell
.\CCCaster_v10.exe --headless --host --port 17860 --debug-spikes
```

環境変数 `CCCASTER_DEBUG_SPIKES=1` でも有効。通常起動は無効。現在はネット対戦の戦闘専用で、トレーニング・キャラセレ・起動直後の採取には使わない。子DLLのSpinProbeは自動的に有効化する。

実ゲーム2窓でログをまとめて採る場合：

```powershell
$env:CCCASTER_COMBAT_STRESS='2'
./src/harness/run_bounded_real_pair.ps1 -Seconds 45 -Port 17860 -Network '60,96,5' -DebugSpikes
# 実際に表示された出力フォルダーを指定
python -X utf8 src/harness/analyze_spike_debug.py build_logs/bounded_real_YYYYMMDD_HHMMSS --minimum-us 100
```

解析閾値はµs。10µs以上を見る場合は `--minimum-us 10`。解析結果は新規 `spike_debug_analysis/README.md`、詳細はreport.json。既存結果を上書きしないため再解析は`--output`で別の新規ディレクトリを指定する。objdumpの既定は `C:/msys64/mingw32/bin/objdump.exe`、別環境では `--objdump` で指定する。

## 採取の仕組み

- ランチャーが今回生成したPID/TIDだけを対象にする。既存ゲームや外部PIDには接続しない。
- DLLから戦闘世代360F経過後に名前付きイベントで通知。ランチャーの専用スレッドがDebugActiveProcessで接続する。
- 実QPC・周波数、対象PID/TID、EIP/ESP/EBPと汎用レジスタ・EFLAGS、EIPから64byteの実メモリ命令、ESPから128byteのスタック値を採取する。
- SuspendThread/GetThreadContext/ReadProcessMemoryで読取り、必ず追加した停止1回をResumeThreadで戻してからログ整形する。ゲームのレジスタ・命令を書き換えない。
- 採取前・コンテキスト取得後・再開後の3時刻と元の停止回数を保存。採取停止時間の上限を解析に明示する。
- モジュールのロード時刻、アンロード、実ロードベース、SizeOfImage、PE timestamp、UTF-8パスを記録し、ASLRとアドレス再利用を考慮してRVAへ変換する。
- 初回接続のbreakpointだけ処理し、その他の例外はDBG_EXCEPTION_NOT_HANDLEDでゲームへ渡す。デバッグイベントによる全スレッド停止と処理時刻も別途記録する。
- 最大20万サンプルで採取を停止。PID＋QPC名のTSVへ128件ごとにflushし、正常終了はENDを記録。強制終了・途中行は解析で不完全として扱う。
- デバッガ終了に伴うゲーム自動終了を無効化し、終了時にdetachする。起動・接続失敗は明示ログを残す。

Windowsデバッグイベントが全スレッドを止める仕様は[Microsoftのデバッガループ説明](https://learn.microsoft.com/en-us/windows/win32/debug/writing-the-debugger-s-main-loop)を参照。

## 逆アセンブル調査への導線

READMEのフレーム／区間→EIP→命令列リンクから調べる。report.jsonのmatched_samplesには、その瞬間のレジスタ、モジュール＋RVA、採取停止上限、スパイク区間の種類・フレーム番号がある。疑わしいアドレスのスタック候補もモジュール＋RVAへ変換する。

逆アセンブルは採取時EIPからの実メモリ命令を32bit Intel構文で生成する。フック適用前のディスク上命令と混同しない。最大40種類/プロセス、重複を除き長い区間を優先。命令バイトのSHA256も保存する。採取範囲外の関数全体・呼出元の調査は、記録した正しいモジュールとRVAから既存の逆アセンブル手順で行う。

スパイク区間は既存SpinOS/SpinTail/SpinReturn/ReplayWork等の固定長計測と実QPCで照合。再計算、復元、スピン監視、終了後処理を区別する。スタックの値は単なるアドレス候補で、検証済みの呼出スタックではない。

## 検証と制約

- 150125：CLI環境変数のCRTキャッシュ差で未接続。Win32環境読取りへ修正。採取成功の根拠には使わない。
- 150225：起動直後の接続で初期化待機となり同期タイムアウト。150433：キャラセレ到達後の接続でもロード後に戦闘まで到達しなかった。起動時のデバッガ接続は採用しない。
- 150611：戦闘360F後の接続で1559確定FのREC/FRAME/STATE/MEM一致、差分・欠落・失敗0。両側1302件採取、100µs以上の区間に各37件重複、28/32命令列を逆アセンブル成功、正常ENDあり。ただし採取間隔中央値15.328/15.329msだったため、WaitForDebugEventのtimeoutを採取待機に使わず高精度waitable timerへ変更。

デバッガ採取がスパイクを作るため、診断モードの時間・頻度を通常時の性能として扱わない。実採取周期はREADME/report.jsonのactual_sample_interval_usを使い、設定値2msで代用しない。2ms級の統計採取では10µs単発や全スパイクを必ず捕まえられない。原因の確定には、同じ候補を軽量API計測・ETW等で再確認する。

最適化・業務処理の変更は今回行わない。通信版10・拡張2を維持。全キャラ・別PC・長時間・機器切断は未確認。

## 最終実機結果

根拠：`build_logs/bounded_real_20260911_150802`。45秒・60〜96ms/5%・D2/R4・近距離連続ヒット。双方デバッグ接続後も進行し、1565確定FのREC/FRAME/STATE/MEM差分・欠落・失敗0。32bit全体ビルド・22 CTest・解析専用3テスト成功。

| 採取結果 | host | client |
|---|---:|---:|
| 総採取件数 | 6930 | 6921 |
| 100µs以上の区間に重複した採取 | 123 | 154 |
| 生成した命令列 | 40 | 40 |
| 実採取間隔中央値 µs | 3025.6 | 3024.9 |
| 実採取間隔最大 µs | 13880.9 | 27217.2 |
| 採取停止上限中央値 µs | 50.9 | 50.5 |
| 採取停止上限最大 µs | 954.8 | 1457.7 |

両側ENDあり。終了競合時のSuspendThread error=5を記録して採取終了しており、診断ファイルの終端があることと全API成功は区別する。初回接続のbreakpointは期待される記録。

実例：host frame131958の再計算区間1164.3µs内、msvcrt.dll RVA0x8DC4A（実アドレス0x76D3DC4A）から `rep movs DWORD PTR es:[edi],DWORD PTR ds:[esi]` を採取した。client frame131444でもRVA0x8DCECに同じコピー命令を確認。ゲームEXE/DLL側のスタック候補と組み合わせて保存・復元などの呼出元調査へ進める。ただし採取の重複や頻度だけでコピーをスパイク原因と断定しない。

*.asmはEIPからの直線的な解読であり、途中のジャンプテーブル・データも命令風に表示される。後続行すべてが実行経路とは限らない。完全な制御フロー復元・シンボル付きスタック巻戻しは未実装。

最終解析成果物は `build_logs/bounded_real_20260911_150802/spike_debug_analysis_final/README.md`。各窓のcontext_insideでコンテキスト採取時刻が区間内か、sample_overlap_usで採取停止との重複量を分けている。
