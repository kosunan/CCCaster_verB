# プロセスとタスクバー表示はあるが画面が開かない報告

## 2026-09-14 続報：旧DLLのログと誤診の訂正

EXEを合わせてもTrainingが起動せず、相手のEXEはユーザー環境で動作するとの報告。前回の環境変数確認案内後も同じ結果との続報を受領した。受領ログを `build_logs/window_report_20260914/hook_initial.txt` と `hook_followup.txt` に保管。後者は前者6起動分をそのまま含み、通信対戦1回とTraining1回の計2起動が追加されている。最新Trainingはmode=1／host=1で、末尾はStartupAssets設置。追加の通信対戦ではUDP・相手READY・WASAPI activeまで進んでいるが、画面表示・キャラ選択成功の記録はない。

最新起動のDLL識別文言は `(multi-instance bypass applied)`。これは修正前ソース `build_logs/window_startup_20260913/source_before/dllmain.cpp:356` と一致し、修正後の `(game-only multi-instance bypass applied; Windows APIs unchanged)` とは異なる。**使用DLLは起動停止対策前の世代と判断できるが、報告者DLLの正確なハッシュは未取得。**

前回の「Initialization complete行がないので初期化が停止」「CCCASTER_STARTUP_PROFILEが原因」という断定を撤回する。修正前ソース335〜338行は計測初期化とSignalReady呼出しの後にそのままreturnし、その通常ログを実装していない。現在のソースのログを旧配布DLLにも期待した誤診だった。環境変数の実値・削除成否も受領しておらず、残存を推定する根拠はない。追加の恒久的な環境変数変更は案内しない。

旧多重起動対策にはWindows API本体を改変する既知不具合があり、下記の修正が候補。ただし今回の停止原因そのものと断定しない。次は、同対策とTraining P1修正を含む既存 `build_logs/training_p1_20260913/cccaster_B_training_P1_fix_EN_20260913.zip` の3バイナリをまとめて更新し、実際に修正後のDLL識別文言へ変わることを確認する。失敗が続けば最新フックログ・GUI詳細ログとDLLハッシュを採取する。英語の訂正版手順は `build_logs/window_report_20260914/INSTRUCTIONS_EN.txt`。

今回ZIPを再読し、SHA-256 `77c6b6684f14df0fbe1fb4ef6b7e126d67816f4632bd6a5a0641e2e6c3e246d7`、収録3バイナリのPE Machine 0x014c、READMEを含む4ファイルの収録ハッシュ一致、DLL内の修正後識別文言と初期化完了ログの存在を確認した。根拠は同証跡フォルダーの `verification.json`。既存検証済みZIPの再確認と記録のみで、新規コード修正・ビルド・実ゲーム試験・公開は実施していない。今回の報告者環境での復旧は未確認で、過去の別報告の復旧を流用しない。

## 2026-09-13 調査記録

2026-09-13。ゲーム中の高FPS報告より先に調査するよう依頼された。報告者のDLLログは `[StartupAssets] enabled=1 verify=0 direct=1` で終わり、プロセスは開始しタスクバーにも表示されるとの回答。ランチャーログは未取得。停止位置・報告者側の復旧は未確定。

## 確認した不具合と修正

従来の多重起動対策は `FindWindowA/W` と `CreateMutexA` のWindows API本体を書き換えていた。後者は実際のMutexを作らず `0x1337` を返す。このためゲームだけでなく、同一プロセスにある描画・音声・外部DLLからの呼出しにも影響する。

独立した実ゲームへ小さな観測DLLを読み込み、修正前後で同じAPI試験を実施した。観測用の非表示ウィンドウとMutexだけを生成・解放する。これはSpecial Kを導入した試験ではない。

| 観測 | 公開beta_1.1 | 今回の修正後 |
|---|---|---|
| CreateMutexAのハンドル有効性 | 無効、値4919（0x1337） | 有効 |
| 同じ名前で再作成した際の既存検出 | 失敗、エラー0 | 成功、ERROR_ALREADY_EXISTS=183 |
| 所有MutexのReleaseMutex | 実行できず | 成功 |
| 観測用ウィンドウのFindWindowA/W | 両方失敗 | 両方成功 |

`dllmain.cpp` の多重起動対策を、ゲームの `0x40D253` にある判定関数呼出しだけの置換へ変更した。引数なしのCALLを同じ5バイトの `mov eax,1` に置換し、続く元のtest/jneを維持する。対応する従来版・コミュニティ版の両ファイルで、呼出元15バイトと判定関数80バイトの一致を確認した。DLL読込時の全コード照合に加えてこの局所命令も照合し、失敗時は適用しない。Windows API本体とゲームファイルは書き換えない。

初期化完了・起動gate通知の成否、Direct3DCreate9の呼出し／戻り、CreateDeviceフック設置とHRESULTを通常ログへ追加した。起動時だけの記録であり、毎フレームの出力は追加していない。高速起動、起動時SHA-256検査、通信版10を維持する。

## 検証結果

- 32bitビルドと37 CTest成功。
- コミュニティ版Trainingでキャラ選択の描画要求・通常入力受付まで到達。API試験の失敗→成功を確認。物理画面の目視確認とは区別する。
- 配布する診断スクリプト2本も独立コピーで実行し、高速側ON／比較側OFFのログとキャラ選択の描画・入力受付、終了コード0を確認。根拠: `diagnostic_scripts_test.json`。
- 同一PCの新旧EXE混在2窓、40秒、脚本入力、遅延15〜25ms・損失5%、短縮コード参加。1,351確定Fの入力・代表状態・メモリ比較で差分／欠落0。両側でWASAPI active、CreateDevice成功。
- 元の指定MBAA.exeを含む77件のEXE／INIは不変。試験で起動した対象のみ終了。
- 報告者の画面未表示はローカルでは未再現。API不具合の修正を確認した段階であり、報告された停止原因と断定しない。Special K併用・別PC・ゲーム中3000 FPSの解決は未確認。

根拠: `build_logs/window_startup_20260913/`。`before/api_probe_16916.json` と `after/api_probe_23692.json`、各ゲームログ、`mixed_pair/rollback_comparison.json`、`preservation_after.json`。再現用の観測DLLソース・32bit注入補助・起動器も同フォルダーに保存。診断ZIPのREADMEとリリース上の診断説明を英語化し、公開名を `cccaster_B_startup_diagnostic_EN_20260913.zip` とした。公開ダウンロードの全体SHA-256 `2fafaa92...da04` と既存3資産の保全を確認。公開根拠は `public_english_diagnostic_verification.json`。バイナリ3点はいずれもPE Machine 0x014c。

## 報告者側の確認手順

診断ZIPのREADMEに従い、ゲームを終了してから3つのEXE/DLLを `cccaster_B` へ差し替える。通常のトレーニングを再試行する。再発時は `cccaster_hook_log.txt` とランチャー出力を採取する。

ZIPの `diagnose_fast.cmd` は詳細ログ付きの高速起動、`diagnose_baseline.cmd` はその起動だけ高速化を外した比較。先に高速側を試し、各試行のゲームを閉じてから次を実行する。ゲームが停止していても、その時点のランチャーログとDLLログを取得できる。設定ファイルを書き換えず、環境変数は各スクリプト内だけに限定する。

最後のStartupAssets行は画像処理を行うフックの設置を表す。画像読込み中の停止を示すものではない。Direct3DCreate9の開始行まで届かない場合は入口解放前後、開始だけの場合は生成API内、戻り後の場合はフック設置やそれ以降を次に調べる。ログは非同期のため、行の欠落だけで停止位置を断定しない。
