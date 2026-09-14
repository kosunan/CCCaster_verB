> **旧監査資料（2026-08-13）**。現行仕様は [CURRENT_STATE](../../CURRENT_STATE.md) を参照。本文の「現行」「未実装」は当時の状態。

# 03. ゲームメモリ層 — MBAA の内部状態への読み書き

対象コード: `src/core_dll/mbaa_mem/` + `src/core_dll/engine/SceneFastBoot.cpp`。
本章の記述はすべて 2026-08 時点の実装コードを根拠とする。`docs/design/` の旧資料は現ツリーと一致しないため参照していない。

---

## 責務

MBAACC Ver.1.07 Rev.1.4.0 のプロセスメモリを、上位層（フェーズ判定・入力配信・観測）が使える形に翻訳する。具体的には4つ。

1. **読み取り** — gameMode / introState / WorldTimer / RealTimer / menuStateCounter を毎フレーム供給する
2. **書き込み** — 確定した P1/P2 の入力をゲームの入力バッファに置く
3. **恒久パッチ** — ゲーム自身の入力読み取り・非アクティブ停止を起動時に潰す
4. **観測** — デシンク判定用の生値をフレーム単位で記録する

このうち **1 と 2 だけが `IGameMemory` seam の内側**にある。3 と 4、および FastBoot は seam の外で直接アドレスを触る。

---

## 現状の構造

### 主要コンポーネント

| ファイル | namespace | 役割 | seam |
|---|---|---|---|
| `MbaaAddresses.hpp` | （マクロ） | 全アドレス・モードID定数。約130個 | — |
| `MbaaInputDefs.hpp` | （マクロ） | 入力書込み先ポインタ・オフセット・ボタンビット | — |
| `GameInput.hpp` | `game_interface` | 1P分の入力型。`direction`(uint16, テンキー表記) + `buttons`(uint16) | — |
| `IGameMemory.hpp` | `game_interface` | 読み書き口（純粋仮想6メソッド）+ 設置関数 | 定義 |
| `GameMemory.cpp` | `game_interface` | 設置口の実装。未設置時は `NullGameMemory`（全read 0 / write 破棄） | 内 |
| `RealGameMemory.*` | `game_interface` | 実アドレス実装。DLL のみ | 内 |
| `GamePhaseDetector.hpp` / `PhaseMonitor.cpp` | `game_interface` | gameMode → `GamePhase` 変換。全 static、状態を持たない | 内（seam 経由で読む） |
| `GamePhase.hpp` | `game_interface` | `Unknown/MainMenu/CharaSelect/Loading/InGame/Rematch` | — |
| `MbaaPatcher.*` | `game_memory` | 起動時1回の恒久パッチ | **外** |
| `MemoryPatcher.hpp` | `core::memory` | VirtualProtect ラッパ。アドレス知識を持たない汎用層 | — |
| `MbaaMemTrace.*` | `game_memory` | `CCCASTER_MEM_TRACE=1` で毎フレーム17値をログ | **外** |
| `engine/SceneFastBoot.*` | `domain::scene` | タイトル→キャラセレの自動遷移 | **一部外** |

`GamePhaseDetector.hpp` / `PhaseMonitor` / `PhaseMonitor.cpp`（先頭コメントは `GameMonitor.cpp`）は同一概念に3つの名前が付いている。grep のとりこぼしに注意。

### 処理の流れ

```
DLL InitThread (dllmain.cpp:241)
  └ InstallRealGameMemory()      ← 他の初期化より先。未設置だと全read が 0 になるため
  └ MbaaPatcher::ApplyStartupPatches()  (:248)   恒久パッチ

毎フレーム (Hooked_Present → SceneRunner::Step)
  (A) PhaseMonitor::GetCurrentPhase()   → GameMem().GameMode()
  (C) phase < CharaSelect なら SceneFastBoot::ProcessFrame() → 早期 return
  (E) 入力生成 → SceneInputFilter → MatchInputBuffer
  (F) MatchScene（IntroState / MenuStateCounter / WorldTimer を読む）
  (F2) MatchInputBuffer::TryReadForGame → GC::WriteInput → GameMem().WriteInput()
  末尾 MbaaMemTrace::Sample(writeHead)   ← seam を通さず直接読む
```

読みは `GameMem()` の仮想呼び出し1回（1-2ns）を経由する。毎フレーム十数回でも 16.7ms の 0.001% 未満で、コストは論点にならない。

### 他領域との境界

| 相手 | 境界 |
|---|---|
| 01 プロセス | `InstallRealGameMemory()` の呼び出し位置と、パッチ適用順序を決めるのは 01 |
| 02 フック | `Present`/`EndScene` フックが本層の呼び出し契機。`WorldTimer` と `Present` が 1:1 であるという実測（後述）は 02 の性質に依存する |
| 04 フレーム空間と時間 | `WorldTimer()` の供給元。**本層は値を返すだけで、番号付けの意味は与えていない** |
| 05 入力パイプライン | `WriteInput(GameInput, GameInput)` が最終出口。`GameInput` 型の所有は本層 |
| 06 ネットワーク | 直接の依存なし。`NetplaySession.cpp:228` が `WorldTimer()` をログ目的で1箇所読むのみ |
| 07 セッション状態機械 | `PhaseMonitor::GetCurrentPhase()` / `IntroState()` / `MenuStateCounter()` が判定材料 |
| 08 ロールバック | `rollback/DumpEntryList.hpp` が本層の未使用アドレス群を参照するが、CMake 除外でビルドされていない |
| 09 UI | `GameFrameOrchestrator.cpp:174-181` が `GameMode()` を UiPhase に変換 |
| 10 検証基盤 | `FakeGameMemory`(tests) / `FakeGame`(harness) が seam の差し替え先。`SceneFastBoot`・`MbaaMemTrace` は `harness_stubs.cpp` で丸ごと置換 |

---

## 現状の問題

### 1. seam の外に3つの穴が開いており、そこはテストできない

`IGameMemory` の設計意図は明快で、ヘッダにも書かれている — 同期ロジックが `*CC_XXX_ADDR` を直接触っている限り MBAA を起動しないと一行も検証できない。実際 `PhaseMonitor` を seam 経由にしたことで `test_phase_monitor.cpp` がゲーム無しで回るようになった。

しかし seam の外に残ったものがある。

| 場所 | 触るアドレス | テスト可否 |
|---|---|---|
| `SceneFastBoot.cpp:116,118` | `CC_GAME_STATE_ADDR` 読み書き | 不可（stub 置換） |
| `SceneFastBoot.cpp:122` | `CC_SFX_ARRAY_ADDR` 1500B ゼロ埋め | 不可 |
| `SceneFastBoot.cpp:181-183` | `CC_FORCE_GOTO_ADDR` コード書換 | 不可 |
| `MbaaPatcher.cpp` 全体 | 13箇所のコード/データ書換 | 不可（呼び出しごと消す以外にない） |
| `MbaaMemTrace.cpp:46-64` | 16アドレスの読み取り | 不可（stub で no-op） |

`MbaaMemTrace` を外に置いた判断には理由がある（観測用の値を seam に足すと本来の責務がぼやける）が、**その結果「デシンク検出の指標を計算する部分」がテスト不能になっている**。`SceneFastBoot` は理由が弱い。`GameState()` のアクセサを1つ足せば seam 内に入り、メニュー遷移ロジック全体がテスト可能になる。

推測: `SceneFastBoot` が seam 外に残ったのは、`CC_FORCE_GOTO_ADDR` のコード書換（seam に入れるべきでないもの）と同じ関数の中にあったため、まとめて外に置かれただけ。

### 2. `SceneFastBoot::IsComplete()` は永久に false になる

```
SceneRunner.cpp:144   if (phase < GamePhase::CharaSelect && !SceneFastBoot::IsComplete())
SceneFastBoot.cpp:95  if (gameMode == CC_GAME_MODE_CHARA_SELECT) { s_complete = true; ... }
```

`s_complete` を true にできるのは gameMode が CharaSelect のときだけだが、その瞬間 `phase == GamePhase::CharaSelect` になるので **:144 のゲートが偽になり `ProcessFrame()` が呼ばれない**。よって `s_complete` は永久に false、`SceneFastBoot.cpp:99` の `SetModeNormalSpeed()` も `[FastBoot] ★ CharaSelect reached!` ログも一度も出ない。実機ログにこの行が無いことと整合する。

前進方向では実害が無い（ゲートの `phase < CharaSelect` 側が代わりに効き、通常速度復帰は `MatchScene.cpp:81,107` が行う）。危険なのは**戻り方向**である。対戦中に phase が CharaSelect 未満に落ちると FastBoot が再起動し、決定ボタンを偽造し始める。そして phase が CharaSelect 未満に落ちる条件は次項のとおり広い。

### 3. `GamePhase::Unknown` が「未知」ではなく「FastBoot 対象」として使われている

`PhaseMonitor::GetCurrentPhase()` がマップしているのは 8 個の gameMode だけで、残りはすべて `Unknown` になる。

| gameMode | 値 | → GamePhase |
|---|---|---|
| `STARTUP` | 65535 | MainMenu |
| `TITLE` | 2 | MainMenu |
| `CHARA_SELECT` | 20 | CharaSelect |
| `LOADING` | 8 | Loading |
| `LOADING_DEMO` | 13 | Loading |
| `IN_GAME` | 1 | InGame |
| `REPLAY` | 26 | InGame |
| `RETRY` | 5 | Rematch |
| **`MAIN`（メインメニュー）** | **25** | **Unknown** |
| **`OPENING`** | **3** | **Unknown** |
| **`HIGH_SCORES`** | **11** | **Unknown** |
| 上記以外すべて | — | Unknown |

`GamePhase::Unknown` は enum の先頭（値 0）なので `Unknown < CharaSelect` が成り立つ。つまり **`Unknown` は「FastBoot を走らせる」という意味を持ってしまっている**。gameMode 25（本物のメインメニュー）と 3（オープニング）が Unknown なのは偶然この用途に合致しているが、意図された設計ではない — `SceneFastBoot.cpp:128,168` はこれらをリテラル `25` / `2 or 3` で直接判定しており、`CC_GAME_MODE_MAIN` 定数を使っていない。

副作用は3つ。

- 一瞬でも未知の gameMode を通ると、その1フレームは `SceneRunner.cpp:151` で早期 return し、**(E) の入力生成も (F2) のゲーム書込みも丸ごと飛ぶ**。一方 WorldTimer は進む（後述の実測前提）ので、netFrame とゲームフレームの対応が1つずれる
- `SceneInputFilter::Apply()` は `Unknown` を `default:` で素通しする。フェーズ固有の封印が外れる
- 対戦後にメニューへ戻ると FastBoot が入力偽造を再開する（項目2）

### 4. `RealGameMemory::WriteInput` — 無検証・幅不一致（AUDIT A-7）

```
char* base = *(char**)0x76E6AC;      // CC_PTR_TO_WRITE_INPUT_ADDR
if (!base) { ログ; return; }
*(uint32_t*)(base + 0x18) = p1.direction;   // ★ uint16 の値を 4 バイト書いている
*(uint16_t*)(base + 0x24) = p1.buttons;
*(uint32_t*)(base + 0x2C) = p2.direction;   // ★ 同上
*(uint16_t*)(base + 0x38) = p2.buttons;
```

問題は3点。

- **検証が NULL チェックだけ**。ゲームが解放済み／未初期化のポインタを置いていれば `base + 0x3A` まで盲目的に書く。SEH も `IsBadWritePtr` もない。DLL アンロード時やプロセス終了間際にここを踏むとクラッシュする
- **`direction` は `GameInput` 上 uint16 なのに 4 バイト書いている**。`+0x1A..0x1B`（P2 は `+0x2E..0x2F`）を巻き込んでゼロクリアしている。それが未使用パディングである保証はどこにもない。AUDIT 5.5 が実機確認項目に挙げたまま未解決
- **`IsAvailable()` は `CC_GAME_MODE_ADDR` の読み取り可否しか見ていない**。書込み先の生存とは無関係で、`WriteInput` の事前条件になっていない

### 5. `MbaaPatcher` — 未文書のパッチと、矛盾するジャンプ

`MbaaPatcher.hpp` は「適用するパッチ」を3つと書くが、実装は5群を当てる。

| # | アドレス | 書くもの | 目的 | コメント |
|---|---|---|---|---|
| 1 | `0x41F098`(2), `0x41F0A0`(3), `0x4A024E`(2), `0x4A027F`(3), `0x4A0291`(3), `0x4A02A2`(3), `0x4A02B4`(3), `0x4A02E9`(2), `0x4A02F2`(3) | `0x90` NOP | ゲーム内蔵の入力読取り・バッファクリアループを潰す | あり |
| 2 | `0x54D2C0` (20 B) | `0x00` | キーボードマップ無効化。物理キーボードを遮断 | あり |
| 3 | `CC_AUTO_ACTIVATE_ADDR` = `0x40E0C0` (11 B) | `0x90` NOP | 非アクティブ時の一時停止を無効化（裏画面でも進行） | あり |
| **4-1** | `0x4A1D42` | `EB 0E` | **不明** | **なし** |
| **4-2** | `0x4A1D4A` | `EB` (1 バイトのみ) | **不明** | **なし** |

**4-1 と 4-2 は矛盾している**（AUDIT 🟡 / `MbaaPatcher.cpp:49-53`）。

`EB 0E` は「次命令（`0x4A1D44`）から +0x0E」＝ `0x4A1D52` への無条件ショートジャンプ。つまり `0x4A1D44`〜`0x4A1D51` は実行されない。**4-2 が書き換える `0x4A1D4A` はこの区間の内側**にある。

- 4-1 が正しいなら 4-2 は到達不能なコードを書き換えているだけで無意味
- 4-2 が正しいなら 4-1 のジャンプ幅が過大で、意図した命令を飛ばしている
- さらに 4-2 は**オペコード1バイトだけ**を `EB` に差し替えており、続くバイトが偶然ジャンプ変位として解釈される。元命令の長さを確認せずに書いた形跡がある

どちらが意図かはコメントも changelog も無く判断できない。**逆アセンブルなしにこの2つを触ってはいけない**（AUDIT 5.4 の実機確認項目）。

加えて `MemoryPatcher` の全メソッドは `VirtualProtect` 失敗時に `false` を返すが、**`MbaaPatcher` は全戻り値を捨て、無条件に `[MbaaPatcher] 起動パッチ適用完了` をログする**（`:55`）。パッチが1つも当たっていなくてもログは成功を示す。

キーボードマップのアドレスも整合していない。`MbaaAddresses.hpp:56` に `CC_KEYBOARD_CONFIG_OFFSET = 0x14D2C0`（イメージベース 0x400000 からのオフセット）があるのに、`MbaaPatcher.cpp:40` はローカルの `constexpr uintptr_t KEYBOARD_MAP_ADDR = 0x54D2C0` を別に定義している。値は一致するが、定数が二重化している。

### 6. アドレス定数の 8 割が未使用

`MbaaAddresses.hpp` + `MbaaInputDefs.hpp` に約130個の定数があるが、**ビルド対象コードが実際に読み書きするのは約29個（22%）**（AUDIT 🟡）。以下が実使用の全量である。

**seam の内側（`RealGameMemory.cpp`）**

| 定数 | アドレス | 型 | R/W | 用途 |
|---|---|---|---|---|
| `CC_GAME_MODE_ADDR` | `0x54EEE8` | u32 | R | `GameMode()` / `IsAvailable()` の生存判定 |
| `CC_INTRO_STATE_ADDR` | `0x55D20B` | u8 | R | `IntroState()` |
| `CC_WORLD_TIMER_ADDR` | `0x55D1D4` | u32 | R | `WorldTimer()` |
| `CC_REAL_TIMER_ADDR` | `0x562A40` | u32 | R | `RealTimer()` |
| `CC_MENU_STATE_COUNTER_ADDR` | `0x767440` | u32 | R | `MenuStateCounter()`。Rematch のサブメニュー判定 |
| `CC_PTR_TO_WRITE_INPUT_ADDR` | `0x76E6AC` | ptr | R | 入力書込み先の解決 |
| `CC_P1_OFFSET_DIRECTION` | `+0x18` | (u32書込) | W | P1 方向 |
| `CC_P1_OFFSET_BUTTONS` | `+0x24` | u16 | W | P1 ボタン |
| `CC_P2_OFFSET_DIRECTION` | `+0x2C` | (u32書込) | W | P2 方向 |
| `CC_P2_OFFSET_BUTTONS` | `+0x38` | u16 | W | P2 ボタン |

**seam の外側 — `SceneFastBoot`**

| 定数 | アドレス | R/W | 用途 |
|---|---|---|---|
| `CC_GAME_STATE_ADDR` | `0x74D598` | R/W | 値 1/99 を 101（`INTRO_SKIP`）に書き換えてイントロを飛ばす |
| `CC_SFX_ARRAY_ADDR` (+`_LEN`=1500) | `0x76E008` | W | 高速起動中の SE 連打を抑止 |
| `CC_FORCE_GOTO_ADDR` | `0x42B475` | W(code) | `EB xx` でメニュー遷移先を強制。xx は Versus=0x3F / Training,Replay=0x22 / VersusCPU=0x5C |

**seam の外側 — `MbaaMemTrace`（読み取りのみ）**

| 定数 | アドレス |
|---|---|
| `CC_ROUND_TIMER_ADDR` | `0x562A3C` |
| `CC_RNG_STATE0_ADDR` / `CC_RNG_STATE1_ADDR` | `0x563778` / `0x56377C` |
| `CC_P1_SEQUENCE_ADDR` / `CC_P2_SEQUENCE_ADDR` | `0x555140` / `+0xAFC` |
| `CC_P1_HEALTH_ADDR` / `CC_P2_HEALTH_ADDR` | `0x5551EC` / `+0xAFC` |
| `CC_ROUND_COUNT_ADDR` | `0x5550E0` |
| `CC_P1_WINS_ADDR` / `CC_P2_WINS_ADDR` | `0x559550` / `0x559580` |

（上記に加え、`GAME_MODE` / `INTRO_STATE` / `GAME_STATE` / `WORLD_TIMER` / `REAL_TIMER` / `MENU_STATE_COUNTER` も再読する）

**seam の外側 — `MbaaPatcher`**: 上表の 13 箇所。

残り約90個は**どこからも参照されていない**。特に注意すべきもの:

- `CC_P1/P2_CHARA_SELECTOR_ADDR`, `_MOON_SELECTOR_`, `_COLOR_SELECTOR_`, `_SELECTOR_MODE_`, `_RANDOM_COLOR_` — **キャラセレ同期は未実装**。定数の存在を実装の根拠にしないこと
- 位置・速度・加速度・メーター・ヒート・ガードバー・向き・カメラ — `rollback/DumpEntryList.hpp` からのみ参照されるが、`core_dll/CMakeLists.txt:45` で `rollback/` はビルド対象外
- `CC_WINDOW_PROC_ADDR`, `CC_LOOP_START_ADDR`, `MM_HOOK_CALL1/2_ADDR`, `MULTIPLE_MELTY`, `CC_D3DX9_OBJ_ADDR`, `FONT0-2`, `BUTTON_SPRITE_TEX` — 旧 CCCaster 由来の遺物
- `CC_SKIP_FRAMES_ADDR` (`0x55D25C`) — **定義自体がコメントアウト**。描画スキップは API フック側で行う。ただし `launcher/GameLauncher.cpp:13` が同じ値をローカル `constexpr` で再定義しており（こちらも未使用）、「禁止」は名目にとどまっている

### 7. `MbaaMemTrace` が記録するもの

`[MEM] netFrame mode intro state WT RT roundTimer menuCtr rng0 rng1 p1seq p2seq p1hp p2hp roundCnt p1win p2win`（1行1フレーム、空白区切り、`CCCASTER_MEM_TRACE=1` で有効）。

先頭が `netFrame`（= `MatchInputBuffer::GetWriteHead()`）なのは、両プロセスのログをこの番号で突き合わせるため。用途は次の3つに分かれる。

| 群 | 項目 | 何が判定できるか |
|---|---|---|
| 突き合わせ軸 | `netFrame` | 両機の行を対応付ける |
| フレーム空間 | `WT`, `RT`, `roundTimer`, `mode`, `intro`, `state`, `menuCtr` | 同じ `netFrame` で同じゲーム時刻・同じフェーズか。`ΔWT / ΔnetFrame` から対応ずれの速度が出る |
| デシンク指標 | `rng0`, `rng1`, `p1seq`, `p2seq` | 状態分岐の**最速検出**。RNG とシーケンス番号は分岐した瞬間に食い違う |
| 結果指標 | `p1hp`, `p2hp`, `roundCnt`, `p1win`, `p2win` | 分岐が見た目に出た時点。遅いが人間に読める |

限界が2つある。

- **`SafeRead` の fallback が 0 で、「読めた 0」と「読めなかった 0」が区別できない**（AUDIT 3-2 が指摘）。起動直後の行はすべて信用できない
- `CC_RNG_STATE2/3` を記録していない。RNG 状態は 4 つあるので、0/1 だけでは分岐の見落としがありうる

### 8. 設計の前提となる実測（REALHW_RESULT B-1 / B-2）

以下は 2026-08-13 の実機ログで確定した事実であり、本層の上に乗る全設計の前提になる。

| 項目 | 結果 |
|---|---|
| **Loading 中の `WorldTimer`** | **進む。全フェーズで 1 フレーム 1 カウント、速度も同一**（`phase=3(Loading)` で `ΔWT=60 / Δfip=60`）。ロード中だけ止まるという期待は外れ |
| **`Present` 回数と `WorldTimer`** | **厳密に 1:1**。98サンプル×2機で例外なく `ΔWT = Δfip`。`WT` は Present 呼出し回数そのもの |

帰結は2つある。

1. `WorldTimer` は**フレーム番号の権威として使える**。`relFrame = WT − BaseWT` が両機で同じ論理的瞬間を同じ番号にする（REALHW D）
2. 同時に、`WorldTimer` は**ロード時間差を吸収しない**。ロードが長い側は WT がその分多く進む。だから基準点 `BaseWT` はラウンドごとに取り直す必要がある

現状 `WorldTimer()` は `MatchScene.cpp:100`（`phaseBaseWorldTimer` へ代入）と各所のログにしか使われておらず、**算術的な用途はゼロ**（AUDIT 1)。値は正しく取れているのに配線されていない。

---

## 目指す設計

### 設計原則

1. **ゲームメモリに触るコードは seam の内側に置く。例外はコード書換のみ**
   データ読み書きは全部 `IGameMemory` を通す。`.text` を書き換えるもの（`MbaaPatcher` / `CC_FORCE_GOTO_ADDR`）だけは性質が違うので外に置いてよい。この線引きなら「テストできない範囲」が「逆アセンブルが要る範囲」と一致し、説明可能になる。

2. **書込みは事前条件を検証してから行う**
   ゲームプロセスを壊す唯一の経路が `WriteInput` である。読み取りは最悪 0 を返すだけで済むが、書込みは戻らない。

3. **`GamePhase::Unknown` に制御上の意味を持たせない**
   「未知」と「FastBoot 対象」は別概念。順序比較 (`phase < CharaSelect`) で分岐する限り、enum に定数を足すたびに制御が変わる。

4. **定数は「使っているもの」と「資料」を分ける**
   130個の定数が同列に並んでいる限り、定数の存在が実装の存在と誤読され続ける。

5. **観測は本番経路の性質を変えない**
   `MbaaMemTrace` は無効時ゼロコストを維持する（現状 `IsEnabled()` は静的初期化1回で満たしている）。

### 変更点

| # | 内容 | 対応する問題 | 参照 |
|---|---|---|---|
| **G-1** | `IGameMemory` に `GameState()` の R/W と `WriteSfxClear()` を追加し、`SceneFastBoot` から直接アドレス参照を消す。残るのは `CC_FORCE_GOTO_ADDR` のみ | 問題1 | — |
| **G-2** | `SceneFastBoot::IsComplete()` を機能させる。ゲート側で `phase == CharaSelect` になった時点に `MarkComplete()` を呼ぶか、`ProcessFrame` を無条件呼び出しにして内部で判定する | 問題2 | — |
| **G-3** | `WriteInput` に書込み先検証を入れる。`IsBadWritePtr(base, 0x3A)` + SEH。加えて `direction` の書込み幅を `uint16_t` に揃える | 問題4 / A-7 | AUDIT 2-3 |
| **G-4** | `MbaaPatcher` が `MemoryPatcher` の戻り値を集約し、1つでも失敗したら失敗としてログ・IPC へ通知する | 問題5 | AUDIT 🟡 |
| **G-5** | パッチ 4-1 / 4-2 を逆アセンブルで確定させ、コメントを付ける。判明するまで**触らない** | 問題5 | AUDIT 5.4 |
| **G-6** | `PhaseMonitor` に `CC_GAME_MODE_MAIN`(25) / `OPENING`(3) を明示マップし、FastBoot ゲートを順序比較から `phase == MainMenu \|\| phase == Unknown` の明示列挙に変える | 問題3 | — |
| **G-7** | `MbaaAddresses.hpp` を「実使用」「rollback 予約」「未使用（資料）」の3ファイルに分割する。未使用側は `#if 0` ではなく別ファイルに追い出し、include されない状態にする | 問題6 | AUDIT 🟡 |
| **G-8** | `MbaaMemTrace` に `CC_RNG_STATE2/3` を追加し、`SafeRead` を `std::optional` 相当にして「読めなかった」を `-` で出力する | 問題7 | AUDIT 3-2 |
| **G-9** | `WorldTimer()` を 04 章のフレーム空間に配線する（`relFrame = WT − BaseWT`）。本層の変更は不要で、値の消費側の作業 | 問題8 | REALHW G-2 |

### 移行手順

順序に意味がある。**G-3 → G-1/G-2/G-6 → G-7 → G-4 → G-5** の順で行う。

| 段 | 作業 | 検証 |
|---|---|---|
| 1 | **G-3**（書込み検証と幅の是正） | `+0x1A..0x1B` を書かなくなっても対戦が成立することを実機で確認。ここは他の変更と混ぜない — 挙動が変わったときの切り分けが不能になる |
| 2 | **G-1**（seam 拡張）+ **G-2**（IsComplete 修正）+ **G-6**（Unknown の除去） | この3つは FastBoot の制御フローを共有するので同時に行う。`FakeGameMemory` で「gameMode 25 → メニューナビ → 20 到達 → 完了」を通す単体テストを新設する。**現状この経路のテストは1件もない** |
| 3 | **G-7**（定数の分割） | ビルドが通ること。未使用ファイルを include から外してもビルドが通る＝実使用リストが正しいことの証明になる |
| 4 | **G-4**（パッチ失敗の検出） | わざと不正アドレスを混ぜて失敗ログが出ることを確認 |
| 5 | **G-5**（4-1/4-2 の解明） | 逆アセンブル。結論が出るまで手を入れない |
| 6 | **G-8**（トレース拡充）→ **G-9**（WT の配線） | G-8 を先に入れることで、G-9 の効果を `[MEM]` ログで測れるようにする |

段 2 と段 6 は独立しており並行できる。段 5 は他と依存しない。

---

## 未確定事項

| # | 内容 | 判定方法 | 影響 |
|---|---|---|---|
| U-1 | **`+0x1A..0x1B` / `+0x2E..0x2F` が未使用パディングか**。現状 `direction` の 4 バイト書込みが毎フレーム 0 で潰している | 逆アセンブル、または G-3 適用後の実機挙動比較 | G-3 の可否。潰していたものが意味を持っていた場合、幅を直すと挙動が変わる |
| U-2 | **パッチ 4-1 / 4-2 の意図**（`0x4A1D42` / `0x4A1D4A`） | 逆アセンブル必須 | どちらかを消してよいか。4-2 が生きているなら 4-1 のジャンプ幅が誤り |
| U-3 | **`0x4A02xx` 帯の NOP 群と 4-1/4-2 の関係**。アドレスが近接しており、同じ入力処理ルーチンの可能性がある | 同上 | 入力クリア無効化の完全性 |
| U-4 | **`CC_GAME_STATE_ADDR` の全値の意味**。定義されているのは 1/2/12/99/100/101 のみで、FastBoot が使うのは 1/99→101 だけ | `[MEM]` の `state` 列を全フェーズで観測 | イントロスキップの取りこぼし。IntroBarrier の判定材料になりうる |
| U-5 | **`CC_ALIVE_FLAG_ADDR`(`0x76E650`) が `IsAvailable()` の判定に使えるか**。現状 `IsAvailable()` は `CC_GAME_MODE_ADDR` の読取り可否しか見ていない | 実機で終了時の値遷移を観測 | 終了間際の `WriteInput` クラッシュ回避（A-7 の残り半分） |
| U-6 | **RNG 状態 4 個のうち、どれがデシンク検出に最も早いか** | G-8 適用後、意図的にデシンクさせて比較 | 検証コストと検出感度のトレードオフ |
| U-7 | **`WriteInput` の書込みがゲームの読み取りより前か後か**（`Present` フック内の位置）。1:1 が成立していても、フレーム内順序が保証されているかは別問題 | 逆アセンブル、または遅延1フレームの実測 | 05 入力パイプラインの遅延見積り |

推測: U-7 は現状デシンクの原因ではない（両機で同じフック位置を使うため対称）。ただし遅延計算の絶対値には効く。
