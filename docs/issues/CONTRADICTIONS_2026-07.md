# コード内の矛盾一覧（2026-07-27 調査）

`src/` 配下 128 ファイルを走査し、**実装と食い違う記述**を根拠付きで洗い出した結果。
このプロジェクトは「ドキュメントを信じた作業者が誤った修正をする」ことで一度凍結して
いるため、記述の誤りは実害として扱う。

判定の材料にしたもの: 単体テストが固定している挙動、他の使用箇所との整合、git 履歴
（どのコメントが改名・撤去前の残骸か）、実機トレースの実測値。

## 対処の方針
**記述を直すのではなく、間違いを選べなくする。** コメントで「〜してはならない」と
書いても次の作業者は読まない。選択肢そのものを消すか、型で表現できないようにする。

## 解消済み（2026-07-27）
| # | 対処 |
|---|---|
| 1 | `MatchScene` の嘘の冒頭コメントを**削除**（直さずに消した） |
| 2 | `MatchScene::IsDrivingInput()` を追加し、自動ナビ中は (F2) が書かないようにした |
| 5 | 呼び出し元の記述を hpp / dllmain から**削除**。実装1箇所を参照させる |
| 6 | `SetModePause()` を**削除**。速度モードは 2 つに（4→2） |
| 7 | `FastBootRunner.*` を**削除**（`SceneFastBoot` と競合しうる裏スレッド） |
| 8 | `FrameInputSlot.hpp` / `FrameSyncState.hpp` を**削除**（`GamePhase` と二重定義） |
| 9 | `rollback/` を**ビルド対象から除外**。16bit 入力前提で現行と非互換。ファイルは残す（ステート保存アドレスに参照価値があるため） |
| 10 | `Poll()` の呼び出しを**1フレーム1回**に。エッジ検出が動くようになる |
| 12 | `SceneRunner.cpp` の重複したフロー一覧を**削除**。本文が唯一の真実 |
| 18 | 受信ループの `inputCount` を 10 にクランプ。**バッファオーバーランの修正** |
| 低 | `SetModeRollupSkip` / `ClearInput` を削除 |

---

## 最高

| # | 内容 | 正しいのは |
|---|---|---|
| 1 | `MatchScene.hpp:9-13` / `.cpp:4-7` が入力パイプラインを「実装済み」と書くが、`OnCharaSelect` / `OnLoading` は空。`ReadBufferAndWrite` は撤去済み。hpp:12 の「Intro同期(0→1遷移)」も実装（intro=2 到達ラッチ）と別物 | 実装 |
| 2 | **`MatchScene::OnRematch()` の自動ナビ入力が同フレーム内で必ず上書きされる。** `SceneRunner::Step()` は (F) で `OnRematch()` を呼んだ直後、(F2) で `TryReadForGame` → `WriteInput` を無条件実行する | (F2) が最後に書く＝自動ナビは効かない |
| 3 | `TimeHooks.hpp:11-20` は「Rollup/FastBoot 用の一時的な高速化」と書くが、`dllmain.cpp:221-222` が起動時に `SetTimeMultiplier(1000)` / `SetSleepBypass(true)` を**恒久設定**。1 に戻すコードが存在しない | 実装（常時1000倍速） |
| 4 | `NetplaySession.cpp:166,179` のコメント「14ms Sleep + 残りCPUスピン」は二重に誤り。実装は `Sleep(1)` ループで、それが `Hooked_Sleep` により `pOrigSleep(0)` に化ける＝**完全なビジースピン** | 実装（2スレッドが常時 CPU 100%） |
| 5 | `SceneRunner::Step()` の呼び出し元が3箇所で食い違う。hpp:7「Hooked_EndScene」/ cpp:6「Hooked_Present」/ dllmain:248「Hooked_EndScene」。EndScene は **1フレームに約8回**呼ばれる | cpp（Present） |

---

## 高

| # | 内容 | 正しいのは |
|---|---|---|
| 6 | `FrameControl::SetModePause()` は名前と doc に反してゲームを止めない（`SetNormalSpeed()` と同一） | 実装。`MatchScene.cpp:82-85` に実測注記あり |
| 7 | `mbaa_mem/FastBootRunner.*` は完全なデッドコードだが CMake でビルドされ続けている。doc の「キャラセレ到達で自動停止」も嘘（`Stop()` の呼び出し元なし） | 実際に使うのは `SceneFastBoot` |
| 8 | `sync/FrameInputSlot.hpp` / `FrameSyncState.hpp` は孤立デッドコード。`FrameSyncState`(8値) が `GamePhase`(6値) と同じ概念を別粒度で二重定義 | `GamePhase` |
| 9 | `rollback/` 一式が **16bit 入力前提**（`InputEntry{uint16_t local, remote}`）。現行は 32bit (`direction<<16\|buttons`)。**4つ目の非互換な符号化**。`StateBuffer` の doc「128フレーム」「古い情報を破棄」も実装と不一致 | 現行の 32bit |
| 10 | **`DirectInputHook::Poll()` が1フレームに2〜3回呼ばれ、エッジ検出が原理的に動かない。** `Poll()` は毎回 `prevState = state` するため、2回連続で呼ぶと差分が消える。`GetActiveDeviceDirection` / `GetAnyInputEdge` が常に空振り | hpp の「毎フレーム1回」 |
| 11 | `DirectInputHook.cpp:165-180` が `CC_BUTTON_*` 11個を独自に再定義（`MbaaInputDefs.hpp` を include していない）。片方を直しても他方は黙って古いまま | `MbaaInputDefs.hpp` が一元管理のはず |

---

## 中

| # | 内容 |
|---|---|
| 12 | `SceneRunner.cpp` のヘッダのフロー (A)-(I) と本文のセクション記号が不一致。**(F2) と (G) が2箇所ずつ**あり、片方は入力書込み、片方はログ |
| 13 | `NetplaySession.hpp:16`「DLLスレッドは FrameInputBuffer を監視するだけ」は逆。ゲームスレッドが書き、通信スレッドが監視する |
| 14 | `needKeepalive` のコメント「CB書込みしない Phase で true」は誤り。実装は無条件に毎フレーム true |
| 15 | ファイル `GamePhaseDetector.hpp` / クラス `PhaseMonitor` / 冒頭コメント `GameMonitor.cpp` の三重不一致。どの名前で grep しても全体像が出ない |
| 16 | 7ファイルが実在しないクラス名 `FrameInputBuffer` を参照（実体は `MatchInputBuffer`） |
| 17 | `UiPhase::Rematch` は生成されず `Rematch_Ui_View` は到達不能。表示内容もメニュー定義と不一致 |
| 18 | **ロールバック定数が3系統で矛盾。** `NUM_INPUTS=30` / `MAX_ROLLBACK=15` / `NUM_ROLLBACK_STATES=60or256` はいずれも未使用で、実際は 10 / 4 / 15。かつ受信ループが `inputCount` を無検証で使うため `inputCount>10` のパケットで `inputs[10]` を**オーバーラン** |
| 19 | `MbaaPatcher.hpp` は3つのパッチと書くが実装は5つ適用。追加2つは**コメント一切なし**で用途不明 |
| 20 | `readPos = writeHead - (delay+rollback)` のコメントに下限クランプの記載なし。かつ対戦中の delay 変更で `readPos` がジャンプし、フレームがスキップ/再配信される |

---

## 低（空実装・未使用フィールド・細かい齟齬）

`StartMappingPlayer1/2` が空関数、`SetTestModeEnabled` が実DLLで未使用、
`SetModeRollupSkip` の引数が無視、`ClearInput` 呼び出し元0件、
`HandleMenuGate` のメニュー制限が空、`test_overlay` が `add_test` 未登録。

未使用フィールド: `remoteInputs[20]` / `currentFrame` / `phaseBaseFrame`（常に0）/
`peerPhaseBaseFrame`（読み手なし）/ `charaSelectSyncDone` / `fastBoot` /
`rollbackReady` / `phaseBaseWorldTimer`（代入とログのみ）。

`GLOSSARY.md` の誤り: マジックナンバーを `"CCTR"` と記載（実際は `"CC10"`）、
パケット最大51バイト（実際は105バイト）、「FastBoot」が DLL 注入と
メニュー自動遷移の**2つの別物を指している**。

---

## 要確認（実機観察が必要）

1. **入力の方向フィールド幅** — `RealGameMemory.cpp:66` は `uint32_t`(4バイト)、
   `FastBootRunner.cpp:129` は `uint16_t`(2バイト) で同じオフセット `+0x18` に書く。
   4バイト書きが `+0x1A..0x1B` を巻き込んでいる可能性
2. `HandleMenuGate()` のカウンタ判定 `+1` が意図的なマージンか off-by-one か
3. `Metronome::WaitForNextTick()` に catch-up 上限がなく、長時間遅延後にフリーラン
   する可能性。クランプで吸収する設計なのか未実装なのか doc に記載なし
4. Rematch のメニュー決定が `max(local, remote)` である理由（片方が「もう1回」で
   片方が「リプレイ保存」ならリプレイ保存が勝つ）が旧仕様と一致するか
