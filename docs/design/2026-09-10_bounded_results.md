# 期限付き入力待機方式：実装・検証結果（2026-09-10）

入力が揃うまでゲーム進行を待機し、通常3秒・接続や画面合流30秒で打ち切る方式を実装した。通信スレッドは待機中も再送を続ける。サブエージェントは使用していない。

## 確認結果

- 自動テスト8件すべて成功。入力番号の欠番・二重消費・古いパケット・異なる通信版・IPC分離・期限切れを検査。
- UDPの2プロセス試験：ロード60/180フレームの差を与え、2,701フレームの入力と相対進行が一致。遅延100〜180ms、理論総欠落率36%の条件でも成功。
- 実ゲーム2窓：140秒、送信と受信それぞれ25〜45msの遅延と10%の欠落（片道合計50〜90ms、理論総欠落率19%）。対戦終了から再戦まで到達。
- 共通7,171フレームで入力、相対進行、体力、位置、ラウンド数、勝数、乱数状態232Bのハッシュが一致。途中の欠番・重複なし。
- 停止時の末尾はホスト7,171／クライアント7,174。試験用の強制停止による末尾3フレームは比較対象外。
- 絶対WorldTimerはロード原点の差により6,935フレームで異なる。相対進行は一致。メニュー内部カウンターは18フレームで異なり、成功判定から除外している。全メモリの一致を主張する試験ではない。
- 相手のゲーム進行だけを止める試験では、3,001,992μsで期限切れを検出。その待機中のWorldTimerは1027→1027で、ゲームを進めていない。

実機の証拠：`build_logs/bounded_real_20260910_113627/` の `game_1.log`、`game_2.log`、`comparison.json`。期限切れの証拠：`build_logs/bounded_timeout_verified/result.json`。

## 現行仕様と未検証範囲

通信版は3。両側に同じDLLが必要。D+Rは0〜8フレームの固定入力バッファで、ロールバックの保存・復元・再計算は未接続。既定値はD=2、R=4の合計6フレーム。対戦中の変更は受け付けない。

確認環境は同一PC、MBAACC 1.07 Rev1.4.0、スクリプト入力。別PC・実回線・NAT越え・人が操作するコントローラー・長時間連続対戦は未検証。観戦とオフラインモードも今回の完了判定には含めない。

## 再実行

プロジェクトルートから、32bit MinGWが利用できる環境で実行する。

```powershell
cmake --build build -j8
ctest --test-dir build --output-on-failure
powershell -File src/harness/run_bounded_pair.ps1 -Scale 1 -Network '50,90,20' -Name gate_loss_repeat
powershell -File src/harness/run_bounded_pair.ps1 -Scale 1 -HostStall 350 -Name gate_timeout_repeat
powershell -File src/harness/run_bounded_real_pair.ps1 -Seconds 140 -Network '25,45,10'
python src/harness/compare_bounded_real_pair.py build_logs/bounded_real_日時
```

実ゲーム試験には `_TEST_MBAACC/MBAACC_1` と `MBAACC_2` に各自のゲームが必要。スクリプトはテスト先の旧DLL等をログフォルダーに退避し、今回起動したプロセスだけを終了する。比較器の終了コード0と `passed: true` を確認する。
