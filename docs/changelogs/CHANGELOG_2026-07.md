# refactor: 間違った選択肢を消す — 矛盾23件の調査結果に対処

`src/` 全体を走査して実装と食い違う記述を洗い出した（`docs/issues/CONTRADICTIONS_2026-07.md`）。
対処は「記述を直す」ではなく「間違いを選べなくする」で統一した。コメントで
禁止しても次の作業者は読まないため。

**消した選択肢**: `FastBootRunner`（`SceneFastBoot` と競合しうる裏スレッド）、
`FrameInputSlot` / `FrameSyncState`（`GamePhase` と概念が二重定義）、
`SetModePause` / `SetModeRollupSkip` / `ClearInput`（速度モードを4→2に）。
`rollback/` はビルド対象から外した — 入力を 16bit で扱う設計で現行の `GameInput` と
非互換なため。ステート保存アドレスに参照価値があるのでファイルは残す。

**消した記述**: 嘘の冒頭コメント、重複したフロー一覧、食い違う呼び出し元の記述。
直さずに消したのは、実装1箇所が唯一の真実であるべきだから。

**直したバグ**: 受信ループが `inputCount` を無検証で使い `inputs[10]` を
オーバーランしていた。Rematch の自動ナビが同フレーム内で確定入力に上書きされて
効かなかった（2c の結線で作り込んだ）。`Poll()` が1フレームに2〜3回呼ばれ、
`prevState` が毎回更新されてエッジ検出が原理的に動かなかった
（「コントローラ設定でボタンを押しても反応しない」の原因）。

---

# chore: changelog を軽量化し、テストの時間圧縮を追加

changelog に測定値・表・変更ファイル一覧まで書いていたため、git 履歴と三重に
重複して読む場所が分からなくなっていた。「なぜそうしたか」だけに絞り、
測定値と設計の詳細は `docs/design/` に置くルールに変えた（`CONTRIBUTING.md`）。
過去のエントリは書き換えない — 履歴を触る価値がないため。

`CCCASTER_TIME_SCALE` でフレーム周期を圧縮できるようにした。検証1回が47秒
かかり試行回数の制約になっていたため。4倍速で13.3秒、同じ不具合を同じように
検出できることを確認済み。

スケール対象を Metronome・通信スレッド・ハーネスの3箇所にしたのは、通信スレッドが
独自のペースを持っており、そこを外すとゲームだけ速くなって歩調が崩れるため。
θ・RTT・α補正は実時刻で測る値なのでスケールしない。

**圧縮するとタイミング余裕の検証にはならない**（4倍速では RTT に対する余裕が 1/4）。
区切りの実機ゲートは必ず等倍で回すこと。

---

# feat: 実機メモリトレースを追加し、実測値で harness を校正 — デシンクの再現に成功

## 2026-07-27: 実機の振る舞いをテスト環境へ反映

### 概要
実機のゲームメモリを毎フレーム記録する `MbaaMemTrace` を追加し、
その実測値で `FakeGame` のタイムラインを校正した。あわせて harness にも
実機と同じ「同じ netFrame でゲーム状態が一致するか」の判定を追加した。
結果、**実機でしか見えなかったデシンクが harness で再現するようになった**。

### 追加した観測
`CCCASTER_MEM_TRACE=1` で毎フレーム1行を出力する。

```
[MEM] netFrame mode intro state WT RT roundTimer menuCtr
      rng0 rng1 p1seq p2seq p1hp p2hp roundCnt p1win p2win
```

`netFrame`(writeHead) を先頭に置き、両プロセスをこの番号で突き合わせる。
RNG 状態とキャラのシーケンス番号を入れたのは、体力やタイマーより早く確実に
差が出るため。判定は列ごとに「最初に食い違った netFrame」を出す形にした。

トレースは seam の外に置いた。`IGameMemory` は同期ロジックが使う最小の窓口で、
観測用の値を足すと責務がぼやけるため。harness ではスタブに置換する。

### 実測で確定した原因

```
HOST   : WT - netFrame  開始 -156 → 終了 207   （差 363 = starve 363）
CLIENT : WT - netFrame  開始 -156 → 終了 203   （差 359 = starve 359）
```

**`starve` の回数がそのままゲームフレームとネットプレイフレームの対応ずれ量**。
両者は WT=45 / netFrame=201 で揃って始まるが、背圧で書込みを止めるたびに
ゲームだけが進み、対応が1ずつ崩れる。

その結果、実機では以下のように状態が分岐していた。

| 列 | 分岐点 |
|---|---|
| `rng0` | netFrame=201（ほぼ即座） |
| `WT` | netFrame=208 (host=380 / client=52) |
| `p1seq` / `p1hp` | netFrame=367 |

同じ netFrame でも両者のゲームが全く違う地点にいるため、乱数消費もキャラ状態も
一致しない。**入力の伝送は正しく（共通フレームの入力列は一致、conflict 0）、
ゲーム進行の対応付けが壊れている**という切り分けが確定した。

### 実測値による FakeGame の校正
| 区間 | 旧（机上） | 新（実測） |
|---|---|---|
| CharaSelect | 180F | 456F |
| Loading | 60F | 55F |
| intro=2 | 60F | 138F |
| intro=1 | 60F | **224F** |
| ラウンド(intro=0) | 300F | 1761F |

intro=1 の 224F は `MbaaAddresses.hpp` の `CC_PRE_GAME_INTRO_FRAMES (224)` と
一致しており、測定が正しいことの裏付けになる。

### harness に状態突き合わせを追加
これまで harness は入力列しか比べておらず、状態の乖離を見ていなかった。
前回 harness が [OK] を出したのに実機がデシンクしていたのはこのため。
実機と同じ物差し（同じ netFrame での mode / intro / WT / RT 一致）を入れた。

### 再現の確認
| | 最初に分岐する列 | 内容 |
|---|---|---|
| 実機 | `WT` | netFrame=208 で host=380 / client=52 |
| harness（校正後） | `WT` | netFrame=231 で host=100 / client=75 |

同じ列が同じ機構で分岐する。85秒の実機テストでしか見えなかったものが
47秒の harness で捕まえられるようになった。

### 次の一手（未着手）
ネットプレイフレームが自由走行のカウンタで、ゲームの進行に紐付いていないことが
根本原因。`WorldTimer` はゲームフレームと 1:1 で進むので、合意した基準点からの
相対値をフレーム番号にすれば、netFrame N が両者で同じゲーム瞬間を指すようになる。

> **【訂正 2026-07-27】** 本節で「コミット `f5241e3` の『WT基準の相対フレーム同期』が
> CB撤去で失われた」と書いたのは誤り。全524コミットを横断検索した結果、WorldTimer から
> フレーム番号を導出する演算は履歴のどこにも存在しなかった。f5241e3 が入れたのは
> 変数とヘルパーだけで、`ToRelativeFrame()` は一度も呼ばれていない。
> 変数名とコミットメッセージから実装の存在を推測し裏を取らなかったことによる誤り。
> 実在した WT 結合は `505ea07`(2026-03-03) の SleepFrame 完全一致同期で、これは別物。
> 正確な調査結果は `docs/design/core_dll/GameMemory_Seam.md` §8 を参照。

### 変更ファイル
- [NEW] `mbaa_mem/MbaaMemTrace.hpp/.cpp` — 実機メモリの毎フレーム記録
- [MODIFY] `engine/SceneRunner.cpp` — トレース呼び出し
- [MODIFY] `src/harness/FakeGame.hpp/.cpp` — 実測値で校正、状態サンプリング追加
- [MODIFY] `src/harness/harness_main.cpp` — 状態記録の書き出し
- [MODIFY] `src/harness/harness_stubs.cpp` — MbaaMemTrace のスタブ
- [MODIFY] `src/harness/run_pair.ps1` — ゲーム状態の突き合わせ
- [MODIFY] `src/harness/run_real_pair.ps1` — `-MemTrace` オプション
- [MODIFY] `src/core_dll/CMakeLists.txt`

---

# feat: 実機テストを自動化 — デプロイ強制・入力自動生成・決定性判定

## 2026-07-27: 実機テストのゲート整備

### きっかけ
`dual_test.bat` 実行時にキャラセレでボタンが反応しないとの報告。
原因は**配備されていた DLL が B-2 時点のもので 2a〜2d が入っていなかった**こと。
B-2 時点では入力パイプラインが撤去済みだったため、反応しないのは仕様どおり。
キーアサイン画面が反応したのは、そこが `UIManager` から DirectInput を
直接読む別経路でパイプラインを通らないため。

`dual_test.bat` がデプロイしない罠を2回踏んだ（3/11版で1回、B-2版で1回）。
`AGENTS.md` に書くだけでは足りなかったので構造的に塞いだ。

### 実装
1. **`dual_test.bat` が必ずデプロイする**ようにした
2. **`src/harness/run_real_pair.ps1`** を新設。デプロイ → ハッシュ照合 → 起動 →
   待機 → 判定を一気通貫で行う。人の操作は一切不要
3. **入力の自動生成**。`CCCASTER_SCRIPT_INPUT=1` で `ScriptedInput()` に切替。
   フレーム番号だけから決まる純関数で、DLL と harness が
   `core_dll/common/ScriptedInput.hpp` を共有する
4. **実機の決定性判定**。DLL が配信フレームを `[REC] netFrame p1dir p1btn p2dir p2btn`
   として記録し、両プロセスのログを突き合わせる（スクリプト入力時のみ出力）
5. 設計書 §7 に実機ゲートと「実機でしか出ない不具合の見分け方」を追記

### 実機テストの結果

```
host  : 3717 フレーム配信 / client: 2918 フレーム配信
共通フレーム: 2850
[OK] 共通フレームの入力列は完全に一致
conflict: 両者 0
```

IntroBarrier も実機で機能した（HOST=WT723 / CLIENT=WT718 でほぼ同時に解除）。

### 発見: 入力列は一致するがゲーム状態が乖離する

| | ラウンド開始回数 | RealTimer | stall |
|---|---|---|---|
| HOST | 1回 | 3298 | 451 |
| CLIENT | **2回** | 496 | **1252** |

WorldTimer は 4171 / 4192 とほぼ同じで、両者は同じ実時間だけ動いている。
にもかかわらず CLIENT だけラウンドを1つ多く終えており、ゲーム状態が
デシンクしている。

**原因**: 1ゲームフレームと1ネットプレイフレームの対応が保証されていない。
現在の実装は「入力が確定したらゲームメモリに書く」だけで、
ゲーム側のフレーム進行そのものは制御していない。
`stall` の間もゲームは進み続け、直前の入力のまま次のフレームを処理する。
HOST が 451 フレーム、CLIENT が 1252 フレーム分そうなったため、
同じ入力列を違うゲームフレームに適用する形になり結果が分かれた。

`GC::SetModePause()` が名前に反してゲームを止めない（`SetNormalSpeed()` と
同一実装）ことも、この構造の裏返し。

**これは入力パイプラインの不具合ではない。** 入力の伝送は決定的に動いている
（共通2850フレーム一致、conflict 0）。足りないのはフレーム進行の同期であり、
旧CCCaster がメトロノームとフレームスキップで解いていた領域にあたる。
harness の FakeGame はタイムライン駆動で進むため、この問題は harness では
再現しない — 実機テストを入れて初めて見えた。

### 変更ファイル
- [NEW] `core_dll/common/ScriptedInput.hpp` — 自動テスト用の入力列（DLL/harness 共有）
- [NEW] `src/harness/run_real_pair.ps1` — 実機2窓の自動テスト
- [MODIFY] `engine/SceneRunner.cpp` — スクリプト入力への切替と `[REC]` 記録
- [MODIFY] `src/harness/harness_main.cpp` — 共有ヘッダを使うよう変更
- [MODIFY] `_TEST_MBAACC/dual_test.bat` — デプロイを追加（gitignore 対象）
- [MODIFY] `docs/design/core_dll/GameMemory_Seam.md` — §7 実機テストのゲート

---

# feat: SceneInputFilter を実装 — 画面ごとの入力制約を送信前に適用 (2d)

## 2026-07-27: 入力パイプライン再構築 2d

### 概要
no-op スタブだった `SceneInputFilter` を実装した。証言①「キャラセレがたまに
ズレる」への対策で、旧CCCaster の historyCheck 相当（ISSUE_TRACKER A-3 / A-4）。

### 適用位置を「送信前」にした理由
フィルタはローカル入力をバッファに書く直前に適用する。読み出し時ではない。

読み出し時に適用すると引数の `phase` がローカルのものになる。ロード時間が
左右で違うとフェーズは実際にずれるため、同じフレームに対して両者が違う
フィルタをかけて決定性が壊れる。送信前に適用すればフィルタ済みの値が回線を
通るので、両者が受け取る値は必ず一致し、フェーズの一致を前提にしなくてよい。

これに伴い `SceneRunner` の読み出し側（F2）からフィルタ呼び出しを撤去した。

### 実装した制約
| 画面 | 制約 |
|---|---|
| CharaSelect / Rematch | カーソル移動後 `DIR_SEAL_FRAMES`(2F) は決定・キャンセルを封印 |
| CharaSelect / Rematch | 決定受付から `CONFIRM_GUARD_FRAMES`(3F) 以内の決定を破棄 |
| InGame | START / FN1 / FN2 を除去 |

やりすぎないための条件もテストで固定した。
- ニュートラルへ戻すのは「移動」ではない（レバーを離すと決定できなくなるため）
- 同じ方向を押し続けても封印し直さない（押しっぱなしで永久に決定不能になるため）
- InGame では封印を効かせない（方向転換のたびに攻撃が消えると操作不能）

状態はフェーズ遷移時に `Reset()` で捨てる。呼び出しは1フレーム1回で、
バッファへ実際に書き込むフレームとだけ対応する（背圧で止めたフレームでは呼ばない）。

### 検証
`ctest` 5スイート全緑（`scene_input_filter` を新設、15ケース）。
対称ロードの決定性チェックは [OK]、共通 1,031 フレーム一致、`conflict` 両者 0。

### 未解決: 非対称ロードでの1フレームずれ
ロード 60F/240F の決定性チェックが [NG]（303フレーム不一致）。
測定した結果、規則性が明確だった。

```
共通フレーム 684 のうち 同一フレームで一致: 381
p1 (HOST→CLIENT 方向) の不一致: 303
p2 (CLIENT→HOST 方向) の不一致:   8
host[N].p1 == client[N+1].p1 : 682 / 684
```

HOST→CLIENT 方向だけが一様に1フレームずれている。`conflict` は両者 0 なので
2c で解消したデータ競合ではなく、フレーム付番の問題。

**2d の退行ではない。** フィルタは内容を変えるだけで付番に触れず、対称ロードでは
[OK] のまま。2c の時点で非対称ロードの決定性は未検証だった。

原因は未特定。送信側の `BuildPacket` と受信側の `ProcessReceivedPacket` は
`inputs[i] ↔ baseFrame - i` で対称に見えるため、コードを読むだけでは特定できて
いない。次は送信時と確定時の (frame, input) をログに出して突き合わせる。
推測で直さない。

### 変更ファイル
- [MODIFY] `engine/SceneInputFilter.hpp/.cpp` — 実装（no-op スタブから）
- [MODIFY] `engine/SceneRunner.cpp` — 適用位置を (F2) から (E) へ、フェーズ遷移で Reset
- [NEW] `src/tests/test_scene_input_filter.cpp` — 15ケース
- [MODIFY] `src/tests/CMakeLists.txt` — `scene_input_filter` を追加

---

# feat: 入力パイプラインを結線し決定性を確認 — 2スレッド競合と背圧不在を解消 (2c)

## 2026-07-27: 入力パイプライン再構築 2c

### 概要
撤去されていた入力パイプラインを結線した。あわせてハーネスに決定性テストを
実装し、2プロセスがゲームに渡す入力列の一致を判定できるようにした。
結線の過程で、旧実装から引き継いでいた**2スレッド間のデータ競合**と
**背圧の不在**という2つの根本問題が露見し、いずれも解消した。

### 結線した経路
```
DirectInputHook → WriteLocal(head+1, 入力, 予測, rollbackable) → 送信
相手パケット    → ConfirmRemote(frame, 入力)
TryReadForGame(readPos = head - (delay+rollback)) → SceneInputFilter → WriteInput
```

`SceneRunner::Step()` の中では一切ブロックしない。相手入力が未着なら何も書かず
即座に抜ける（ゲームメモリには直前フレームの値が残るため挙動は「保持」と同じ）。
ここでブロックすると keepalive が途絶えて切断扱いになる（2026-03-11 の事故）。

### 露見した問題 1: 背圧の不在
初回実行で HOST だけが netFrame 791 まで一度も配信できなかった。
ランナーは HOST を 500ms 先に起動するため HOST は約30フレーム先行しており、
CLIENT がまだ生成していないフレームを読み続けていた。
`delay + maxRollback = 6` フレーム（100ms）では 500ms の先行を吸収できない。

`writeHead - confirmedRemoteFrame > delay + maxRollback` なら書き込みを止める
背圧を追加した。ロールバック netplay で先行側を抑えるのは「入力の枯渇」であり、
その入口がこの判定。IntroBarrier 修正時に「残る課題」とした点の実装にあたる。

### 露見した問題 2: MatchInputBuffer の 2スレッド競合
背圧を入れると `ConfirmConflicts` が 80 → 1970 に増え、かつ両者で対称になった。
これにより原因が先行/追従の非対称性ではないと確定できた。

真因は、ゲームスレッドの `WriteLocal` と通信スレッドの `ConfirmRemote` および
冗長入力の読み出しが、同一スロットの同じフィールド群を非アトミックに
読み書きしていたこと。`WriteLocal` は `frame`/`valid` を先に立てて `localInput` を
後に書くため、通信スレッドが「新しいフレーム番号 + 古い入力値」を読み、
それを冗長入力として相手に配っていた。

**旧実装も同じ競合を抱えていたが、検出口が無いため誰も気づけなかった。**
2a で `ConfirmConflicts()` を作っていなければ今回も見逃していた。

対処としてレーンを分離した。

| レーン | 書く側 | 内容 |
|---|---|---|
| local | ゲームスレッドのみ | `localInput` / `predictedRemote` / `rollbackable` |
| remote | 通信スレッドのみ | `remoteInput` |

各レーンはペイロードを書いてからフレーム番号を release ストアで公開し、
読む側は acquire ロードで番号を確認してからペイロードを読む。
「番号は新しいが中身は古い」が構造的に起きない。
スロットを外部に露出する `FindSlot` は廃止し、`TryGetLocalInput` /
`TryGetRemoteInput` に置き換えた（呼び出し側が競合を再導入できないようにするため）。

### 検証の推移
| | 背圧前 | 背圧後 | レーン分離後 | ラベル修正後 |
|---|---|---|---|---|
| conflict (host/client) | 80 / 0 | 1970 / 1907 | 0 / 0 | 0 / 0 |
| 不一致フレーム | 15 | 376 | 1 | **0** |

最後の1件は計測側の不備だった。未確定フレームで直前入力を再書き込みしていたため、
記録の netFrame ラベルだけが進んで中身と対応しなくなっていた。
「未確定なら書かない」方式に変えて解消（保持用の状態変数も不要になった）。

最終結果: 共通 1,026 フレームの入力列が完全一致、`conflict` は両者 0。

### 残る挙動（設計上の必然）
HOST の `stall=177` に対し CLIENT は `stall=31`。背圧が先行側を
`confirmed + 6` に抑えるため、HOST は常に境界ぎりぎりを読むことになり、
相手のパケット到着が間に合わないフレームが出る。
これを滑らかにするのがロールバックで、現状は予測が外れても巻き戻せないため
「確定するまでゲームに渡さない」安全側の挙動になっている。
ディレイのみの netplay としては正しい。

### 変更ファイル
- [MODIFY] `sync/MatchInputBuffer.hpp` — レーン分離と公開順序の明示、`FindSlot` 廃止
- [MODIFY] `engine/SceneRunner.cpp` — (E) 入力書込み + 背圧、(F2) 確定フレームの配信
- [MODIFY] `network/SyncCodec.cpp` / `sync/NetplaySession.cpp` — 新 API へ移行
- [MODIFY] `src/harness/FakeGame.hpp/.cpp` — 記録に netFrame を追加
- [MODIFY] `src/harness/harness_main.cpp` — 決定的なスクリプト入力の注入
- [MODIFY] `src/harness/run_pair.ps1` — 記録の突き合わせによる決定性判定
- [MODIFY] `src/tests/test_input_buffers.cpp` — レーン分離の検証3件を追加

---

# refactor: フレーム空間を一本化 — MenuInputBuffer と宛先フラグを撤去 (2b)

## 2026-07-27: 入力パイプライン再構築 2b

### 概要
MENU と MATCH で別々に管理されていた2つのフレーム空間を、セッション通しの
単一フレーム空間に統合した。3月11日版の画面停止（`peerF=12551` の跨ぎ汚染）は
これで構造的に発生しなくなる。

### 撤去したもの
| 対象 | 理由 |
|---|---|
| `MenuInputBuffer`（ファイルごと削除） | 2つ目のフレーム空間そのもの |
| `FLAG_BUFFER_MENU` / `FLAG_BUFFER_MATCH` | パケットが受信側の内部データ構造を指名する設計。フェーズ認識が両者でずれた瞬間に別々のバッファへ振り分けられていた |
| `_lastSentMenuFrame` / `_lastSentMatchFrame` | `_lastSentFrame` 1本に統合 |
| writeHead 後退の検知コード | フレーム空間をリセットしないので後退しえない |
| `ConfirmRemote` の分岐ログ（毎フレーム10行） | 3月の実機ログ 118,204行の主因 |

`BuildPacket` から `bufferTargetFlag` 引数を削除した。

### 調査で判明したこと
両バッファとも `NetplaySession::Start` で frame 200 に一度初期化されるだけで、
フェーズ遷移でのリセットは既に存在しなかった（CB撤去の際に一緒に消えていた）。
つまりフレーム空間は実質すでにセッション通しで、2本に分かれていたことだけが
残骸として残っていた。`_latestPeerFrame` の単調最大値は、フレーム空間が1本で
リセットもされない以上、常に正しい意味を持つ。

### 検証
harness（ロード 60F/240F）で退行なし。

| 項目 | 結果 |
|---|---|
| IntroBarrier | HOST=WT512 / CLIENT=WT483（2b 修正を維持） |
| パケット往復 | 両者 439 受信で対称 |
| ログ量 | 4,500行 → 536行 |

実行末尾の `Peer Disconnected!` は HOST が正常終了した後に CLIENT が検出したもの。
非対称実行では HOST が 180F 早く終わるため必然であり、退行ではない。

`ctest` 4スイート全緑（`input_buffers` は Menu 分を除いて 69 → 61チェック）。

### 変更ファイル
- [DELETE] `sync/MenuInputBuffer.hpp`
- [MODIFY] `network/SyncCodec.hpp/.cpp` — 宛先フラグ撤去、確定先と冗長入力取得を一本化
- [MODIFY] `sync/NetplaySession.hpp/.cpp` — 送信経路と追跡変数を一本化
- [MODIFY] `ui/UIManager.cpp` — MenuInputBuffer への delay 反映を削除
- [MODIFY] `src/tests/test_input_buffers.cpp` — MenuInputBuffer 節を削除

---

# refactor: MatchInputBuffer を安全化 — デシンクが黙って通る経路を塞いだ (2a)

## 2026-07-27: 入力パイプライン再構築 2a

### 概要
入力パイプラインを結線する前に、土台となる `MatchInputBuffer` を安全化した。
L1 で `[HAZARD]` として記録していた危険な挙動を、検出可能な形に置き換えている。
旧実装をそのまま戻すのではなく組み直す方針に沿った最初の段階。

### 解消した [HAZARD]

| 旧挙動 | 現在の要求 |
|---|---|
| 確定済みスロットへの異なる再確定を見逃す | `ConfirmConflicts()` で計数し、ミスマッチとしても記録 |
| リング周回(600F)で別フレームのデータを黙って返す | `FindSlot` / `TryReadForGame` がフレーム番号を検証して拒否 |
| `ConfirmRemote` が `slot.frame` を更新しない | 常に `frame` と `valid` を立てる |
| ミスマッチ「なし」をフレーム0で表す | `HasMismatch()` / `ConsumeMismatch(uint32_t&)` |
| 相手確定「なし」をフレーム0で表す | `HasConfirmedRemote()` |
| フレーム0が永久に読み出せない | `valid` フラグで未書込みと区別するので読める |

冗長入力は同じフレームを何度も確定するため、値の食い違いは通信破綻かデシンクを
意味する。旧実装は黙って上書きしていた。最初の確定値を正とし、
`ConfirmConflicts()` で観測できるようにした。

### API の変更
| 旧 | 新 |
|---|---|
| `WriteSlot(frame, rollbackable, local, remote, confirmed)` | `WriteLocal(frame, local, predictedRemote, rollbackable)` |
| `GetSlot(frame)` → 参照 | `FindSlot(frame)` → ポインタ（周回時 nullptr） |
| `ReadFrameForGame(...)` | `TryReadForGame(...)` |
| `ConsumeMismatch()` → uint32_t | `ConsumeMismatch(uint32_t&)` → bool |
| `InitializeConfirmedRemoteFrame` / `ToRelativeFrame` | 削除（呼び出し元なし） |

`SyncCodec` と `NetplaySession` の冗長入力取得を `FindSlot` に移行した。
周回して別フレームになっている場合は 0 を送る（誤った過去入力を配らない）。

### MenuInputBuffer について
安全化していない。フレーム空間の一本化(2b)で撤去する予定のため、
現状の挙動を `[LEGACY]` 印で記録するにとどめた。

### 検証
`ctest` 4スイート、`input_buffers` は 43 → **69チェック**に増加。全緑。
DLL / EXE / harness ともビルド通過。

### 変更ファイル
- [MODIFY] `sync/MatchInputBuffer.hpp` — 全面書き換え
- [MODIFY] `network/SyncCodec.cpp` / `sync/NetplaySession.cpp` — `FindSlot` へ移行
- [MODIFY] `src/tests/test_input_buffers.cpp` — `[HAZARD]` を正しい挙動の検証に置換

---

# fix: IntroBarrier がロード時間差で無効化される問題を修正 — 事前通知の削除と到達のラッチ化

## 2026-07-27: 証言②「ロード時間のばらつきでずれる」の修正

### 概要
ハーネス(B-3)で再現した IntroBarrier の不成立を修正した。原因は3つあり、
いずれも「1つのフラグに複数の意味を持たせたこと」に起因する。

### 修正内容

**1. Loading 中の事前通知を削除**
`MatchScene::OnLoading` が Loading 突入時点で `localPhaseReady = true` を立てていた。
このフラグは `HandleRoundStartSync` 側で「intro=2 に到達した」の意味で待たれているため、
ロード時間が左右で違うと、まだ Loading 中の相手からの通知でバリアが解除されていた。

**2. intro=2 の到達をラッチ化（最も影響が大きい）**
`HandleRoundStartSync` は `introState` の瞬間値を見て `!= 2` なら待機していた。
しかし `GC::SetModePause()` は名前に反してゲームを止めない（`SetNormalSpeed()` と
同一実装）。相手を待つ間に自分の intro は 2→1→0 と進むため、
瞬間値判定では先に到達した側が次のラウンドまで条件を満たせなくなる。
1 だけ直すと先行側が待ち続けて事態が悪化するため、到達を `s_reachedIntro2` に
ラッチする形に変更した。

**3. `localPhaseReady` をリセット対象に追加**
`ResetInGame` は「常に送信するため」意図的に `localPhaseReady` を true のまま
維持していた（コメントに明記あり）。これでは前の対戦の到達通知が次のバリアを
素通りさせる。通知はレベル駆動なので、片側が先にリセットしても相手の次の
パケットで復帰する。`ResetLoading` でも両フラグをクリアするようにした。

### 検証（harness, ロード 60F / 240F）

| | 修正前 | 修正後 |
|---|---|---|
| HOST バリア解除 | WT=302（待たず先行） | WT=514 |
| CLIENT バリア解除 | WT=482 | WT=481 |

HOST は intro=2 到達後 212 フレーム待って CLIENT の到達を確認してから解除する。
HOST 514 / CLIENT 481 の 33 フレーム差はランナーが起動を 500ms ずらしている分
（≒30F）であり、両者は同一の実時刻で解除している。

対称ロード（60F/60F）でも退行なし。両者とも frame 300 で intro=2 に到達し、
HOST=WT335 / CLIENT=WT301 で解除、ともに frame 1262 で完走した。
なお修正前は事前通知により待機ゼロだったため、対称条件でも HOST に
30F 前後の待ちが増える。これは相手の実際の到達を待つようになった結果であり、
バリアとして正しい挙動。実対戦では両者がほぼ同時に開始するため待ちは小さい。

### 残る課題（この修正の範囲外）
ゲーム時間そのもののずれ（HOST Rematch=1142 / CLIENT=1322 の 180F 差）は解消しない。
バリアはラウンド開始の論理的な基準を揃えるものであって、先行側を実際に
待たせる仕組みではないため。先行側を止めるには入力の枯渇が必要で、
入力パイプラインの再構築を待つ。

### 変更ファイル
- [MODIFY] `engine/MatchScene.cpp` — `OnLoading` の事前通知削除、
  `HandleRoundStartSync` の到達ラッチ化、`ResetLoading`/`ResetInGame` のリセット範囲拡大

---

# feat: ヘッドレスハーネスを新設 — MBAA 無しで netplay 同期を通しで実行できるようにした

## 2026-07-27: B-3 ハーネスの構築

### 概要
`harness.exe` を新設。`FakeGame`（スクリプトされた MBAA）を `IGameMemory` として
設置し、`SceneRunner::Init/Step` を自前ループで回す。2プロセスを loopback UDP で
繋ぐと、MBAA を一切起動せずに netplay の全ライフサイクルが動く。

### 結果
2プロセス実行で以下がすべて成立した（約21秒、GUI なし）。

```
[SyncCodec] Peer READY received.
[NetplaySession] Mode -> WaitStart (peer READY received)
[Metronome] Started.
[NetplaySession] Mode -> Counting. startTime=... θ=1us startFrame=200
[SceneRunner] Sync completed! θ=1us
[SceneRunner] Phase change: 0 -> 2   (CharaSelect)
[SceneRunner] Phase change: 2 -> 3   (Loading)
[IntroBarrier] Pre-signaling during Loading phase.
[SceneRunner] Phase change: 3 -> 4   (InGame)
[IntroBarrier] Both peers at intro=2! phaseBaseFrame=0 Go!
[SceneRunner] Phase change: 4 -> 5   (Rematch)
finished at frame=1262
```

`IntroBarrier` が両プロセスで揃って発火することを、実ゲーム無しで初めて観測した。

### リンク境界
| 実コード（検証対象） | スタブ（`harness_stubs.cpp`） |
|---|---|
| SceneRunner / MatchScene / SceneInputFilter | HookLog |
| NetplaySession / NetplayClock / SyncCodec | DirectInputHook（入力注入口も兼ねる） |
| NetplayManager / PacketRouter / UdpSocket / NetworkSimulator | StateUiLogic（ImGui を引くため） |
| Metronome / WasapiClock | TimeHooks（MinHook を引くため） |
| PhaseMonitor / GameMemory | SceneFastBoot（後述） |

通信は本物の UDP。パケット組立・θ推定・メトロノーム・IntroBarrier はすべて実コード。

### 構築中に判明した2点

**1. FastBoot は seam の外なのでハーネスで落ちる**
初回実行は Segfault した。原因は B-2 で意図的に seam の外に残した
`CC_GAME_STATE_ADDR` の読み書きと `CC_SFX_ARRAY_ADDR` への 1500 バイト `memset`。
設計どおり `SceneFastBoot` をスタブに置換して解決した。
seam を通していない箇所だけが落ちたため、境界の所在が実行時に確認できた形になった。

**2. 同期前後でフレームのペースを握る主体が入れ替わる**
同期成立前は `Metronome` が停止しており `SceneRunner::Step()` は待たない。
実ゲームでは `Present` が約60fpsの外側ペースを作るが、ハーネスにはそれが無く、
ハンドシェイクの `START_MARGIN_US`(0.5秒) が経過する前に全1262フレームを
走り切って一度も接続しなかった（`synced=0`、ログ58行）。
`timeBeginPeriod(1)` + QPC のフレームリミッタを入れて解決。

### ハーネス初日の成果: 証言②「ロード時間のばらつきでずれる」を再現した

`--HostLoadingFrames 60 --ClientLoadingFrames 240` で左右のロード時間を変えたところ、
IntroBarrier が機能していないことが確認できた。

| | ロード | InGame 到達 | IntroBarrier 解除 |
|---|---|---|---|
| HOST | 60F | frame 300 | WT=302 |
| CLIENT | 240F | frame 480 | WT=482 |

HOST は CLIENT の到達を待たず 180 フレーム先行した。以降ずれたまま復帰せず、
Rematch 到達も HOST=1142 / CLIENT=1322 と 180 フレーム離れたままだった。

**原因**: `MatchScene::OnLoading` の事前通知。Loading 突入時点で
`localPhaseReady = true` を立てるため、CLIENT はまだ intro=2 に到達していないのに
「準備完了」を送信する。HOST 側の `peerPhaseReady` が真になり、バリアを素通りする。

コメントには「InGame 到達時にはバリア待機ゼロを実現」と意図が書かれているが、
ロード時間が左右で異なる場合はバリアそのものが無効化される。
`localPhaseReady` が「Loading に入った」と「intro=2 に到達した」の
2つの意味を兼ねていることが本質。

### 現時点の限界
記録された入力は **0件**。入力パイプラインが撤去済みで、ゲームに書き込む処理が
存在しないため。したがって決定性テスト(B-4)の「入力列の突き合わせ」は、
入力パイプラインを再構築するまで比較対象が空のままになる。
一方、上記のようにフェーズ遷移とバリアの検証は入力パイプライン無しで行える。

### 変更ファイル
- [NEW] `src/harness/FakeGame.hpp/.cpp` — スクリプトされた MBAA + 書込み記録
- [NEW] `src/harness/harness_main.cpp` — 引数処理・設置・フレームループ
- [NEW] `src/harness/harness_stubs.cpp` — HookLog / DirectInputHook / StateUiLogic / TimeHooks / SceneFastBoot
- [NEW] `src/harness/run_pair.ps1` — 2プロセス起動と記録の突き合わせ用ランナー
- [NEW] `src/harness/CMakeLists.txt`
- [MODIFY] `CMakeLists.txt` — `add_subdirectory(src/harness)`

---

# refactor: ゲームメモリ seam (IGameMemory) を導入 — 同期ロジックをゲーム無しで検証可能に

## 2026-07-27: B-2 seam の導入

### 概要
同期ロジックが `*CC_XXX_ADDR` を直読みしている状態を解消し、`IGameMemory`
インターフェース経由に統一した。差し替え可能になったことで、フェーズ判定を
MBAA を起動せずにテストできる。挙動は変えていない。

### 構成
| 実装 | 配置 | 内容 |
|---|---|---|
| `RealGameMemory` | `core_dll/mbaa_mem/` | 実アドレスへの読み書き。DLL 初期化時に設置 |
| `NullGameMemory` | `core_dll/mbaa_mem/GameMemory.cpp` | 未設置時の既定。読みは 0、書きは捨てる |
| `FakeGameMemory` | `src/tests/` | 値を明示指定し、書き込まれた入力を全フレーム記録 |

インターフェースは読み6 + 書き1。設計書では読み5としていたが、`UIManager` と
`GameFrameOrchestrator` にあった `IsBadReadPtr` ガードの挙動を保存するため
`IsAvailable()` を追加した。

### 移行結果
ライブ経路の `CC_*_ADDR` 直参照はすべてゼロになった。

| ファイル | 直参照 |
|---|---|
| `engine/SceneRunner.cpp` | 0 |
| `engine/MatchScene.cpp` | 0 |
| `engine/FrameControl.hpp` | 0 |
| `engine/GameFrameOrchestrator.cpp` | 0 |
| `mbaa_mem/PhaseMonitor.cpp` | 0 |
| `ui/UIManager.cpp` | 0 |

`SceneFastBoot` には `CC_FORCE_GOTO_ADDR`（コード書換）、`CC_SFX_ARRAY_ADDR`、
`CC_GAME_STATE_ADDR` が残るが、いずれも FastBoot 固有で性質が異なるため
意図的に seam の外に置いている。テストでは FastBoot 自体をスキップする。

`FrameControl` の入力書込みプリミティブ（`GetInputBasePtr` / `WriteP1Input` /
`WriteP2Input` / `LogNullInputBase`）は `RealGameMemory` に移設した。処理内容は同じ。

### 併せて修正した文書の誤り
`GamePhaseDetector.hpp` の `GetIntroState()` doc が「0=イントロ前 / 2=イントロ完了」と
実装と正反対になっていたのを修正。`IsRoundActive()` の説明も
「introState==2 かつタイマー動作中」→「InGame かつ introState==0」に訂正した。
この誤りは `test_phase_monitor.cpp` で固定したため、再発すればテストが赤くなる。

### 検証
単体テスト4スイート132チェック通過。`test_phase_monitor` は `RealGameMemory` を
リンクしていないため、直読みが残っていればクラッシュして露見する構成。

E2E（2窓70秒）を B-1 時点と比較したところ、ログの出現数と最終状態が完全に一致した。

| 項目 | B-1 | B-2 |
|---|---|---|
| `ConfirmRemote MATCH` | 12910 | 12910 |
| `RECV pkt` | 1334 | 1334 |
| `SceneRunner` 定期ログ | 65 | 65 |
| `ConfirmRemote MENU` | 10 | 10 |
| 最終状態 | `phase=2 fip=3840 WT=3885 intro=0 synced=1 alive=1` | 同一 |
| NULL / FAILED / Disconnected | 0 | 0 |

`[InitThread] RealGameMemory installed.` が両プロセスのログ7行目に出ており、
設置が他の初期化より先に行われていることも確認した。

### 変更ファイル
- [NEW] `mbaa_mem/IGameMemory.hpp` — インターフェースと設置口
- [NEW] `mbaa_mem/GameMemory.cpp` — 設置口の実装 + NullGameMemory
- [NEW] `mbaa_mem/RealGameMemory.hpp/.cpp` — 実メモリ実装
- [NEW] `src/tests/fake_game_memory.hpp` — FakeGameMemory + ScopedGameMemory
- [NEW] `src/tests/test_phase_monitor.cpp` — seam 経由の PhaseMonitor テスト（30チェック）
- [MODIFY] `engine/FrameControl.hpp` — `WriteInput` を seam に委譲、プリミティブを撤去
- [MODIFY] `engine/SceneRunner.cpp` / `MatchScene.cpp` / `SceneFastBoot.cpp` — seam 経由に
- [MODIFY] `engine/GameFrameOrchestrator.cpp` / `ui/UIManager.cpp` — `IsBadReadPtr` → `IsAvailable()`
- [MODIFY] `mbaa_mem/PhaseMonitor.cpp` — seam 経由に
- [MODIFY] `mbaa_mem/GamePhaseDetector.hpp` — doc の誤りを訂正
- [MODIFY] `mbaa_mem/dllmain.cpp` — 初期化の最初期に `InstallRealGameMemory()`
- [MODIFY] `src/core_dll/CMakeLists.txt` / `src/tests/CMakeLists.txt` — ソース追加

---

# chore: 編集時に単体テストを自動実行する PostToolUse フックを追加

## 2026-07-27: 検証ループの自動化

### 概要
`src/core_dll/` または `src/tests/` 配下を編集した直後に単体テストを自動実行し、
失敗を即座にフィードバックするフックを追加した。あわせて現行ビルドの
E2E 動作確認を行った。

### フックの構成
- `.claude/settings.json` — `PostToolUse` / matcher `Write|Edit`、timeout 120秒
- `.claude/hooks/ctest.sh` — 本体
  - 編集ファイルのパスを見て、対象外なら即 `exit 0`（他プロジェクトの編集では何もしない）
  - テスト実行ファイルのみビルド（DLL 本体はビルドしないので数秒で終わる）
  - `ctest` 実行。ビルドまたはテストが失敗したら `exit 2` で stderr を返す
  - この環境に `jq` が無いため、stdin の JSON は `grep`/`sed` で読む

`dual_test.bat` はフックにしていない。1回80秒かかりゲームウィンドウが2つ起動するため、
編集のたびに走らせるのは現実的でない。E2E は区切りごとに手動で回す。

### 検証
故意にアサートを1つ壊してフックを実行し、`exit=2` で失敗テスト名・ファイル・行番号が
返ることを確認したうえで復旧した。対象外パス（別プロジェクト）で即 `exit 0` することも確認済み。

### 現行ビルドの E2E 結果（参考）
デプロイ後に2窓テストを70秒実行した結果:

| 確認項目 | 結果 |
|---|---|
| 接続・時刻同期 | 成立（θ=1μs、`Mode -> Counting`） |
| 画面停止・高速動作 | **発生せず** — 3840フレーム/約64秒 = 60fps |
| Peer 切断 | なし（70秒間 `alive=1`） |
| 進行 | CharaSelect で停止（入力パイプライン撤去のため期待どおり） |

3月11日版で観測された暴走（`peerF=12551` によるもの）は再現しなかった。

### 変更ファイル
- [NEW] `.claude/settings.json` — PostToolUse フック定義
- [NEW] `.claude/hooks/ctest.sh` — ビルド + ctest 実行スクリプト

---

# docs: InGame 画面停止の原因を特定 — peerFrame のフェーズ跨ぎ汚染を記録

## 2026-07-27: dual_test.bat 実行ログの解析

### 概要
`dual_test.bat` を実行して得た症状「対戦開始で画面が止まり、音楽だけ鳴り、
裏で高速動作する」の原因をログから特定し、設計書に必須修正項目として記録した。
コード変更は伴わない。

### 実行されたバイナリについて
`dual_test.bat` はデプロイを行わないため、実行された DLL は 2026-03-11 04:21 の
ビルド（CB撤去前）だった。したがって本ログは B-1 の検証結果ではなく、
凍結時点の症状の観測データである。

### 特定した因果連鎖
1. CharaSelect 中に MENU 用フレームカウンタが 12551 まで進む
2. `SyncCodec::_latestPeerFrame` は受信 baseFrame の単調最大値で、
   フェーズ遷移ではリセットされない（`Reset()` は `Initialize()` からのみ）
3. InGame 突入で MatchInputBuffer は 0 にリセット、MATCH パケットの baseFrame も 0 から
4. `GetLatestPeerFrame()` は 12551 を返し続ける
5. `gap = 12551 - 0` → `skipWait=true` でメトロノーム待機スキップ（高速化）
6. `SetRenderSkipByGap(12551)` → `OnPresentSkip()` が `Present()` をスキップ（画面停止）
7. 音声は D3D と無関係なので鳴り続ける

証拠: `peerF=12551` が InGame 最初のログ行から最後まで一定。
ログ末尾 `whMatch=12360` は解除直前の状態。

### 「最終ラウンドが終わらない」について
InGame 中の入力は全フレーム 0（非ゼロ入力2130件はすべて CharaSelect 中の操作）。
両者棒立ちのためラウンド1・2とも 5513 フレームちょうどでタイムオーバーし、
決着がつかず永久にループする。ヘッドレステストの必然でありバグではない。

### 現状の影響
CB撤去により gap 計算が消え `SetRenderSkipByGap(0)` 固定になっているため、
現在のツリーではこの症状は再現しない。ただし原因は `SyncCodec` に残存しており、
入力パイプライン再構築で gap 制御を戻した時点で再発する。

### 変更ファイル
- [MODIFY] `docs/design/core_dll/GameMemory_Seam.md` — §5 に必須修正項目として追記
- [MODIFY] `AGENTS.md` — `dual_test.bat` がデプロイしない罠を追記
- [ADD] `build_logs/2026-07-27_dual_test/` — 実行ログ2本と 3/11 版 DLL を保全（gitignore対象）

---

# fix: 入力を GameInput 型に統一 — Rematch 自動ナビが方向をボタンとして書く不具合を解消

## 2026-07-27: B-1 入力の型付け

### 概要
入力を生の `uint32_t` で持ち回るのをやめ、`GameInput { direction, buttons }` 型に統一。
符号化の混在によって Rematch の自動ナビが機能していなかった問題を、
型で表現できないようにすることで解消した。seam 導入（B-2）の前提となる変更。

### 直した不具合
`MatchScene::HandleAutoNavigation()` は「下」を `0x0002`、「上」を `0x0001` として返し、
呼び出し側がシフトせず `GC::WriteInput()` に渡していた。
`FrameControl::WriteInput` は `direction << 16 | buttons` を期待するため:

| 意図 | 実際に書かれていた値 |
|---|---|
| 下 (direction 2) | direction=0, buttons=`0x0002` = `CC_PLAYER_FACING` |
| 上 (direction 8) | direction=0, buttons=`0x0001` = `CC_BUTTON_START` |
| 決定 | `0x0410` — 正しく動作していた |

カーソルが動かず、その場の項目を確定していた。双方が別項目を選ぶため画面がずれる。
`SceneFastBoot` は同じ規約を正しく実装しており（`dirBits << 16`）、
どちらが規約かの判断材料になった。

### 混在していた3つの符号化
| # | 符号化 | 状態 |
|---|---|---|
| 1 | `direction << 16 \| buttons`（テンキー表記） | 実際の規約。`GameInput::Pack()` に一本化 |
| 2 | `BIT_UP=0x01 / BIT_DOWN=0x02` のビットマスク | 削除（Rematch がこの前提で書かれていた） |
| 3 | `COMBINE_INPUT` = `direction \| buttons << 8` | 削除（使用箇所ゼロ） |

### 変更ファイル
- [NEW] `mbaa_mem/GameInput.hpp` — `GameInput` 型 + `Dir::` テンキー定数 + `Pack`/`Unpack`
- [NEW] `src/tests/test_game_input.cpp` — 符号化と Rematch ナビの回帰テスト（23チェック）
- [MODIFY] `engine/FrameControl.hpp` — `WriteInput` / `ClearInput` を `GameInput` 経由に
- [MODIFY] `engine/MatchScene.cpp` — `HandleAutoNavigation` の戻り値を `GameInput` に。
  `ResolveMenuSelection` は `.buttons` を参照。`HandleMenuGate` の未使用引数を削除
- [MODIFY] `engine/SceneFastBoot.cpp` — 4箇所の `WriteInput` を型経由に
- [MODIFY] `mbaa_mem/MbaaInputDefs.hpp` — `BIT_*` / `COMBINE_INPUT` / `RETURN_MASH_INPUT` 削除
- [MODIFY] `src/tests/CMakeLists.txt` — `test_game_input` 追加
- [NEW] `docs/design/core_dll/GameMemory_Seam.md` — seam 設計書（B-1〜B-4 の段取り）

### 検証状況
`ctest` 3スイート98チェック通過、DLL/EXE ともビルド成功。
ただし**実ゲームでの確認は未実施**。Rematch の修正が実際に効くことは
ハーネス（B-3）が立つまで確認できない。現時点の主張は「型として正しくなった」まで。

---

# test: 依存ゼロの単体テスト基盤を新設 — 入力バッファと NetplayClock の挙動を固定

## 2026-07-27: L1 テスト基盤の構築

### 概要
入力パイプライン再構築の前段として、ゲームを起動せずに実行できる単体テスト基盤を
新設した。`ctest` を有効化し、依存ゼロで検証できる2モジュールの現在の挙動を
特性化テストとして固定。全75チェックが通過。

### 変更理由
凍結の直接原因は個別バグではなく「仮説を検証する手段が実ゲーム2窓の35秒目視しか
なかったこと」。修正の正否を確認できないまま次を書く状態を先に解消する。

### テスト方針
- 外部フレームワークを導入しない（mingw32 環境で依存を増やさない）。
  `test_support.hpp` に最小のアサートマクロを置き、外部シンボルは `stub_*.cpp` で置換
- ケース名が `[HAZARD]` で始まるものは**現在の危険な挙動をそのまま固定**したもの。
  緑であることは「正しい」ではなく「変わっていない」を意味する

### 固定した挙動のうち危険なもの
- `MatchInputBuffer`: 確定済みスロットへの再確定はミスマッチとして検出されない
  （冗長入力は毎パケット再送されるため、デシンクが黙って通過しうる）
- `MatchInputBuffer`: `RING_SIZE`(600F) 周回時にフレーム番号を検証せず別フレームを返す
- `MatchInputBuffer`: `ConfirmRemote` が `slot.frame` を更新しない
- `MatchInputBuffer` / `NetplayClock`: 「なし」をセンチネル 0 で表すため
  フレーム0のミスマッチ・開始時刻0が「未設定」と区別できない
- `NetplayClock`: RTT 同値ではθを新しいサンプルに乗り換えない（strict less-than）
- `NetplayClock`: `SetPeerStartTime` のθ変換は呼出し時点で固定され、後から再計算されない
- `NetplayClock`: `Reset()` が `_baselineTheta` を消さない

### 変更ファイル
- [NEW] `src/tests/test_support.hpp` — 依存ゼロの最小テストハーネス
- [NEW] `src/tests/test_input_buffers.cpp` — MatchInputBuffer / MenuInputBuffer（43チェック）
- [NEW] `src/tests/test_netplay_clock.cpp` — θ推定・α補正・開始時刻合意（32チェック）
- [NEW] `src/tests/stub_wasapi_clock.cpp` — WasapiClock::GetTimeUs() の置換スタブ
- [MODIFY] `src/tests/CMakeLists.txt` — 2ターゲット追加 + `add_test` 登録
- [MODIFY] `CMakeLists.txt` — `enable_testing()` 追加
- [MODIFY] `AGENTS.md` — テスト実行方法と `[HAZARD]` の扱いを追記

---

# docs: AI向け指示ファイルを落とし穴ベースに再構成 — AGENTS.md 圧縮とガイド統廃合

## 2026-07-27: 指示ファイルの再構成

### 概要
`AGENTS.md` (9KB) と `AI_WORKSPACE_GUIDE.md` (19KB) の二重管理を解消し、
AI 向け指示を `AGENTS.md` 単一ファイル (4.3KB) に集約。内容も「コードを読めば
わかること」を全削除し、「コードを読んでもわからないこと」だけを残す方針に転換。

### 変更理由
- 両ファイルが揃って「最初に読め」と主張し、指示が競合していた
- `AI_WORKSPACE_GUIDE.md` のディレクトリツリーが実在しない構成
  (`src/app/`, `domain_netplay/`, `domain_scene/`, `src/cli/`) を説明しており、
  参照した AI が誤った前提で作業を始める状態だった
- ディレクトリ別の「規範/禁止」9セクションは、コードの配置を見れば導ける内容だった

### AGENTS.md に残した内容（プロジェクト固有の暗黙知）
- 凍結時点の状態 — 入力パイプラインは意図的に撤去済みで、未実装であってバグではない旨
- `SceneRunner::Step()` でのブロック禁止 — キープアライブが不正化し `Peer Disconnected` に至る
- `CC_INTRO_STATE_ADDR` の値の向き (2=紹介中 / 1=pre-game / 0=in-game)
- `CC_SKIP_FRAMES_ADDR` 使用禁止
- フォルダ名と namespace の対応表（過去のリネームで乖離）
- mingw32 (32bit) 必須 — mingw64 では DLL 生成に成功したうえで注入だけ失敗する
- `docs/` は現行仕様ではなく経緯の記録である旨、`tools/` (DummyPeer) の消失
- `.agents/` およびルート直下の `*.txt` / `*.log` / `*.exe` が gitignore 対象である旨

### 変更ファイル
- [MODIFY] `AGENTS.md` — 全面書き換え（9KB → 4.3KB）
- [NEW] `CLAUDE.md` — `@AGENTS.md` の1行のみ（内容の重複を作らない）
- [MOVE] `AI_WORKSPACE_GUIDE.md` → `docs/archive/AI_WORKSPACE_GUIDE_2026-03.md`
- [MODIFY] `README.md` — 実在しない `tools/` の記述を削除、`src/cli_launcher` に修正、
  `docs/` が現行仕様と乖離している旨を追記
- [MODIFY] `.agents/workflows/build.md` — デプロイ先を実在する `MBAACC_1` / `MBAACC_2`
  の2窓構成に修正（gitignore 対象のため本コミットには含まれない）
