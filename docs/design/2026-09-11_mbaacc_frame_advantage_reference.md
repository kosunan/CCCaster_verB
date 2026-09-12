# MBAACC既存トレーニングツールの有利不利表示

調査日: 2026-09-11。ユーザー指定の2リポジトリを固定コミットで静的確認。外部プログラムの実行・本ゲームへの実装・実機検証は未実施。

後続作業で本ゲームへの計測と確定表示を実装し、トレーニング通常攻撃を実機確認した。[実装と検証](2026-09-11_utilities_implementation.md)。以下は参照元の調査時点の記録。

## 標準機能の訂正

入力が変化したときに履歴を追加し継続Fを添える表示は、ユーザーの確認によりMBAACC標準実装として扱う。Fightcadeから新たに取り入れる機能という前回の分類を撤回する。CCCaster独自の入力ログや予測／確定入力の診断表示とは別に、ゲーム標準の履歴を維持する。

## 参照元

- kosunan/MBAACC_Training: `b9a74cb455c2da0ad8c601cffff2791e13b31af0`。
  - [Fn_04_Cui_cnt.py](https://github.com/kosunan/MBAACC_Training/blob/b9a74cb455c2da0ad8c601cffff2791e13b31af0/src/Fn_04_Cui_cnt.py): 状態分類、バー、Advantageの計算・表示。
  - [MBAACC_Training.py](https://github.com/kosunan/MBAACC_Training/blob/b9a74cb455c2da0ad8c601cffff2791e13b31af0/src/MBAACC_Training.py): ゲーム時計を監視する更新ループと操作キャラ選択。
  - [アドレス表](https://github.com/kosunan/MBAACC_Training/blob/b9a74cb455c2da0ad8c601cffff2791e13b31af0/src/Fn_01_Address_Table_and_utill.py)。
- fangdreth/MBAACC-Extended-Training-Mode（ETM）: `038887d7d8e6e70963ce9eb5780a25ac35cf1cac`。既存の[メモリ知識ベース](../memory/README.md)と同一コミット。
  - [DLLのFrameBar.cpp](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/FrameBar.cpp): ゲーム内フレームバー。
  - [外部表示のFrameDisplay.cpp](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/MBAACC-Extended-Training-Mode/FrameDisplay.cpp): コンソール側表示。DLL版と区別して参照。

## 計測と表示の比較

| 項目 | kosunan版 | ETM DLL版 |
|---|---|---|
| 計測を有効にする条件 | `process_exclusive_state`で両側の主状態色が非ニュートラルなら`is_adv_flag=true` | `CalculateAdvantage`で両側の`inactionableFrames != 0`なら有効化し双方のカウンタを0にする |
| 片側が先に動けた期間 | 主状態色が片側だけ非ニュートラルなら、もう片側を`adv_color`にする。その色が続いた更新数を数える | 行動不能0の側だけに`adFrameCount - nLastFrameCount`を加える |
| 終了 | 両主状態色がニュートラルになるとフラグOFF。最後に保持した有利カウンタを表示する | 両側の行動不能が0ならフラグOFF。カウンタの値は保持する |
| 表示の符号 | P1側の有利値が非0なら正、そうでなければP2側の値を負にする | P1カウンタ−P2カウンタ。P2の表示行では符号を反転 |
| 更新単位 | ゲーム時計の変化を外部ループで待つ。バーの連続値は呼出しごとに1加算 | `adTrueFrameCount`が変わったとき処理し、有利時間は`adFrameCount`の差分で数える |
| 停止の扱い | ヒットストップと残り動作Fの変化を見て双方停止を判定し、バーの位置更新を抑止 | 有利時間加算ではP1/P2 FreezeとGlobalFreezeが0の条件。バーでは共通ヒットストップ・画面停止の表示設定が別にある |
| 数値表示 | コンソール上部に有利値と距離、下部に両側各3行の色付きバー。長さは呼出元で80セル | `Startup / Total / Advantage / Link Window`を双方表示し、状態バーと併用 |

根拠: kosunan `Element_Bar.update_bars`（128行以降）、`FrameIndicator.process_exclusive_state`（284行以降）、`get_advantage_value`（274行以降）、`state_bar_cre`（321行以降）。ETM `CalculateAdvantage`（242行以降）、`BarHandling`（688行付近）、`UpdateFrameBar`（771行付近）、数値表示（137〜145行）。

**両実装とも「双方が硬直終了するまで数値を隠す」という厳密な表示契約ではない。** 片側が動ける期間の途中も計算・表示を更新し、双方終了で計数が止まる。前回の「双方終了を待って表示」という説明を、そのまま両ツールの実装説明として使用しない。

## 行動可能の判定と注意点

kosunanの`get_color_code`は残り動作Fに加え、シールド・バンカー・被弾・ジャンプ・無敵・除外動作を区別し、残りFが0でも動作番号350を被弾状態色にする。主状態色を用いた分類で、単に残りFが0なら行動可能という式とは異なる。

ETMの有利値計算自体は`PlayerAuxData::inactionableFrames`を使う。一方、バー全体の継続条件にはガード可否に関わるフラグ、飛び道具、パートナー攻撃もある。バーの色分けではガード硬直・空中状態・受身不能等を見る。**バー表示の条件と、有利値加算の条件を混同しない。** 操作キャラが交代する場合は`FB_Main1/2`と補助キャラの参照先を切り替える。

静的確認で残る注意点:

- kosunanではバーの数値更新後に双方停止判定・位置更新を行う。「双方停止中は全カウンタの更新も停止」とは読めない。
- kosunanは観測1回ごとに加算するため、外部ポーリングの欠測を正確なF数として埋められるとは限らない。外部ツールの待機周期をCCCasterへ移植しない。
- ETM DLL版は最終有利値の計算で`% 100`を使う。CCCasterでは符号付き実数値を保持し、大きい値を剰余で別の値に見せない。
- どちらも上記の計算条件だけでは、全ケースの「ガード後の確定した有利不利」を保証しない。空振り、再接触、着地、キャンセル、受身・起き上がり、投げ、飛び道具、パートナーを実機照合する。
- kosunan READMEは`FirstActive`を接触したフレームと説明し、飛び道具等で不正確な場合を明記する。有利値とは別指標であり、発生Fへ単純に転用しない。

## CCCasterへの反映方針（設計・未実装）

1. 入力履歴はゲーム標準を使う。有利不利の追加表示はトレーニング用途を対象にする。
2. ETMの行動不能カウンタを主な候補とし、kosunanの状態分類で補助条件を照合する。現行GameMem・版識別を通し、外部ツールの固定アドレスをそのまま読む構成にはしない。
3. `未計測 → 計測中 → 確定`を表示状態として別に持つ。無効／未測定を0Fと表示しない。計測中の値を出すなら「計測中」を明記する。
4. 初期実装は地上の単発ガード等、行動可能の境界を照合できるケースに絞る。接触種別を保持し、両側が再び行動不能になる連係で前の値をどう扱うか決める。
5. ゲームの通常更新から1Fずつ採取する。表示回数、実時間ms、WASAPIの予定周期から硬直Fを計算しない。画面停止・ヒットストップの意味を分ける。
6. リセット・ロード・操作キャラ交代で過去の確定値を無効化する。将来ネット対戦へ拡張する際は、巻戻しで観測を破棄／再構築し、未確定状態を確定値として残さない。
7. P1視点の`+nF / -nF`とP2視点の逆符号を明示。標準入力履歴やゲームHUDと重なるか実画面で確認する。

完了判定には、両者同時終了、P1先行／P2先行、画面停止、片側ヒットストップ、連係・着地・飛び道具、リセットのゲーム内フレーム列と表示値の突合が必要。今回確認したのはソース上の条件であり、この完了判定はまだ満たしていない。
