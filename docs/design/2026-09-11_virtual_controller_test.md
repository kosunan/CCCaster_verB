# 仮想コントローラによる実ゲーム自動試験

## 実装

`src/harness/run_virtual_controller_pair.py` が ViGEmBus 上に仮想DS4を2台作成する。VID/PIDは054C:05C4と054C:09CC。ゲームは通常のDirectInput列挙・Poll・GetDeviceState・バインド変換・入力時計・中央バッファ・GameMem経路を使用する。`CCCASTER_SCRIPT_INPUT=0` とし、ScriptedInputや旧直接入力書込みには切り替えない。

試験プロセスだけに `CCCASTER_TEST_VIRTUAL_PRODUCT` を渡し、指定製品が1台のときだけ読む。一致なし／複数なら入力0と診断ログを出し、試験側は不合格にする。既存のコントローラ設定には書かず、試験用バインドをメモリ内で持つ。物理DS4等が同じ製品として接続されている場合は、曖昧な識別を許さない。

各側のアナログ上下左右・左上、十字キー上下左右、A/B/C/Dを順に押し、DirectInput変換値と解放後0を照合する。操作していない側への混入も検出する（13項目×2台）。その後、キャラ選択・ランダムステージ・対戦・元ゲームの再戦メニューを操作する。選択開始後の既定18秒無操作で接続維持も確認する。

`--hotplug-delay`を指定すると、先にゲームを起動し、指定秒数後に仮想DS4を接続する。実装側は`WM_DEVICECHANGE`を即時再列挙の合図にはせず、ゲームフレームへ再検出要求を渡す。通知を受け取れない機器もあるため、キャラクター選択中だけ1秒間隔で接続GUID一覧を比較し、差分がある場合に限ってDirectInputデバイスを再構築する。再構築後は保存済みデバイス名に対応する設定も読み直す。対戦中は一覧取得・再列挙を実行しない。

`--scenario 0` はホストが先にONCE、クライアントが後からONCE。`1` はホストだけキャラセレ、`2` はクライアントだけキャラセレ。後者2つの相手は再戦画面で無操作。既存比較器で、確定済み戦闘フレーム、再戦の確定通知／ACK／遷移、待機中のローカルFとWT、確定後の入力遮断を検証する。ロード後のキャラ・スタイル・カラー・ステージも両者で比較する。

短時間に再戦へ到達するため `CCCASTER_TEST_RETRY_QUICK=1` を使用する。ゲームスレッドで同じ論理Fにラウンドを短縮し、通常の時間切れ・勝敗・メニュー遷移を通す。通常長対戦や物理パッドの手操作試験とは区別する。

## 実行

このPCにはViGEmBusが既に存在する。Python依存は `build/virtual-pad-venv` に隔離し、vgamepad 0.1.0を使用。ドライバがない環境では先に用意が必要。PowerShell 7（pwsh）とビルド済み32bit EXE/DLL、両側の独立したテストゲームが必要。

```powershell
python src/harness/setup_virtual_controller.py
& build/virtual-pad-venv/Scripts/python.exe -X utf8 src/harness/run_virtual_controller_pair.py --seconds 110 --port 17870 --scenario 0
& build/virtual-pad-venv/Scripts/python.exe -X utf8 src/harness/run_virtual_controller_pair.py --seconds 110 --port 17871 --scenario 2 --network '60,96,5'
& build/virtual-pad-venv/Scripts/python.exe -X utf8 src/harness/run_virtual_controller_pair.py --seconds 75 --port 17873 --hotplug-delay 3 --idle-select 0 --test-root _TEST_CLOSE_MBAACC
```

出力先は `build_logs/virtual_pad_日時/`。総合結果は `virtual_controller_result.json`、同期比較は `rollback_comparison.json`、再戦比較は `native_retry_comparison.json`。不一致／遷移未達／証拠不足は終了コード1。既存ゲームの起動を先に調べ、起動中なら配布・操作を開始しない。試験で起動したゲームだけを終了し、仮想デバイスを解放する。既存INIのSHA256も照合する。

## 試験で発見した不具合

独立キャラ選択の早期returnが、IPCの `syncCompleted` 更新より手前にあった。接続済みでもキャラ選択が15秒を超えると、ランチャーが起動同期のタイムアウトとしてゲームを終了していた（表示文言は12秒）。`state.isSynced` が成立した時点で、独立選択分岐の前でも一度通知するよう修正。未接続を接続済みと扱ったり、タイムアウト自体を無効化したりはしない。

再現ログ: `build_logs/virtual_pad_20260911_162820/`、修正前再現の `162910/`。最初の `162759/` はWindows PowerShell 5.1のモジュール探索失敗で起動しておらず、不合格。実行環境をpwshに統一した。

## 確認結果

- 32bitビルドと22 CTest成功。
- `build_logs/virtual_pad_20260911_162941/`: 105秒、15〜25ms・損失5%、26入力項目成功。双方ONCEで再戦、2971確定Fの4種比較すべて差分0・欠落0・FAILEDなし。先に確定したホストは相手待ち289F、再戦ローカルF/WTの不連続0・確定後入力0。両者ステージ46、既存INI不変。この試験は18秒無操作追加前。
- `build_logs/virtual_pad_20260911_163139/`: 110秒、60〜96ms・損失5%、キャラ選択18秒無操作の後26入力項目成功。クライアントだけキャラセレ→再抽選→戦闘復帰。2339確定Fの4種比較で差分0・欠落0・FAILEDなし、R最大4。ステージ34→29が両者一致。再戦ローカルF/WTの不連続0・確定後入力0、既存INI不変。

別PC・物理パッド・全キャラ／全ステージ設定・長時間安定性はこの自動試験だけで確認済みとはしない。

- `build_logs/virtual_pad_20260911_163342/`: 110秒、60〜96ms・損失5%、18秒無操作と26入力項目成功。ホストだけキャラセレ→再抽選→戦闘復帰。2332確定Fで差分0・欠落0・FAILEDなし、R最大4。ステージ12→28が両者一致、再戦ローカルF/WT不連続0・確定後入力0。最新スクリプトのランダム抽選必須判定も合格。既存55 INI不変。
- 最終DLL SHA256: `4F02AB3CF500943139AA3C882681CE7D0AC6127D48E735F8E54919C64D30C749`。buildと両側テストコピーが一致、EXE/DLLはすべてPE Machine 0x014c。`163342/final_files.json`に記録。
- `build_logs/virtual_pad_20260912_014937/`: ゲーム起動3秒後に仮想DS4を2台接続。両側とも再起動なしで約1.26〜1.31秒後に製品を1台として識別し、26入力項目、左右分離、対戦・再戦、確定状態比較、ランダムステージ、INI不変がすべて合格。32bit Releaseビルドと28 CTestも成功。通常の`_TEST_MBAACC`は既存対戦が稼働していたため触れず、`_TEST_CLOSE_MBAACC`だけを使用した。物理パッドの機種別挿抜は未確認。
- `build_logs/virtual_pad_20260912_015600/`: 監視をキャラクター選択中だけに限定した最終版。起動3秒後の2台を約1.08〜1.13秒で検出し、26入力項目成功。60〜96ms・損失5%の対戦へ移った後はホットプラグ処理を呼ばず、2265Fの確定比較、双方ロールバック58/61回・最大深度4、再戦、選択ロード一致、INI不変を含む総合判定が合格。最終DLL SHA256は`BC274BDD057B13DE85075C9C919AE6834660E547EA96DAB2AD3659EDB77DD060`で、build・隔離2窓・通常2窓に反映済み。

## 環境の後片付けに残る事項

最初のpip導入で、vgamepad 0.1.0のビルド処理が同梱ドライバを実行してしまった。指定したインストール抑止環境変数はこの旧版では効かず、16:24:34にエラー31の重複バス `ROOT\SYSTEM\0004` が作成された。既存バス `ROOT\SYSTEM\0001` と物理XboxはOK、試験が生成したDS4は2台とも解放済み、起動前からのGUI PID8756も継続している。

重複項目だけへの `pnputil /remove-device` はAccess is deniedとなり、この実行環境の権限では除去できていない。既存バスも使うドライバパッケージのアンインストールは行わない。管理者のPowerShell 7で次を実行すると、識別名・エラーコード・作成日時・既存バスを検査してから重複項目だけを削除する。

```powershell
./src/harness/remove_duplicate_test_bus_20260911.ps1
```

再導入は `setup_virtual_controller.py` を使用する。PyPIのSHA256を確認し、PythonモジュールとクライアントDLLだけを隔離環境へ配置してsetup.pyやドライバ導入を実行しない。実行済みで既存バスへの接続成功。出典・SHA256は `build/virtual-pad-venv/virtual_controller_dependency.json` に保存した。
