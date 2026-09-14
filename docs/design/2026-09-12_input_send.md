# コントローラ取得からUDP送信要求までの短縮

対象はMBAACC Ver.1.07 Rev.1.4.0、32bit、通信版10・拡張5。入力時計の周期・フレーム番号・中央バッファ、D/R、F4遮断と再送履歴は維持する。

## 変更

- `NetplaySession` は新しい自入力を受信キュー一括解析より先に送る。解析後も再確認し、相手のACKと新入力を取りこぼさない。
- 最新入力窓を先に、未消費先頭の補修窓を後に送る。従来は `BuildPacket` の既定引数が補修窓を選び、送信順が逆だった。
- パケットは通信スレッド専有の予約済みvectorへ組み立てる。全ヘッダとpayloadを毎回埋め、予約領域や前パケットの入力を残さない。ゲーム由来の共有状態は従来のmutex／atomic経由で読む。
- 宛先を開始時に解決して保持。通常の対戦UDPは通信スレッドから非ブロッキングのOS `sendto` へ渡す。毎回の送信ラムダ生成・IP文字列解析・vector複製・shared_ptr確保・ASIOへのpostを外す。
- 受信と非同期の汎用送信はASIOを維持。直接送信はASIOの共有socket内部を触らず、不変のnative handleと宛先だけを使う。設定は送信開始前、破棄は送信停止後。送信側の遅延・損失注入が有効なときは従来経路へ戻す。
- OS送信失敗を成功扱いにせず、次の採取・ACK・定期処理で再送する。バッファ不足時にスピンや待機を追加しない。

非ブロッキングはネットワーク側の空き待ちを抑止するが、OS呼出しそのものの実行時間を一定にはできない。成功も相手への到着保証ではない。[Microsoft sendto仕様](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-sendto)、[非ブロッキングI/O](https://learn.microsoft.com/en-us/windows/win32/winsock/nonblocking-i-o-2)。

ゲームの派生入力状態へ直接書いたり、命令を追加パッチしたりする必要はない。今回の対象はパケットまでの経路であり、入力適用は既存GameMem経由。実際にコンパイルされた32bit命令列は `build_logs/input_send_20260912/udp_disassembly.txt` に保管する。

## 計測方法

`CCCASTER_INPUT_SEND_TRACE=1` で実QPCを1/60µs単位で採取する。`InputCaptureSend` は取得開始・取得完了・中央バッファ公開直前、`InputSend` は構築開始・準備完了・OS要求開始・完了の時刻。採取側は送信通知の後、送信側はOS呼出しの後にログを整形する。F番号で最初の成功送信と結合し、欠落・異常な時系列・送信失敗を別に数える。

`CCCASTER_INPUT_SEND_BASELINE=1` は比較専用で、旧受信解析優先・旧送信順・使い捨てpacket・ASIO post経路を選ぶ。宛先ラムダの毎回生成は比較側でも省かれており、変更前全体の完全再現ではない。ASIO側completeは非同期完了コールバック時刻、直接側completeはOSからの戻り時刻なので、両者のcomplete同士は性能比較に使わない。共通の「OS要求開始」を主比較点とする。

コマンド:

```powershell
$env:CCCASTER_INPUT_SEND_TRACE='1'
# 比較側だけ: $env:CCCASTER_INPUT_SEND_BASELINE='1'
& build/virtual-pad-venv/Scripts/python.exe -X utf8 src/harness/run_virtual_controller_pair.py --seconds 75 --port 17920 --scenario 0 --idle-select 0 --network '60,96,5'
python -X utf8 src/harness/analyze_input_send.py <出力フォルダー>
```

診断ログあり・同一PC2窓・仮想DS4のDirectInput取得で測る。物理スイッチからの遅延、USBの報告待ち、NIC送出時刻、別PC到着時刻の測定ではない。取得周期による最大約1Fの待ちは今回の区間外であり、周期や平均FPSをμ秒の実測値には代用しない。

## 検証結果

同じバイナリで各75秒、仮想DS4、受信60〜96ms・損失5%、双方ONCE再戦。取得開始→OS要求開始の実測µs:

| 方式 | 側 | 件数 | 中央値 | p99 | 最大 |
|---|---|---:|---:|---:|---:|
| 比較用の従来経路 | ホスト | 1578 | 50.5 | 81.338 | 306.6 |
| 比較用の従来経路 | クライアント | 1579 | 57.7 | 93.488 | 137.3 |
| 直接送信 | ホスト | 1532 | 20.9 | 36.945 | 208.2 |
| 直接送信 | クライアント | 1533 | 20.1 | 33.336 | 240.9 |

中央値は29.6µs（58.6%）／37.6µs（65.2%）短縮。取得完了→OS要求開始だけなら42.15→11.3µs／49.3→10.7µs。構築区間は7.9→0.8µs／10.8→0.8µs、送信準備後→OS要求は22.05→0.1µs／27.2→0.1µs。区間中央値の和を総時間の中央値として扱わない。

全件結合、診断欠落・時系列異常・送信失敗0。新方式の最大値には入力公開→通信スレッド開始187.3／227.5µsの待ちがある。クライアントの最大は旧試行より悪化しており、最悪値改善や全入力のµs上限を保証しない。新方式のOS要求→戻りも最大150.6／137.5µs。USB／OSスケジューラ／ドライバの中断を命令削減だけで消したという結果ではない。受信遅延は疑似ネットワーク条件であり、片道のµs到達時間としては測っていない。

両試行とも26入力項目・左右分離・双方ONCE再戦を確認。確定入力・代表状態・保存対象の比較は旧1573F／新1526Fで差分・欠落・失敗0。保存表がゲームの全状態を網羅することの証明ではない。INIは不変。

- 旧: `build_logs/virtual_pad_20260912_211743/`
- 新: `build_logs/virtual_pad_20260912_211931/`
- 各 `input_send_summary.json`、`comparison_utf8.json`、`virtual_controller_result.json`。比較集約は `build_logs/input_send_20260912/comparison.json`。

着手時ソース・DLLは `build_logs/input_send_20260912/before/`、初期のビルド不備を修正後の32bitビルドは `build_final.log`、32 CTest成功は `ctest.log`。バッファ再利用のwire一致・UDP受信内容／順序／コピー寿命・不正宛先・巨大パケット失敗・従来送信・遅延／損失注入を検査した。解析器の最初の成功送信選択・欠落区別・異常時系列／診断欠落検出の2試験も成功。改行を規約のLFへ統一後のビルドは `build_normalized.log`。

追加計測をOFFにした最終40秒の自動入力・60〜96ms/5%試験も1283確定Fの入力・代表状態・保存対象が一致し、差分・欠落・失敗0。`build_logs/input_send_20260912/normal/comparison.json`。この試験は仮想DS4ではなくScriptedInputであり、上記DirectInput計測とは別。両テストコピーを最終成果物へ更新した。設定・ゲームEXEのSHA256は `preserved_before.json` と最終 `verification.json` で照合する。

最終照合: 98 INI・8ゲームEXE不変、両側WASAPI active、追加計測・送信失敗ログ0。EXE/DLLともPE Machine 0x014cで、最終再ビルドの`.text`は上記時間計測に使ったバイナリと完全一致した。試験で起動したゲームは終了済み。
