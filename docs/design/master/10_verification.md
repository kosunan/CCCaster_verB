> **旧監査資料（2026-08-13）**。現行仕様は [CURRENT_STATE](../../CURRENT_STATE.md) を参照。本文の「現行」「未実装」は当時の状態。

# 10. 検証基盤 — 何をどこで確かめるか

## 責務

「変更が壊していないこと」を、**ゲームを起動せずにどこまで言えるか**を決める領域。
本プロジェクトの検証は3階層あり、下の階層ほど速く・安く・回数を稼げるが、
**同期の本質（実時間のタイミング余裕とゲーム内部状態の一致）は上の階層でしか確かめられない**。

この章が持つもの:

| 種別 | 実体 |
|---|---|
| 単体テスト | `src/tests/`（5ターゲット + `test_overlay` は Windows 専用）、`ctest` |
| 2プロセス harness | `src/harness/`（`FakeGame` / `harness_main` / `harness_stubs` / `run_pair.ps1` / `run_pair.sh`） |
| 実機 E2E | `_TEST_MBAACC/dual_test.bat`、`src/harness/run_real_pair.ps1` |
| 観測系 | `common/LogSink.*`、`common/DebugLog.hpp`、`mbaa_mem/MbaaMemTrace.*` |
| テスト用注入口 | `common/TimeScale.hpp`（`CCCASTER_TIME_SCALE`）、`common/ScriptedInput.hpp`（`CCCASTER_SCRIPT_INPUT`） |
| ビルド構成 | ルート `CMakeLists.txt`、`src/*/CMakeLists.txt`、`cmake/toolchain-mingw32.cmake` |

責務外: 各領域のロジックそのもの（01〜09 が持つ）。この章は**測り方**だけを決める。

---

## 現状の構造

### 検証の階層

| 階層 | 実行時間 | 必要環境 | 検証できること | 検証できないこと |
|---|---|---|---|---|
| **単体テスト**<br>`ctest` | 数十 ms | Linux / Windows どちらでも | 入力符号化（`GameInput`）、`MatchInputBuffer` のリング/確定/衝突検出、`SceneInputFilter` のフェーズ別制約16ケース、`PhaseMonitor` の seam 経由判定、`NetplayClock` のθ推定・α補正 | **プロセス間の相互作用が一切無い**。フレーム空間、背圧、パケット往復、時間経過に依存する挙動 |
| **harness 2プロセス**<br>`run_pair.sh` / `.ps1` | 数十秒（既定 2,800F ≈ 47秒） | Linux / Windows。ゲーム不要 | 実 UDP loopback を通した `SceneRunner` / `NetplaySession` / `SyncCodec` / `Metronome` の通し動作。ハンドシェイク、背圧、stall/starve、**同じ `netFrame` に同じ入力が配られたか**、`FakeGame` の `mode/intro/WT/RT` の一致 | 実ゲームの状態（RNG・HP・シーケンス番号）、`TimeHooks` の影響、FastBoot の実挙動、`MbaaPatcher`、DirectInput、描画、クロック品質 |
| **実機 dual_test**<br>`run_real_pair.ps1` / `dual_test.bat` | 数分（既定 75秒待機 + 手作業） | **Windows GUI 必須**、MBAA.exe 2窓 | 上記すべて + 実ゲームメモリの分岐点（`CCCASTER_MEM_TRACE=1` 時）、注入・パッチ・オーバーレイ・コントローラ、実クロックのドリフト | 自動判定の網羅性。再現性（機体差・GUI・人の操作が入る）。**現状ロス/遅延注入は対戦パケットに効いていない**（AUDIT B-7） |

**この表の読み方**: 下2行が空欄になった項目は「誰も見ていない」ということ。
特に harness の「検証できないこと」列がそのまま **「harness で OK なのに実機で NG」の発生源**である。

### 主要コンポーネント

| コンポーネント | 役割 | 押さえどころ |
|---|---|---|
| `FakeGame` | `IGameMemory` の実装。決められたフレーム数で画面を進める**タイムライン駆動** | 入力に一切反応しない。`Advance()` で `_worldTimer` を常時 ++（実機と一致することを 2026-08-13 に実測確認） |
| `FakeGame::Script` | 各画面の滞在フレーム数。既定は 2026-07-27 の実機トレース由来の実測値 | 机上の値にすると harness が実機と違う挙動を示し、**嘘の安心**を与える。`--loading-frames` を左右で変えるとロード時間差を再現できる |
| `harness_main` | `Present` コールバックの代わりに 60Hz で `SceneRunner::Step()` を回す | 時間の仮想化はしない。N フレームに N/60 秒かかる（`CCCASTER_TIME_SCALE` を除く） |
| `harness_stubs` | `HookLog` / `DirectInputHook` / `StateUiLogic` / `TimeHooks` / `MbaaMemTrace` / `SceneFastBoot` を差し替え | `DirectInputHook` はスタブであると同時に**入力注入口**（`SetTestInputP1/P2`） |
| `run_pair.sh` / `.ps1` | 2プロセス起動 → 記録の突き合わせ | **判定と出力形式を両OSで意図的に揃えてある**。物差しが違うと「Linux では OK / 実機では NG」の切り分けができない |
| `run_real_pair.ps1` | 実機2窓の自動テスト | デプロイ + **MD5 照合**、`CCCASTER_SCRIPT_INPUT=1`、`[REC]` と `[MEM]` の突き合わせを行う |
| `dual_test.bat` | 実機2窓の手動起動 | デプロイはするが照合はしない。決定性判定も無い。`--sim-*` は効いていない |

### 判定ロジック（`run_pair` 共通）

2種類を独立に見る。**片方だけでは足りない。**

1. **決定性チェック** — `netFrame → "p1dir p1btn p2dir p2btn"` の対応表を両プロセスから作り、
   共通 `netFrame` で完全一致するか。不一致なら先頭5件を出す。
2. **ゲーム状態の突き合わせ** — `*.state`（`netFrame mode intro WT RT`）を同様に比較し、
   列ごとに**最初に分岐した `netFrame`** を出す。同一 `netFrame` が複数あれば最初を採用（実機 `[MEM]` 判定と同じ規則）。

> 入力列が一致していても、状態が分岐していれば実際の対戦はデシンクする。だから2つ出す。

### ビルド構成

| 構成 | コマンド | 生成物 | 用途 |
|---|---|---|---|
| Windows / MSYS2 mingw32 | `PATH` に `C:\msys64\mingw32\bin` を先頭で通して `cmake --build build` | DLL + EXE + harness + tests | **実機で使う唯一の成果物** |
| Linux ネイティブ | `cmake -B build && cmake --build build -j` | harness + tests のみ | 同期ロジックの高速反復 |
| Linux → mingw32 クロス | `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake` | PE32 一式 | **コンパイル確認のみ**。MSYS2 と GCC バージョンが違うため実機には使わない |

ガードとその理由:

| ガード | 場所 | なぜ必要か |
|---|---|---|
| 32bit 強制（`CMAKE_SIZEOF_VOID_P EQUAL 4` でなければ `FATAL_ERROR`） | ルート `CMakeLists.txt:39-45` | 64bit でも DLL は生成できるが、32bit の MBAA.exe への注入が**無言で**失敗する。症状から原因に辿り着けないので configure 時点で落とす |
| `_WIN32_WINNT=0x0601` / `WINVER=0x0601` 固定 | ルート `CMakeLists.txt:51-53` | 既定値任せだと MSYS2(GCC15, 既定 Win10) では通り、古い mingw-w64(既定 WinXP SP2) では `inet_pton`/`inet_ntop` が未宣言になる。どのツールチェインでも同じ API 集合が見えるようにする |
| MinHook / ImGui を `if(WIN32)` で囲む | 同 `:66-105` | どちらも Windows 専用（x86 命令長デコーダ / win32・dx9 バックエンド）。非 Windows では取得もしない |
| `i686-w64-mingw32` は **posix スレッドモデル必須** | `cmake/toolchain-mingw32.cmake:17-20` | win32 モデルの GCC は `std::thread` / `std::mutex` を提供せず、`NetplaySession` と `LogSink` がビルドできない |

`harness` は 32bit 制約の外にある（`src/harness/CMakeLists.txt:43-52`）。開発機のネイティブ環境でそのまま動く。

### FakeGame と実機の差

**この表が harness の信頼範囲そのもの。**

| 項目 | 実機 | harness | 差が生む症状 |
|---|---|---|---|
| `TimeHooks` | `SetTimeMultiplier(1000)` + `SetSleepBypass(true)` が恒久有効。**自DLL の `Sleep(1)` も `Sleep(0)` に化ける** | スタブ。実 QPC をそのまま返す。Linux では TimeHooks 自体が存在しない | 実機だけで起きるビジースピン・CPU 100%・Metronome 未起動区間のフリーラン（AUDIT A-2/A-4）を harness は**再現しない** |
| 入力への反応 | ゲームが入力を解釈して状態遷移する | **反応しない**。タイムライン駆動 | 「入力が届いた結果として画面が進む」経路が検証されない。メニュー自動ナビ（Rematch）の成否は harness では判定不能 |
| `SceneFastBoot` | `CC_GAME_STATE_ADDR` / `CC_SFX_ARRAY_ADDR` / `CC_FORCE_GOTO_ADDR` を直接書換え、入力を偽造してメニューを進める | スタブ。`GameMode()==CharaSelect` になったら完了を返すだけ | FastBoot の所要時間・失敗・`RenderSkip` による初期化ブロック（AUDIT B-1）が harness に出ない |
| `MbaaMemTrace` | `CCCASTER_MEM_TRACE=1` で RNG/HP/seq を毎フレーム記録 | **no-op** | harness の状態比較は `mode/intro/WT/RT` の4列だけ。**RNG 分岐は原理的に見えない** |
| `MbaaPatcher` | ゲーム自身の入力読み取りを潰す等のバイナリパッチ | 存在しない | パッチ起因の挙動は harness に出ない |
| クロック | WASAPI オーディオクロック | Windows: WASAPI / Linux: `CLOCK_MONOTONIC` | **Linux harness はクロック品質・ドリフトの検証に使えない** |
| IPC | `IpcManager` で CLI ランチャーと通信 | Linux では no-op | UI 反映・切断表示の経路が検証されない |
| ログ出力先 | `LogSink` 経由でファイル | `harness_stubs.cpp` の `HookLog` が `printf` + `fflush` | **同期 I/O なので実機とフレームタイミングの汚染度が違う** |

推測: harness で最も再現しやすいのは「番号のずれ」（starve、ロード時間差）、
最も再現しにくいのは「実時間の余裕不足」（RTT、ジッタ、フリーラン）。
2026-08-13 に実機由来のデシンク（`WT` が `netFrame=231` 付近で分岐）が Linux harness で
同じ形で再現したのは前者に属するため。**後者を harness で「通った」と判断してはいけない。**

### 時間圧縮（`CCCASTER_TIME_SCALE`）

`TimeScale.hpp` が環境変数を1回だけ読み、`ScaleTickUs()` がフレーム周期を 1/N にする。

| スケールする | スケールしない |
|---|---|
| フレーム周期（Metronome のティック、通信スレッドの周期、harness の外側ペース） | θ推定・RTT・α補正 |
| 理由: 両プロセスが同じ値を読むので歩調は揃う | 理由: 実時刻で測る値であり、いじると**測っているものが変わる** |

**4倍速では1フレームの実時間が 16.6ms → 4.2ms になる。RTT に対する余裕が 1/4 になるため、
タイミング余裕の検証にはならない。** 区切り（実機ゲート）は必ず等倍で回す。
`run_pair.sh:52-54` と `run_real_pair.ps1:58` は 1 より大きいときに警告を出す。

### 決定性判定（`ScriptedInput`）

`ScriptedInput(frame, isHost)` は**フレーム番号と役割だけから決まる純関数**。

- 24フレーム周期。host/client で位相を 7 ずらす（**両者がゼロを出すだけの自明な一致にしない**）
- 引数のフレーム番号は `netFrame`（`SceneRunner.cpp:200` は `head + 1`）。
  両者が同じ番号に同じ値を出すので、受け取った側は相手の入力を**検算できる**
- `CCCASTER_SCRIPT_INPUT=1` で有効。通常プレイでは無効
- 有効時のみ `SceneRunner.cpp:259-262` が `[REC] netFrame p1dir p1btn p2dir p2btn` を出し、
  `run_real_pair.ps1` がこれを突き合わせる

**なぜ必要か**: 人がコントローラを操作すると両者の入力が再現できず、
不一致が「操作の違い」なのか「同期の破綻」なのか区別できないため。

### `[HAZARD]` マークの意味

`src/tests/` で `CC_CASE("[HAZARD] ...")` と書かれたケースは、
**現在の危険な挙動をそのまま固定したもの**であり、正しさの保証ではない。

現存するのは `test_netplay_clock.cpp` の4件（θ変換が `SetPeerStartTime` 時の値で固定される、
RTT 同値では新サンプルに乗り換えない、開始時刻 0 と未設定を区別できない、
`Reset` が `baselineTheta` を消さない）。

**失敗したらテストを直すのではなく、その挙動を変えたのが意図的かを判断すること。**
`test_input_buffers.cpp` は旧 `[HAZARD]` 5件が「あるべき挙動」の検証に**昇格済み**で、
これが `[HAZARD]` の正しい出口である。

### 他領域との境界

| 相手 | 境界 |
|---|---|
| **01 プロセスライフサイクル** | harness は DLL 注入を通らない。`dllmain` の初期化順序・DETACH 経路は実機でしか検証できない |
| **02 フック層** | `TimeHooks` / `DxHook` / `WndProcHook` / `DirectInputHook` はすべてスタブ。**フック層は harness の検証範囲外** |
| **03 ゲームメモリ** | `IGameMemory` seam が唯一の接点。`FakeGame` がそれを実装する。`RealGameMemory` と `MbaaAddresses` の妥当性は実機でしか確かめられない |
| **04 フレーム空間と時間** | `*.state` の `WT/RT` 比較がこの章の主判定。04 が定義する `relFrame = WT − BaseWT` を固定するテストは**まだ無い**（下記 P-2） |
| **05 入力パイプライン** | `ScriptedInput` → `[REC]` の突き合わせが決定性判定。`SceneInputFilter` は単体テスト16ケースで固定済み |
| **06 ネットワーク** | harness は**本物の UDP**（loopback）を使う。`NetworkSimulator` は harness にリンクされているが DLL 側では有効化されていない（B-7） |
| **07 セッション状態機械** | `NetplaySession` を実物のままリンクするので、ハンドシェイク〜Counting 遷移は harness で検証できる |
| **08 ロールバック** | `rollback/` は CMake 除外。**テストも harness も一切通っていない** |
| **09 UI** | `StateUiLogic` はスタブ。`test_overlay` のみ Windows 専用でビルドされ、ctest には登録されていない |

---

## 現状の問題

### P-1. ログの 94% がノイズ — 読めないログは無いのと同じ（実測）

2026-08-13 の実機ログ 11,164 行の内訳:

| 行種 | 行数 | 割合 | 出力条件 |
|---|---|---|---|
| `[SyncCodec] RECV pkt` | 5,267 | 47% | `SyncCodec.cpp:111` — **受信パケットごと**（無条件） |
| `[Backpressure] NEAR` | 5,252 | 47% | `SceneRunner.cpp:190-193` — `lead > maxLead - 16` |
| **小計** | **10,519** | **94%** | |
| `[SceneRunner]` 定期ログ | 98 | 0.9% | 60フレームごと |

`NEAR` の閾値は**壊れている**。コメントは「背圧間際（1フレーム以下の余裕）」だが、
既定 `D=2, R=4` で `maxLead = 6`、条件は `lead > -10` となり **`lead ≥ 0` の限り毎フレーム成立する**。
「間際」の判定になっていない。

実害: 今回の解析で使えた情報は 98 行の定期ログと `[IntroTrack]` / `[IntroBarrier]` だけだった。
`LogSink` の非同期化で I/O コストは下がったが、**人間が読める量に収まっていない**。

### P-2. フレーム空間を固定するテストが1つも無い

「同じ `netFrame` なら同じ `WorldTimer` / `mode` / `intro`」を検証する**テストが存在しない**。
`run_pair` の状態突き合わせは存在するが、これはスクリプト側の目視レポートであり、
`ctest` の合否にならず CI で落ちない。

デシンクがテストをすり抜ける理由はここに尽きる。単体テストは `MatchInputBuffer` の
リング操作を検証するが、**その番号がゲームの何時に対応するかは誰も検証していない**。
04 章が指摘した「`netFrame` はゲームの時計と一度も突き合わされていない」が、
そのまま検証側の穴として写っている。

### P-3. `CCCASTER_MEM_TRACE=1` を付け忘れると最も重要な情報が消える

2026-08-13 の実機解析は `CCCASTER_MEM_TRACE` が未設定で `[MEM]` 行が**0件**だった。
RNG・HP・シーケンス番号という**デシンクの直接証拠**が取れず、
60フレームごとの定期ログから読み取れたのは幸運にすぎない。

`dual_test.bat` にはこのスイッチを立てる仕組みが無い（`run_real_pair.ps1 -MemTrace` にはある）。

### P-4. `dual_test.bat` のロス注入が効いていない（AUDIT B-7）

バッチには `--sim-delay 50,90 --sim-loss 20` と書かれ、画面にも
「SIM: delay=50~90ms, loss=20%」と表示される。しかし `NetworkSimulator` は DLL 側で
有効化されておらず、作用するのは**ネゴシエーションパケットだけ**（`main.cpp:79` / `UdpSocket.cpp:74-101`）。

**対戦中の通信は無劣化。ロス耐性の試験は一度も行われていない。**
表示があるぶん「劣化環境で通った」という誤った自信の根拠になり、素の無表示より悪い。

### P-5. `dual_test.bat` は決定性を判定しない

デプロイはするが**ハッシュ照合をしない**（`copy /Y` の成否も見ない）。
`CCCASTER_SCRIPT_INPUT` も立てないので `[REC]` が出ず、判定は「ログ末尾20行を人が読む」だけ。
`run_real_pair.ps1` に同等以上の機能が揃っているのに、バッチ側は注意書きで誘導しているだけである。

### P-6. 単体テストの被覆に穴がある

| 対象 | 状態 |
|---|---|
| `test_overlay` | `if(WIN32)` 内でビルドされるが **`add_test` されていない**。ctest では一度も走らない |
| `rollback/` | CMake 除外。テスト無し |
| `SyncCodec` / `PacketRouter` | 単体テスト無し。harness の通し動作でしか触れない |
| `Metronome` | 単体テスト無し。catch-up 上限（B-4）の挙動を固定するテストが無い |
| `SceneRunner` | 単体テスト不可能な構造（プロセス内シングルトン + 静的状態） |

### P-7. harness はプロセス内シングルトン前提

`SceneRunner` も `MatchInputBuffer` もシングルトンなので **1プロセス1セッション**。
2セッションを1プロセスで走らせる「インプロセス2ピア」テスト（最速・最決定的）は現状書けない。

---

## 目指す設計

### 設計原則

1. **階層ごとに答えるべき問いを1つに絞る。**
   単体テスト = 「この関数の契約は守られているか」/ harness = 「2プロセスの番号と状態は揃うか」/
   実機 = 「実時間の余裕と実ゲーム状態は保たれるか」。問いが混ざると、
   どの階層が壊れたのか分からなくなる。
2. **ログは「常時出す最小限」と「異常時に出す詳細」に分ける。**
   読めない量を出すのは出していないのと同じ、という 2026-08-13 の実測を出発点にする。
3. **判定は機械が行う。人がログを読む前提の検証は検証ではない。**
   `run_pair` の突き合わせは既にそうなっている。実機側も `run_real_pair.ps1` に一本化する。
4. **harness で通ったことを実機の保証と読み替えない。**
   「FakeGame と実機の差」表に載っている項目は、実機ゲートを通るまで未検証として扱う。
5. **無効な機能に有効そうな表示を出さない。** `--sim-loss` の表示は最優先で消すか直す。

### 変更点

#### V-1. ログのレベル分けと間引き（最優先）

`DebugLog` にレベルを導入し、既定で出す行を絞る。

| レベル | 出す条件 | 例 |
|---|---|---|
| `FATAL` / `ASSERT` | 常時 + **同期フラッシュ** | 既存の `IsUrgent()` 判定をそのまま使う |
| `EVENT` | 状態が変わったとき**だけ** | フェーズ遷移、`Mode -> Counting`、`IntroBarrier` 成立、切断、D/R 変更 |
| `PERIODIC` | 60フレームごと | 既存の `[SceneRunner] phase=... WT=... wh=...` |
| `ANOMALY` | 閾値を割ったとき**だけ** | `[Backpressure] STALLED`、`NEAR` は**閾値を正しく直したうえで** |
| `TRACE` | 環境変数で明示的に有効化したときだけ | `[SyncCodec] RECV pkt`、`[MEM]`、`[REC]` |

具体策:

| 対象 | 変更 |
|---|---|
| `[SyncCodec] RECV pkt` | 既定で**出さない**。`CCCASTER_PKT_TRACE=1` のときだけ。既定では「N 秒間に受けたパケット数・欠番数」の集約1行を `PERIODIC` で出す |
| `[Backpressure] NEAR` | 閾値を `lead > maxLead - 16` から `lead >= maxLead - 1`（余裕1フレーム以下）へ。さらに**状態が変わったときだけ**出す（NEAR に入った/出た） |
| `[Backpressure] STALLED` | 連続 STALLED は先頭1行 + 継続フレーム数の集約に。現状は毎フレーム出る |
| `[REC]` / `[MEM]` | 現状どおり環境変数駆動。ただし**両方を1つのスイッチで立てられるようにする**（P-3 の付け忘れ対策） |

理由: 上記だけで実機ログは 11,164 行 → **数百行**に落ちる。
調査の所要時間はログの読める量で決まる。

#### V-2. フレーム空間を固定するテストを新設

04 章の `relFrame = WT − BaseWT` を**単体テストで固定する**。
`SceneRunner` はシングルトンで直接テストできないので、番号付けを純粋関数に切り出したうえで検証する。

| テスト | 内容 | 何を守るか |
|---|---|---|
| `relFrame` の定義 | `BaseWT` が異なる2機（例: 5412 と 5560）で、同じ論理的瞬間が同じ `relFrame` になる | REALHW D の結論。**原点を交換・合意しなくても揃う**という設計の核 |
| `BaseWT` 未確定時 | `BaseWT` が未設定のあいだは `relFrame` を配らない（0 を配らない） | 「読めなかった 0」を「読めた 0」として確定させない |
| WT の同値・欠番 | 同じ WT が2回来た / WT が飛んだ ときの規則を固定 | 04 章が新規に決めるポリシーの回帰防止 |
| ラウンドをまたぐ再基準化 | Rematch 後に `BaseWT` を取り直すと `relFrame` が 0 に戻り、両者で一致する | REALHW C-3（Rematch で 148F 開く）の再発防止 |

さらに **harness の状態突き合わせを `ctest` の合否に載せる**:
`run_pair` の Python 判定を独立したスクリプトに切り出し、
「共通 `netFrame` で `mode/intro/WT/RT` がすべて一致」を **exit code** で返す。
現状のように目視レポートで終わらせない。

#### V-3. ログ規約の明文化（`LogSink` との契約）

`LogSink::IsUrgent()` は `[FATAL]` / `[ASSERT]` / `Disconnected` / `Aborted` を含む行だけを
同期フラッシュする。**新しく致命的なログを足すときは、必ずこの4語のどれかを含めること。**
含めない行は非同期に流れ、`TerminateProcess` を伴う経路ではワーカーごと消えて残らない。

これはコメントに書いてあるが**機械的に守らせる仕組みが無い**。
`DebugLog` に `FATAL` レベルを設け、そのレベルの出力が必ず `[FATAL]` を前置するようにして、
規約を型で守る。

#### V-4. `dual_test.bat` の是正

| 項目 | 変更 | 理由 |
|---|---|---|
| `--sim-delay` / `--sim-loss` | **`NetworkSimulator` を DLL 側で有効化するまで、バッチから削除し表示も消す** | 効いていない注入を表示するのは、無表示より悪い。誤った自信の根拠になる |
| 有効化の経路 | `NetworkSimulator` を IPC または環境変数で DLL 側に伝え、対戦パケットにも適用する（AUDIT 3-1） | これを直すまで「ロス耐性は未試験」と明記し続ける |
| デプロイ | `copy` 後に **MD5 照合**（`run_real_pair.ps1:40-45` と同じ）。不一致なら停止 | 古い DLL を掴んだまま何の警告も出ない事故を潰す |
| 決定性判定 | `CCCASTER_SCRIPT_INPUT=1` を立て、`[REC]` の突き合わせを行う | 人がログ末尾を読む方式をやめる |
| 位置づけ | **`run_real_pair.ps1` を正とし、`dual_test.bat` は「人が操作したいとき用」に限定** | 同じ役割の入口を2つ維持すると片方が必ず腐る |

#### V-5. CI（Linux で回す範囲）

| ジョブ | 内容 | 落とす条件 |
|---|---|---|
| `build-linux` | `cmake -B build && cmake --build build -j` | コンパイル/リンクエラー |
| `test` | `ctest --test-dir build --output-on-failure` | いずれかのテスト失敗（`[HAZARD]` を含む） |
| `build-win32` | `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw32.cmake` で DLL 一式 | Windows 側を壊していないかのコンパイル確認 |
| `harness-pair` | `run_pair.sh --host-loading 60 --client-loading 180`（**等倍**） | 決定性 NG または状態分岐あり |
| `harness-pair-fast` | `--time-scale 4` で回数を稼ぐ | 同上。ただし**タイミング余裕の検証ではないと明記** |

**実機ゲートは CI に入れない。** Windows GUI が要るため自動化できない。
代わりに「リリース前に `run_real_pair.ps1` を等倍 + `-MemTrace` で通す」を**手動ゲート**として位置づけ、
その結果を `docs/issues/REALHW_RESULT_*.md` に残す運用を続ける。

推測: CI で最初に効くのは `build-win32` だと考える。Linux で作業していると
Windows 専用ヘッダの取りこぼし（`<Dbt.h>` の大文字小文字など）が最も混入しやすいため。

#### V-6. 実機テスト前チェックリスト

**上から順に、1つでも欠けたら回さない。**

| # | 項目 | 確認方法 | 飛ばすとどうなるか |
|---|---|---|---|
| 1 | 32bit でビルドされているか | configure が通っている（ガードが落とす）+ 生成 DLL の bitness | 注入が無言で失敗する |
| 2 | デプロイのハッシュ照合 | `build/bin/libcccaster_hook.dll` と `MBAACC_{1,2}/cccaster/` の MD5 一致 | **古い DLL がテストされ、何の警告も出ない** |
| 3 | 古いログの削除 | `cccaster_hook_log.txt` を両機で削除 | 前回の行が混ざり、解析が丸ごと無効になる |
| 4 | `CCCASTER_MEM_TRACE=1` | `run_real_pair.ps1 -MemTrace` | **デシンクの直接証拠（RNG/HP/seq）が取れない**（2026-08-13 に実際に発生） |
| 5 | `CCCASTER_SCRIPT_INPUT=1` | 同スクリプトが自動設定 | 決定性を判定できない |
| 6 | `CCCASTER_TIME_SCALE=1`（等倍） | 同スクリプトが警告を出す | タイミング余裕の検証にならない |
| 7 | ロス/遅延注入の有効性 | B-7 が直っているか。未修正なら**「無劣化条件での結果」と明記** | 「劣化環境で通った」という誤った記録が残る |
| 8 | 単体テストと harness が通っている | `ctest` と `run_pair` が緑 | 実機で見つかるのが下の階層のバグだと、切り分けに数時間かかる |

### 移行手順

**順序に意味がある。** 観測を直してから測る。測れるようにしてから直す。

| 段階 | 内容 | なぜこの順か |
|---|---|---|
| **S-1** | `[Backpressure] NEAR` の閾値修正と `[SyncCodec] RECV pkt` の既定 OFF | 1行の変更でログが 1/17 になる。**以降のすべての調査の効率を決める**ので最初 |
| **S-2** | `DebugLog` のレベル分け導入、`FATAL` と `IsUrgent` の契約を型で固定 | S-1 を場当たりでなく規約にする |
| **S-3** | `dual_test.bat` から `--sim-*` を削除、MD5 照合を追加 | 誤った自信の根拠を消す。コード変更なしで今すぐできる |
| **S-4** | `run_pair` の状態突き合わせを exit code 化し、`ctest` に登録 | フレーム空間テスト（S-5）の受け皿を先に作る |
| **S-5** | `relFrame = WT − BaseWT` の単体テスト新設（04 章の実装と同時） | 04 の変更と**同一コミット**で入れる。後回しにすると再び「テストの無い同期機構」が増える |
| **S-6** | CI（`build-linux` / `test` / `build-win32` / `harness-pair`） | S-4/S-5 が揃ってから。緑にならないジョブを先に置くと無視される |
| **S-7** | `NetworkSimulator` の DLL 側有効化（AUDIT 3-1） | ロス耐性の試験を初めて可能にする。ここまでは「未試験」と明記し続ける |
| **S-8** | `MbaaMemTrace` に `CC_RNG_STATE2/3` を追加（AUDIT 3-2） | 「読めた 0」と「読めなかった 0」を区別する |

---

## 未確定事項

| # | 事項 | 判断に必要なもの |
|---|---|---|
| 1 | インプロセス2ピア harness を作るか | `SceneRunner` / `MatchInputBuffer` のシングルトン解体が必要。得られるのは「決定的で ms 単位」の階層。コストが見合うかは 08（ロールバック）の検証要件次第 |
| 2 | `FakeGame` を入力に反応させるか | 反応させると忠実度は上がるが、**それ自体が検証対象のない新しいコードになる**。現状は否定的。Rematch 自動ナビの検証を harness で行いたくなったときに再考する |
| 3 | ログのレベルを実行時に変えられるようにするか | 環境変数で足りるか、IPC/UI から切り替えたいか。実機で「異常が起きてから詳細を上げる」運用が要るなら後者 |
| 4 | `test_overlay` を ctest に登録するか | ImGui のヘッドレス実行が Windows CI 上で安定するか未検証 |
| 5 | 実機ゲートを部分的に自動化できるか | `run_real_pair.ps1` は既に無人で回る。セルフホストの Windows ランナーを用意する価値があるかは、実機テストの頻度次第 |
| 6 | harness の状態比較に何列足すか | 現状 `mode/intro/WT/RT` の4列。`FakeGame` に擬似 RNG を持たせても実機の RNG とは無関係なので、**足す意味があるのは決定性の自己検査だけ**。要検討 |
| 7 | `--time-scale` を CI の既定にするか | 4倍速は回数を稼げるが、タイミング起因のデシンクを見逃す。推測: 等倍を必須ゲート、4倍速を追加の回数稼ぎ、という二本立てが妥当 |
