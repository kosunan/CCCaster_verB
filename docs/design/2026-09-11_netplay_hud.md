# Netplay HUD / Character Select Settings — 2026-09-11

## 操作 / Controls

- Ctrl+0–8: Input Delay (frames)
- Alt+0–8: Maximum Rollback (frames)
- F4: Controller Setup
- D + R must be <= 8. Number row and numpad supported.
- Both players can change settings on Character Select. Values are synchronized through confirmed input frames and take effect at the next match. Settings stay fixed during a match.
- SYNCING...: waiting for confirmed input. NEXT MATCH: synchronized values ready for the next match. D + R <= 8: change rejected; previous valid values retained. NOT APPLIED: an unconsumed request was discarded at the scene boundary.
- Update both peers to protocol version 9. Existing controller configuration is preserved. Changes are session-only and are not saved as launcher defaults.

キャラクター選択中に双方から変更できる。設定コマンドを通常の確定入力列に含め、既存の再送・順序制御を使用する。同じフレームの競合はP1→P2の順に処理し、その都度D+R<=8を検査。同期中は次の要求を受け付けない。現在のキャラセレの遅延枠は変更せず、次の入力世代入口でD/Rを適用し、予測履歴とスナップショットを既存手順で作り直す。

入力の上位8bitを設定命令に使用し、ゲームに渡す前に除去。ディレイはA0..A8、最大ロールバックはB0..B8。通信パケットの従来D/R欄は接続時の設定照合用として維持し、対戦の実適用値とは区別する。旧バージョンが命令を方向入力と解釈しないよう通信版9へ更新した。

## HUD

DirectX 9の既存描画経路を使用。ImGuiウィンドウを作らず、描画リストへ薄い半透明パネルと文字を直接追加する。DELAYはシアン、MAX RBはアンバー、変更時は800msの小さな強調。キャラセレは2行、対戦中は1行。英語ASCII表記でフォントの言語依存を避ける。未計測JTR、周期換算fps、ゲームの遅れと混同しやすい時計差は非表示。RTTは従来どおり直近60UI更新に渡された推定値の最大。

キーイベントのCtrl/Alt判定をGetKeyStateへ変更し、短い操作で修飾キー状態を取り逃す問題を修正。AltGrは設定ショートカットとして扱わない。キャラセレで修飾キーを押している間は非同期キーボード入力のゲーム反映も抑制する。

## 検証

- Release 32bitビルド成功。12テスト成功。
- build_logs/bounded_real_20260911_014144: 実ゲーム45秒、遅延15..25ms・損失5%。ホストD=3、クライアントR=2を同じ確定フレームで処理。D=8への変更は合計超過で拒否。次の対戦で双方D=3/R=2を適用。
- 同試験の確定1344フレームでREC/FRAME/STATE/MEMの不一致0、欠落0。ロールバック31/32回、最大深度2。比較器の既存除外項目は絶対WorldTimer・メニューカウンタ。
- build_logs/bounded_real_20260911_014452: 640x480のキャラセレ実画面でCtrl+1、Alt+3を送信し、双方の画面にD=1/R=3とNEXT MATCHを確認。設定確定フレーム67076/67699も双方一致。
- 通常文字列のみの単体表示ではなく、ゲーム上のキャラセレ・対戦HUDを目視確認した。全解像度・全キーボード配列の試験は未実施。

試験専用: CCCASTER_SCRIPT_INPUT=1とCCCASTER_TEST_SETTINGS=1で設定変更列を送信。同設定=2でキャラセレの自動操作を止めてHUD/ショートカットを確認できる。通常起動では無効。
