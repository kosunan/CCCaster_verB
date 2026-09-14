# メトロノーム専用スレッド（2026-09-11）

## 構成

NetplaySessionが入力時計スレッドを1本追加で所有する。WASAPI時計を基準にInputTimelineの絶対締切まで待ち、入力採取・中央バッファ公開を行う。通信解析、パケット生成、スナップショット、ゲームの書込みは担当しない。ゲームスレッドは従来どおり採取予定時刻+1msへ実更新開始を合わせる。処理時間に16.667msを追加する方式ではない。

通信スレッドは受信・入力公開の通知で起床し、定期締切も維持する。従来の1msポーリングを除去。新たなフレームキューや毎フレームのスレッド生成はない。相手時計の補正は引き続きInputTimelineが採取処理時に適用する。パケット到着直後の即時補正ではなく、次の採取処理で評価する。

ThreadSignalは単一消費者向け。イベント・高分解能待機タイマーを起動時に作って再利用する。通知世代番号を待機前に取得し、待機直前の通知も保持する。開始・再開・停止で時計スレッドを起こす。停止は両スレッドへ通知してjoinし、最後にMetronomeを止める。時計とmutexの静的寿命もSessionより長く保つ。

入力時計の待機は締切前500µsまでタイマー、残りだけ時計を確認する。ロード待ちは無期限の通知待機。高分解能タイマー非対応時は通常タイマー、作成失敗時はcondition_variableへ退避するが、同等精度は保証しない。WASAPI時計のmutexやコントローラAPIの競合、OSによる中断は残る。

## 最終測定

実験Worktree `I:/work_space/CCCaster_v10_experiments` の `build_logs/bounded_real_20260911_030107/game_1.log` と `game_2.log` が今回の根拠。before_*.logは前の実行なので集計から除外する。

同一PC2窓、40秒、自動入力、遅延15〜25ms・損失5%、D2/R4、TimeScale1、CCCASTER_FRAME_TIMING_TRACE=1。両側WASAPI active、高分解能待機有効、QPC切替記録なし。

| 計測 | ホスト | クライアント |
|---|---:|---:|
| 採取締切からの遅れ：120件平均の記録範囲 | 1〜48µs | 55〜146µs |
| 同遅れの記録最大 | 1792µs | 573µs |
| 実更新開始間隔：全記録範囲 | 676〜41966µs | 664〜44420µs |
| 入力スレッドCPU：活動中約2秒窓、論理CPU1個比 | 0〜4.69% | 0.78〜2.33% |
| Pump経過時間の記録最大 | 335µs | 157µs |
| ロールバック回数／最大深度 | 268／3 | 259／3 |

採取遅れはPump入口の時計と予定締切の比較であり、物理USB入力の到着時刻ではない。診断値は整数µs。CPU時間はWindowsの会計粒度が粗く、0はCPU負荷ゼロを意味しない。iterationsはループ回数であり、カーネル起床回数ではない。入力・ゲーム両方の待機費用を含むプロセス全体のCPU改善は未立証。

確認済み1174FのREC/FRAME/STATE/MEM差分・欠落0（絶対WTとメニューカウンターは従来どおり除外）。15単体テスト成功。`build_logs/dedicated_clock_shutdown` の短いharnessでは2622F一致、両側終了コード0で正常停止を確認。手操作・別PC長時間試験の代用にはならない。

通常のcondition_variableによる時刻待機も試したが、実ゲームで平均約9〜10ms遅れたため不採用。1msスピン版から500µs版へ短縮して負荷を抑えたが、実行間で条件が完全一致する比較ではない。安定区間だけを根拠に全フレーム一定と結論しない。

## コールバックについて

WASAPIのEVENTCALLBACKは音声バッファ準備のイベント通知。通知周期は音声エンジン・機器の対応周期であり、ゲームの60Hzとは限らない。IAudioClient3で対応周期を照会できるが、任意の16.666…msを保証するものではない。

- [Microsoft: IAudioClient](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioclient)
- [Microsoft: IAudioClient3](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-iaudioclient3)
- [Microsoft: 高分解能待機タイマー](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createwaitabletimerexw)

このため、コールバック化だけで精度が上がるとは判断しない。現実装はWASAPIを基準時計、専用スレッド＋高分解能タイマーを起床手段とする。音声供給自体のイベント化は別候補だが未実装・未測定。瞬間的な誤差ゼロと、追いつき時も含む実ゲーム更新間隔の一定化は未達。
