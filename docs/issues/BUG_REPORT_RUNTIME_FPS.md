# beta_1.1でゲーム中に約3000 FPSとなる報告

2026-09-13。コミュニティEXEでの起動は復旧したとの報告。その後、ゲーム中にFPSが約3000まで上がり、FPS制限のためSpecial Kを使用しているとの申告があった。起動中だけの現象ではない。FPS表示元、実ゲームの動作速度、Special Kを導入する前後の条件は未確認。

## コードで確認した経路

`SceneRunner::Step()` は起動完了後のオフライン通常更新で `Metronome::WaitForNextTick()` を呼び、入力準備後に同じ絶対締切をCS解放側へ引き継ぐ。描画・更新に要した時間もこの周期に含む。

ゲーム自身の時計加速とSleep省略は、通常更新でも独自の待機へ置き換えるため維持する設計。`SetModeNormalSpeed()` は描画省略等を解除するが、ゲーム自身の時計を等倍へ戻す処理ではない。この設定が残っていることだけで原因と断定しない。

`WasapiClock` は音声時計を利用できなければQPCへ切り替え、Metronomeによる待機を継続する。WASAPIの初期化失敗だけで無制限更新になる設計ではない。報告者環境で実際にどの経路を通ったかは、ログ未取得のため未確定。

## ローカルで確認した範囲

- 指定EXEのSHA-256は `6d1415ca9573100e86a779ac2f81e9bedd322664e3daeae0229a67d13720310a`。
- 配布済みbeta_1.1と同じDLLを使用。SHA-256は `703e9c3d2d0476d8735ca1d4fc4532efd99f06f4262f4627f31bf16b36185fb4`。
- 独立コピー `CommunityRegression/MBAACC_1` で、仮想DS4のDirectInput経路からTrainingのキャラ選択・戦闘・攻撃まで実行。Special Kを起動・導入する操作は行っていない。
- ゲーム外のPythonプロセスからWorldTimerを読取り、外部のQPC経過と比較。10.014695秒で601更新、平均60.0118126Hz。これは測定期間全体の平均で、瞬間フレーム精度の値ではない。
- 同じWT区間の連続した601組の待機準備時刻は、間隔16,666.1〜16,669.5µs。最終CS解放時刻や物理画面の表示時刻を直接測った値ではない。
- ログではWASAPI active、Metronome開始、実デバイスのPresentフック設置を確認。描画要求の移動平均も約60 FPSとなり、約3000 FPSの状態は再現していない。
- 既存INIは前後で不変。試験で起動した子プロセスだけを終了した。コード・配布物・公開リリースは変更していない。

根拠: `build_logs/fps_report_20260913/training_20260913_060537/` の `result.json`、`timing_summary.json`、`game.log`、`launcher.log`。再実行用脚本は `build_logs/fps_report_20260913/training_probe.py`。

PowerShellのModules一覧はWOW64側のモジュールまでしか取得できておらず、第三者DLLが存在しないことの証拠には使わない。

## 次に必要な情報

報告者側の `cccaster_B/cccaster_hook_log.txt` の `[Clock]`・`[Metronome]`・`[DxHook]` 行を依頼済み。可能ならログ全体と、FPSの表示元、ゲーム動作自体の加速有無、Special Kを使わない状態での挙動を確認する。原因・修正・報告者環境での復旧は未達。
