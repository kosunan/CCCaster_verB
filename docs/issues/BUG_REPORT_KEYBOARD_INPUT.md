# キーボード・コントローラー入力が効かない報告

## 最新状態：修正版の通常起動で復旧確認済み

2026-09-13、`cccaster_B_training_P1_fix_EN_20260913.zip` についてユーザーから「このバージョンでは通常起動で正常に動いたそうです」との続報を受領。Trainingの1P無反応は、修正済み・報告者の通常起動で復旧確認済みとする。今回の根拠はユーザー経由の動作報告で、新たなログや当方の実機試験ではない。以下の通常起動確認待ちはこの続報以前の記録。別件のFPS報告は対象外。

## 通常CLIのTraining起動席の不一致を再現・修正

対戦回帰：新旧EXE混在の同一PC2窓、脚本入力、40秒、15〜25ms遅延／5%損失で1,325確定Fの入力・代表状態が一致。差分／欠落0、ロールバック8／2回（`mixed_pair/rollback_comparison.json`）。通信対戦の役割維持をこの範囲で確認した。

診断無効での通常CLIメニューも追加確認し、host=1・InputConfig/InputSource/InputRouteの出力なしで戦闘・移動・攻撃に成功（`training_20260913_141459/`、`no_diagnostic_check.json`）。診断のログ待ちによって偶然直るという説明ではない。英語手順付き `cccaster_B_training_P1_fix_EN_20260913.zip` を同証拠フォルダーへ作成し、PE 32bit・5収録ファイルの再読一致とSHA256を `package_verification.json` に保存。ゲーム・INI・ログは収録せず、公開アップロードはしていない。

追加報告は「1Pで動かず2Pでは動くが、入力診断ZIPのdiagnose_input.cmdなら動く」。差し替え後の通常GUIは未試験との回答。ログは `build_logs/training_p1_20260913/reporter_hook_after.txt` と `reporter_launcher.txt` に保存。以前の10起動はmode=1 host=0、最後の診断起動はmode=1 host=1。成功起動はP1にF500 ELITE、P2は未選択で、設定保存後に非ゼロのP1入力が通過し、mode=1の戦闘まで進んでいる。今回はINI読込警告がなく、前の別報告者の設定欠落を原因として流用しない。

通常CLIメニューは `_isHost=false` の初期値／直前の接続役割を残したままTrainingを起動する。一方、`--training` とGUI Trainingは既にhost=true。SceneFastBootはこの役割を見て起動ナビの決定入力をP1／P2へ送るため、通常メニューではP2側からTrainingへ入る。オフラインの入力書込みがP1/P2を正しく分けていても、ゲーム側の操作席が異なっていた。

修正前バイナリの通常CLIメニューを隠しコンソールへの選択入力で実行し、仮想DS4のP1決定がraw1=output1=00000410として渡っても30秒間キャラ選択から進まないことを再現（`training_20260913_141318/`）。同じDLL・同じINIでランチャーだけ修正するとhost=1でTraining戦闘へ到達し、接近・攻撃まで成功（`training_20260913_141410/`）。両試験ともINI不変。仮想DS4専用の入力経路なので物理デバイス設定の総合検証ではない。

修正は `MainController::LaunchAndMonitorGame()` の共通入口でTrainingだけ `_isHost=true` に揃える。通信対戦の役割は維持し、入力割当やゲームメモリーへの書込方法は変更しない。32bitビルド・37 CTest成功。復旧報告を一括で解決扱いにせず、この再現できた起動経路の修正と、報告者側の通常起動確認待ちを区別する。以下は修正前の経緯。

## 最新状態：両機器での復旧報告を受領

2026-09-13、ユーザーから診断版でキーボード・コントローラーの両方が動くようになったとの報告と3ログを受領。`build_logs/input_route_20260913/reporter_recovery/` に原本を保存し、`hashes.json` にSHA256を記録した。以下の未達記述はこの続報以前の調査経緯。

- `hook_after.txt:215` でKeyboard（resolved=-2）の実設定を再読込、247行で保存・終了許可。253行ではconfiguring=0、raw1=output1=00080000で非ゼロ入力が遮断されず書込関数へ渡っている。
- 同370行でWireless Controller（resolved=0）を再読込、402行で保存・終了許可。410行以降も非ゼロの入力取得・遮断後出力が記録されている。両機器でゲームウィンドウと前面ウィンドウは一致している。
- 記録された入力はキャラ選択mode=20。ゲーム側の消費をログだけで証明するものではないが、ユーザーの操作成功報告を裏付ける入力経路が確認できた。
- `launcher.txt:1` はINIを開けず既定値で起動した警告、`hook_after.txt:122` は開始時P1未選択。今回の試験ではその後の選択・保存が反映されている。以前の失敗時と設定条件が同一だったかは不明であり、以前の選択ミスや診断ログ追加による修正とは断定しない。

状態は「報告者による復旧確認、ログで両機器の入力通過確認、根本原因は未特定」。今回の更新はログ保管と文書のみ。追加のコード修正・再ビルド・公開は行っていない。末尾のWASAPI→QPC切替は入力不能の原因を示す証拠ではなく、別件FPSの解決扱いにはしない。

2026-09-13。先の画面未表示についてはユーザーから解決の報告を受領。別の症状としてキーボード入力が効かないとの報告とDLLログを受領した。キーボード設定・F4の応答・実際に押したキーは未確認。原因確定・修正は未達。

続報：F4の「最後の入力」ではキーボードを検出できるが、ゲーム操作には反映されないとの回答。提出されたP1Device=Wireless Controllerは最後にコントローラーを試した結果であり、Keyboardもドロップダウンで正しく選択してテストしていたとの訂正を受領。提出INIだけを根拠にデバイス選択ミスとした判断は撤回する。

F4のKeyboardEdgeはImGuiへ届いたキーイベント、ゲーム側のCheckInputBindはGetAsyncKeyStateと前面ウィンドウの一致条件を使う。設定保存成功時にはConfigManagerの再読込とDirectInputHook::ReloadConfigsが呼ばれるため、静的確認だけでは保存後の未反映を断定できない。続報でキーボード・コントローラーともゲーム内で無反応と確認。共通する設定反映／入力遮断／ゲームへの入力受渡しを優先して調べる。

## 受領ログ

`build_logs/keyboard_report_20260913/reporter_hook_log.txt` に保存。受領ファイルのSHA-256は `5a2f2917fb9f7800561c318c29bdce50ed43834bea21dabe90b13141e76d8938`。

複数回の起動が追記されたログ。最新は `game-only multi-instance bypass applied` の診断版で、Training（mode=1）、gate通知成功、CreateDevice HRESULT=0、WndProc設置成功、キャラ選択mode=20、Metronome開始まで記録されている。WASAPIから連続QPCへの切替も記録されているが、入力不能との因果関係を示す記録はない。

設定読込先は `C:\Games\MBAACC Vanilla\cccaster_B\cccaster_v10.ini`。P1Device/P2Device、キーバインド、前面ウィンドウ、キー押下は記録されていない。描画初期化成功やStartupInput preparedだけで物理キーボード取得成功とは判断しない。

## コードで確認した条件

- `DirectInputHook::ReloadConfigs()` はSettingsのP1Device/P2Deviceを読む。未指定は未選択となり、その席の入力は0。
- Keyboardを指定した場合は `Keyboard.ini` のMappingを使用。未保存項目の既定はUp=I、Down=K、Left=J、Right=L、A/Confirm=S、B/Cancel=D、C=F、D=G、E=E、Start=LeftShift。
- キー取得は `GetAsyncKeyState`、ゲームウィンドウが前面のときだけ受け付ける。
- F4内の変更は保存まで実入力へ反映しない。`SAVE & CLOSE [F4]` またはF4で保存して閉じる。設定中のゲーム入力遮断は仕様。
- TrainingではP1/P2の設定をそのまま使う。ログのhost=0だけを根拠に1Pを読まない不具合とは断定しない。

## 入力診断版

追加検証：英語の `diagnose_input.cmd` 自体から通常入力設定で起動し、キャラ選択、InputSource・InputRouteの出力、高速起動ON、ログ収集と正常終了を確認（`diagnostic_scripts_test.json`、`cmd_input_game.log`）。同一PCの新旧EXE混在2窓・脚本入力・40秒・15〜25ms遅延／5%損失で1,315確定Fの入力・代表状態が一致、欠落0、ロールバック11／12回。根拠は同証拠フォルダーの `mixed_pair/`。これは物理機器操作の確認ではない。

`CCCASTER_INPUT_DIAGNOSTIC=1` のプロセスだけ、実設定の再読込（InputConfig）、設定画面の保存成否（InputSetup）、P1の取得値・前面ウィンドウ（InputSource）、オフライン入力遮断前後（InputRoute）を記録する。周期ログは60呼出ごとと値の変化時。InputRouteのoutputはゲーム入力書込関数へ渡す引数であり、実メモリー読戻しやゲーム側の消費を保証しない。原因を修正した扱いにはしない。

英語手順付き `build_logs/input_route_20260913/cccaster_B_input_diagnostic_EN_20260913.zip` を作成。3バイナリ・起動cmd・README・SHA256のみを収録し、ゲーム本体・INI・ログは含めない。報告者には診断cmdから起動し、各機器をP1に選択して保存・閉じ、全入力を一度離した後に再操作してログを取得してもらう。公開アップロードは行っていない。

32bitビルドと37 CTest成功。仮想DS4を使用した実Trainingでゲーム中まで到達し、非ゼロのInputRouteが遮断後も渡ることとINI不変を確認。仮想入力専用経路を使うため、通常P1の機器選択・物理操作の代用ではない。根拠は `build_logs/input_route_20260913/build.log`、`ctest.log`、`training_20260913_065446/game.log` と `result.json`。報告者環境での再現・原因確定・復旧は未達。
