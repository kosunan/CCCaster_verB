# 旧版の接続支援サーバーを通常対戦へ組み込む

## 利用方法と変更点

通常の募集／コード参加で自動的に利用する。募集側はコードを掲示板へ貼って相手の参加を待てる。募集全体を6秒で打ち切らない。参加側は直接UDP接続を優先し、公開IPv4の試行で1秒応答がなければ旧版サーバーによるホールパンチングを併用する。

独立した二人用の「6秒診断」画面は撤去した。接続確認は対戦接続に含め、GUIには相手待ち、参加者との接続確認、接続支援サーバーへの到達失敗、接続試行の終了を表示する。募集／参加ボタン・本文・状態見出しを拡大した既存の視認性改善は維持する。以前の `2026-09-13_punch_diagnostic.md` と `2026-09-13_gui_readability.md` に記載した専用診断画面は現行仕様ではない。

追加の有料サービス・アカウント登録は不要。接続支援サーバーはアドレス交換を行い、ゲームデータは双方のUDPで直接通信する。TURNやゲーム通信の中継サーバーは追加していない。

## 接続先

EXE横に `relay_list.txt` がなければ、旧版と同じ次の3か所へ登録する。

```text
melty.argoneus.com:3939
melty-backup.argoneus.com:3939
104.238.130.23:3939
```

変更する場合は `relay_list.txt` に1行1接続先の `hostname:port` を記載する。空行と `#` 始まりの行を無視し、最大8か所まで使用する。ファイルがある場合はその内容で既定リストを置き換える。無効化する場合はEXE横の `cccaster_v10.ini` の `[Netplay]` に `RelayEnabled=0` を追加する。未指定では有効。既存設定ファイルへ自動追記しない。

## プロトコルと待機

参照元はローカル旧3.1.007の `I:/work_space/CCCaster/OLD/lib/SmartSocket.cpp` と `scripts/server.py`。

1. 募集側はサーバーへTCP接続し、`U` と待受ポートの16bit little endianを送る。
2. 参加側は `U<募集側公開IPv4>:<待受ポート>` を送る。
3. 双方がTCPの `MatchInfo` と一致する32bit IDを受け取り、対戦交渉用UDPソケットから5byteの役割・IDをサーバーへ送る。
4. TCPの `TunInfo` から相手の外部UDPアドレス・ポートを受け取り、双方から既存v10の接続交渉パケットを送る。
5. 成立後は既存の相手nonce、相手アドレス、自ポートをDLLへ渡す。DLLは同じ自ポートを再利用する。ゲーム通信版10・既存の同期処理は変更しない。

複数サーバーへの募集登録を並行して保持する。TCPのkeepaliveを30秒、再送間隔を10秒に設定し、切断検出後30秒で再登録する。OSによる切断検出時間は別に掛かる。サーバー接続準備は各3秒、個々の相手情報交換は8秒、相手情報受領後のUDP試行は8秒。TCPの分割受信・複数通知を処理し、不正ID・不正アドレス・肥大化した応答を拒否する。

無応答のコード参加はLAN IPv4 1秒、公開IPv4 12秒、IPv6 3秒の順に打ち切る。応答を受けた後の既存起動交渉は最大30秒。募集側の交渉失敗は相手を解除して募集へ戻す。相手の来ない募集をこれらの短い期限で終了しない。DNS・OS・公開IP取得まで含めた厳密な総時間保証ではない。

既存の招待コード有効期限は作成から6時間であり、GUIにも明記する。6時間を超えて募集する場合は募集を開始し直して新しいコードを掲示する必要がある。公開IPの変化でもコード更新が必要。旧版ゲームとの対戦互換を追加するものではない。

## 確認結果

根拠フォルダーは `build_logs/legacy_relay_20260913/`。

- 32bit CLI・GUI・専用プローブのビルド成功（`build_final.log`）。
- ローカル実ソケットで、不通／不正サーバーからの切替、TCP分割応答、8秒で失敗した参加者の後の参加、直接接続、無応答参加の終了、キャンセルを確認。さらにサーバーからTCPを切断し、30秒後の同一ポート再登録から後着の参加に成功、開始から30,433msで交渉完了（`integration.json`、`integration_final.log` と各ログ）。
- 公開 `melty.argoneus.com:3939` と `104.238.130.23:3939` に対し、自分の2接続間で一致するMatchInfoと双方のTunInfoを実際に受信（`public_*.json`）。バックアップのTCPは確認時タイムアウト。稼働の継続保証ではない。
- ローカルの接続支援サーバーを経由し、直接経路を試験用に無効化して実ゲーム2窓を40秒実行。両側とも `hole_punch` 経路を通過（`real_pair_route.json`、`real_pair/launcher_*.log`）。人工回線15〜25ms・5%損失で、確定1333FのREC／FRAME／STATE／MEMが差分0・欠落0。ロールバック12／3回、最大深度2／1（`real_pair_compare.log`）。保存領域の完全性や全キャラの検証ではない。
- 関連CTest `startup_negotiation`・`udp_send`・`sync_codec` の3件成功（`ctest.log`）。実ゲーム試験前後の30 INIが不変（`ini_before.json`、`ini_after.json`）。

TCP keepalive設定と相手情報受領後の期限調整は実対戦の後に追加し、その最終差分はビルド・実ソケット試験で確認した。ゲームの同期・入力処理はその間変更していない。

GUIはComputer Useで通常募集を開始し、公開接続支援サーバーに接続した相手待ち表示を日本語／英語で確認した。最小ウィンドウでは以前の配置だと状態が下へ隠れたため、募集開始後の入力欄を畳む修正を追加。コード・コピー・キャンセル・状態説明をスクロールなしで確認した（`gui_host_ja.jpg`、`gui_final_build.log`）。短い待機制限と独立診断ボタンはなく、コピーと募集キャンセルも確認。終了済みの自分の募集コードへGUIから参加し、自動試行の終了、失敗理由と確認先の表示、操作への復帰を確認した（`gui_failed_ja.jpg`）。キャンセル完了後に「キャンセル中」が残る表示も修正。最後に、日本語の進行表示で字形不足により `?` になっていた三点リーダをASCIIの `...` へ置き換えてビルドした。ビルド成果物と通常テスト1・2のCLI／GUIはSHA256一致、PE Machine `0x014c`（`deployed.json`）。

## 残る確認

別PC・別回線・CGNAT・UDP遮断・対称NATなどの組合せでの実対戦、長時間待機後のルーター状態変化、公開サーバー切断からの実回線復帰は未確認。今回の公開サーバー試験は通知／アドレス交換の確認であり、異なる回線間のP2P対戦成功とは区別する。TCP観戦のNAT越えとIPv6経路の整備は対象外。接続支援の導入によって全員が必ず接続できるとは保証しない。

## 再実行

```powershell
cmake --build build --target CCCaster_v10 CCCaster_v10_GUI relay_negotiation_probe -j8
python -X utf8 src/harness/test_legacy_relay.py
python -X utf8 src/harness/test_legacy_relay.py --public melty.argoneus.com:3939
python -X utf8 src/harness/test_legacy_relay.py --real
python -X utf8 src/harness/compare_rollback_pair.py build_logs/legacy_relay_20260913/real_pair
```

公開の接続支援サーバーを検証するのは `--public` 指定時のみ。ローカル試験では `CCCASTER_RELAY_SERVERS` で接続先を上書きし、`CCCASTER_TEST_FORCE_RELAY=1` で候補アドレス通知前の直接接続を抑止する。通常起動では指定しない。
