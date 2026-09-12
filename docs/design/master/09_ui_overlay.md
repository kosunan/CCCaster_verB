> **旧監査資料（2026-08-13）**。現行仕様は [CURRENT_STATE](../../CURRENT_STATE.md) を参照。本文の「現行」「未実装」は当時の状態。

# 09. UI とオーバーレイ — 利用者との接点

## 責務

利用者が CCCaster に触れる面は **CLI ランチャー（別 EXE プロセス）** と **ゲーム内オーバーレイ（注入 DLL）**
の2つしかない。この層の責務は3つ。

| 責務 | 担当 | なぜここに要るか |
|---|---|---|
| 接続・モード選択 | `cli_launcher/` | ゲーム起動前にしか決められない（ハッシュ、ポート、Versus/Training/Spectate） |
| 対戦中の設定変更 | ゲーム内オーバーレイ | D/R とコントローラ割当はゲームを落とさずに変えられなければ使い物にならない |
| **失敗の可視化** | 両方 | **現状ここが空白**。2026-08-13 のバグが長期間残った直接の原因 |

3つ目が本章の主題である。入力が届いていない・相手が止まっている・設定が保存されていない、
のいずれも**画面にもログにも出ない**まま「効かない」とだけ現れる設計になっている。

---

## 現状の構造

### 主要コンポーネント

「生」= 実行時に到達する経路がある。「死」= ビルドされるが呼び出し元が 0 件。

| コンポーネント | ファイル | 行数 | 生/死 | 役割・到達経路 |
|---|---|---|---|---|
| `UIManager` | `core_dll/ui/UIManager.cpp` | 154 | **生** | `UiPhase` 分岐 + WndProc 処理。`GameFrameOrchestrator` と `WndProcHook` から |
| `CharaSelectUiView` | `ui/CharaSelect_Ui_View.cpp` | 31 | **生** | マッピング / D ポップアップ / R ポップアップ / 常時バー の4択 |
| `InGameUiView` | `ui/InGame_Ui_View.cpp` | 15 | **生** | `StateUiView::DrawInGameBar()` のみ |
| `StateUiView` | `ui/State_Ui_View.cpp` | 198 | **生** | 4種のバー・ポップアップ実体 |
| `StateUiLogic` | `ui/State_Ui_Logic.cpp` | 75 | **生** | UI 状態の static 置き場（D/R、ping/jitter、fps、マッピング開閉） |
| `ControllerUiView` | `ui/Controller_Ui_View.cpp` | 308 | **生** | F4 のコントローラ設定画面。`CharaSelect_Ui_View` から |
| `ControllerUiLogic` | `ui/Controller_Ui_Logic.cpp` | 400 | **生** | デバイス割当・バインドの状態と ini 保存 |
| `OverlayRenderer` | `ui/OverlayRenderer.cpp` | 64 | **生** | スタイル push/pop、収まるテキスト、時刻 |
| `RematchUiView` | `ui/Rematch_Ui_View.cpp` | 50 | **死** | `UIManager::Render` に分岐はあるが `UiPhase::Rematch` が**生成されない**（後述） |
| **`ControllerMapper`** | `ui/ControllerMapper.cpp` | **542** | **死** | `Controller_Ui_Logic` + `Controller_Ui_View` とほぼ同一の旧実装。呼び出し元 0 |
| **`NetplayOverlay`** | `ui/NetplayOverlay.cpp` | **189** | **死** | `UIManager` + `State_Ui_*` に置換済みの旧実装。呼び出し元 0 |
| `ConsoleRenderer` | `cli_launcher/ui/ConsoleRenderer.cpp` | 148 | **生** | ANSI エスケープの TUI。メニュー・テキスト入力 |
| `MainController` | `cli_launcher/controller/MainController.cpp` | 398 | **生** | ランチャーの状態機械 |

**死んでいる 731 行はすべて `src/core_dll/CMakeLists.txt:62,67` でビルド対象に入っている**
（`AUDIT_2026-08-13.md` §2 の「UI 層 1,610 行のうち 731 行（45%）が到達不能」）。
`rollback/` のようにコメント付きで除外されているわけではなく、**生きているファイルと同じリストに
並んでいる**ため、見ただけでは区別がつかない。

`ControllerMapper.cpp` と `Controller_Ui_Logic.cpp` は関数名も定数名もほぼ同じで、
`SaveDeviceAllocations` / `SaveBinds` / `ProcessBindingInput` / `ResetBindingState` が両方に存在する。
grep で最初に当たるのがどちらかは運でしかない。**2026-08-13 の保存バグ修正でも、修正したのは
`Controller_Ui_Logic.cpp` だけで `ControllerMapper.cpp` は古いままにしてある。**
つまり今の木には「直った実装」と「直っていない同名実装」が同居している。

さらに悪いことに、**単体テスト（`src/tests/test_overlay.cpp` + `stub_controller_mapper.cpp`）が
テストしているのは死んでいる `NetplayOverlay` の方**である。生きている `StateUiLogic` /
`ControllerUiLogic` にはテストが1本もない。UI のテストが緑でも、それは利用者が触る画面について
何も保証していない。

### 処理の流れ

```
D3D9 Present（1F 1回, MinHook）
└─ DxHook::Hooked_Present
   ├─ (1) GameFrameOrchestrator::OnPresent
   │     ├─ DirectInputHook::Poll()          ← 1F 1回。2回呼ぶとエッジが消える
   │     ├─ SceneRunner::Step()              ← 07/08 章の本体
   │     └─ if (s_imguiFrameReady)
   │           BeginScene / ImGui_ImplDX9_RenderDrawData / EndScene   ★描画はここ
   └─ (2) OnPresentSkip → SpeedFlags::RenderSkip なら元 Present を呼ばない

D3D9 EndScene（1F に約8回）
└─ DxHook::Hooked_EndScene → GameFrameOrchestrator::OnEndScene
   ├─ RenderSkip なら即 return            ★ここが初期化を丸ごと止める
   ├─ 初回のみ: ImGui::CreateContext / Font×2 / ImplWin32_Init / ImplDX9_Init
   │            / WndProcHook::Initialize / DirectInputHook::Initialize
   ├─ RenderTarget == BackBuffer か判定（違えば何もしない）
   ├─ s_imguiFrameReady なら return（1F 1回に間引き）
   ├─ NewFrame ×3
   ├─ GameMode → UiPhase 変換
   ├─ NetplaySession → StateUiLogic へ fps / tickUs / θ / RTT を供給
   ├─ try { UIManager::Render(uiPhase) } catch(...) {}
   └─ EndFrame / Render / s_imguiFrameReady = true
```

`GameMode → UiPhase` の対応（`GameFrameOrchestrator.cpp:176-185`）:

| GameMode | UiPhase | 表示 |
|---|---|---|
| `CC_GAME_MODE_CHARA_SELECT` | `CharaSelect` | 常時バー / D・R ポップアップ / F4 設定画面 |
| `CC_GAME_MODE_IN_GAME` | `InGame` | 拡張バー（D, R, μs, fps, Δ, RTT, JTR） |
| その他すべて | `None` | 何も出ない |

**`UiPhase::Rematch` を生成する `case` が存在しない。** `UIManager::Render` の
`case UiPhase::Rematch:`（`UIManager.cpp:31`）は到達不能で、`Rematch_Ui_View.cpp` 50 行はそのまま
死んでいる。一方 `PhaseMonitor` / `MatchScene` / `SceneInputFlter` は `GamePhase::Rematch` を
実際に扱っており、**「フェーズの概念が2つあり、片方だけリマッチを知っている」**状態になっている。

### ImGui 初期化順序と RenderSkip

初期化は `OnEndScene` の中でしか行われず、その手前に `RenderSkip` の早期 return がある。
`SceneRunner::Init()` は `GC::SetModeHighSpeedSkip()`（`SceneRunner.cpp:107`）で **起動直後から
`RenderSkip=true`** にし、FastBoot 完了まで解除しない。

したがって初期化される順序と条件は次のとおり。

| # | 対象 | 条件 |
|---|---|---|
| 1 | `SceneRunner::Init` → `RenderSkip=true` | DLL 注入直後 |
| 2 | FastBoot 完了（`phase >= CharaSelect`） | `SceneRunner.cpp:144-152` の早期 return を抜ける |
| 3 | `SetRenderSkipByGap(0)` で `RenderSkip=false` | `SceneRunner.cpp:158`（**常に定数 0**） |
| 4 | ImGui コンテキスト / フォント / DX9・Win32 バックエンド | 3 の後の最初の `OnEndScene` |
| 5 | `WndProcHook::Initialize` / `DirectInputHook::Initialize` | 同上（**4 と同じ if ブロック**） |

**FastBoot が完了しない限り、オーバーレイもホットキーもコントローラも1つも存在しない。**
`WndProcHook` と `DirectInputHook` の初期化が ImGui の遅延初期化ブロックに同居しているのが原因で、
これらは本来 D3D デバイスに依存しない。`AUDIT_2026-08-13.md` の B-1 がこれで、
「オーバーレイが出ない」を UI のバグとして追うと必ず外す。

`DirectInputHook` が初期化されないということは、**FastBoot 中は入力デバイスが1つも列挙されていない**
ということでもある（05 章）。

### 描画の2段構え

MBAA は 1 フレームに約8回 `EndScene` を呼び、後続パスでバックバッファを上書きする。
そのため `EndScene` では **NewFrame → Render（ドローデータ生成）まで**を行い、
実際の `RenderDrawData` は `Present` の直前に回している（`GameFrameOrchestrator.cpp:87-92`）。

これは目的に対しては正しいが、**次の3つの暗黙の前提に依存している**。

| 前提 | 破れた場合 |
|---|---|
| `EndScene` の後に必ず `Present` が来る | `s_imguiFrameReady` が `true` のまま残り、`OnEndScene:164` の `if (s_imguiFrameReady) return;` で**オーバーレイが永久に静止**する。復帰経路はない |
| `Present` までに `NewFrame` が再度呼ばれない | ドローデータは ImGui コンテキスト内部バッファを指すので、二重 `NewFrame` で参照先が壊れる |
| `EndScene`〜`Present` の間に `Reset` が来ない | `OnPreReset` が `InvalidateDeviceObjects` した直後に `RenderDrawData` が走る。解放済みリソースを描く |

さらに `Hooked_Present` は **`onPresent`（描画）を呼んでから `onPresentSkip` を判定する**
（`DxHook.cpp:205-214`）。`RenderSkip` が立っている間も ImGui の描画コストは払っており、
その結果は表示されずに捨てられる。

### 入力の横取り

`WndProcHook::HookedWindowProc` は**すべてのウィンドウメッセージ**を
`UIManager::HandleWndProcMessage` に先に通す（`WndProcHook.cpp:41-49`）。戻り値 >0 で
`return 0`、すなわち**元の WndProc に一切渡らない**。

| 順 | 判定 | 消費するもの |
|---|---|---|
| (1) | `ImGui_ImplWin32_WndProcHandler` が非 0 | ImGui が要求したメッセージ全部 |
| (2) | `VK_F4`（Alt なし） | **フェーズを問わず必ず `return 1`**（下記） |
| (3) | マッピング画面が開いている | `WM_KEYDOWN` / `WM_SYSKEYDOWN` / `WM_KEYUP` / `WM_SYSKEYUP` **全部** |
| (4) | `Ctrl` + `0`〜`9` / NumPad | Delay 設定 |
| (5) | `Alt` + `0`〜`9` / NumPad | MaxRollback 設定 |
| (6) | `WM_DEVICECHANGE` | 消費はしない。`DirectInputHook::RefreshDevices()` を呼ぶだけ |
| (7) | `io.WantCaptureMouse` / `WantCaptureKeyboard` | 該当するマウス・キーメッセージ |

**F4 はキャラセレ以外でも吸われる。** `UIManager.cpp:108-115` は、キャラセレでなければ
`return 1`（＝何もせずブロック）、キャラセレなら `OnMappingInput()` して `return 1` と、
**両方の枝で 1 を返す**。ゲーム本来の F4 の機能があるなら、全フェーズで永久に失われる。
なお `mem.IsAvailable()` が偽のときは条件式が偽になるため、**ゲームメモリが読めない間は
どのフェーズでも F4 で設定画面が開く**。この非対称は意図されたものには見えない。

**一方でゲームプレイ入力は奪われない。** 05 章のとおり入力は DirectInput 経由で
`DirectInputHook::Poll()` → `GC::WriteInput` の経路を通り、WndProc を通らない。
`MbaaPatcher` がゲーム自身の入力読み取りを潰しているので、そもそもゲームは WndProc の
キーメッセージを見ていない。

| 経路 | UI に奪われるか | 影響 |
|---|---|---|
| ホットキー（WndProc / WM_KEYDOWN） | **奪われる** | F4・Ctrl+数字・Alt+数字がゲームに届かない |
| ゲームプレイ入力（DirectInput） | 奪われない | マッピング画面を開いていても**入力は書き込まれ続ける** |
| F12（中断） | どちらでもない | `GetAsyncKeyState(VK_F12)` を `SceneRunner::Step()` (H) が毎フレーム直接ポーリング（`Platform.cpp:120`） |

3行目の帰結として、**マッピング画面を開いている間もゲームには入力が流れ、対戦は進み続ける**。
UI 側は「全キーブロック」と書いてあるので、そう読むと事故る。

### CLI ランチャーの画面遷移

`MainController::Run()` の `AppState` 状態機械。TUI は `ConsoleRenderer` の ANSI 描画。

```
MainMenu ─┬─ Netplay [Hash Connect]  → NetplayConnection ─(成功)─→ GameRunning
          ├─ Training Mode [Offline] ────────────────────────────→ GameRunning
          ├─ Spectate [Watch a Match] → Spectating_WaitingForHost → GameRunning
          └─ Exit → Exit

NetplayConnection: 「ポート番号を入れれば HOST / ハッシュを貼れば JOIN」を入力内容で自動判別
                   （空 = HOST:7500、5桁以下の数字 = ポート、それ以外 = ハッシュ）
                   ESC でキャンセル → MainMenu

GameRunning:  IPC 書込み → GameLauncher::BootAndMonitor（注入）
              → syncCompleted を 200ms ポーリング（15秒で TerminateProcess）
              → WaitForSingleObject(INFINITE) でゲーム終了待ち
              → SharedState.lastErrorCode を読んでエラー画面 → MainMenu
```

ランチャーが利用者に見せる失敗はこの `lastErrorCode` の4種だけである
（`MainController.cpp:153-166`）。**`SyncTimeout` を書くコードは DLL 側に存在しない**
（`AUDIT_2026-08-13.md` §2 中）。つまり `[ SYNC TIMEOUT ]` の行は出るが、
`SessionErrorType::SyncTimeout` の分岐には到達しない。

### 他領域との境界

| 相手 | 境界 | 方向 |
|---|---|---|
| 01 プロセス | `IpcManager` / `SharedState`。起動設定を渡し `lastErrorCode` を受け取る | 双方向 |
| 02 フック | `DxHook`（EndScene/Present/Reset）、`WndProcHook`。UI はコールバックの受け手 | フック → UI |
| 03 ゲームメモリ | `GameMem().GameMode()` を `UiPhase` 変換と F4 のフェーズ判定に使う。**UI から書かない** | メモリ → UI |
| 04 フレーム空間と時間 | `SpeedFlags::RenderSkip` が UI の存在そのものを支配。`currentTickUs` を fps 表示に | 時間 → UI |
| 05 入力パイプライン | `DirectInputHook`（Poll / エッジ / デバイス列挙 / ReloadConfigs）。ini の書き手が UI、読み手が 05 | UI ⇄ 入力 |
| 06 ネットワーク | `NetplaySession::GetRttUs/GetThetaUs`。表示のみ | ネット → UI |
| 07 セッション状態機械 | `SetDelayFrames` / `SetMaxRollback` を UI がゲームスレッドから直接叩く（**B-2 の非 atomic 書込み経路**） | UI → セッション |
| 08 ロールバック | 現在なし。roadmap が要求する Rollup 処理時間の表示先はこの層 | （未実装） |
| 10 検証基盤 | `test_overlay` は**死んだ実装**のみを対象。生きた UI の検証は 0 | — |

---

## 現状の問題

| # | 問題 | 根拠 | 影響 |
|---|---|---|---|
| P-1 | ほぼ同一の重複実装が2組ビルドされている | `ControllerMapper.cpp` 542行 / `NetplayOverlay.cpp` 189行、`CMakeLists.txt:62,67` | **次に同じ場所を直す人が確実に踏む**。既に「直った実装と直っていない同名実装」が同居 |
| P-2 | `UiPhase::Rematch` が生成されない | `GameFrameOrchestrator.cpp:176-185` | リマッチ画面の UI が永久に出ない。`GamePhase::Rematch` は存在するのに |
| P-3 | ImGui / WndProc / DirectInput の初期化が `RenderSkip` の後ろ | `GameFrameOrchestrator.cpp:116-145` | FastBoot が止まると UI もコントローラも永久に死ぬ（B-1） |
| P-4 | `s_imguiFrameReady` を落とせるのが `OnPresent` だけ | `:87-92, 164-166` | `Present` が来なくなるとオーバーレイが静止したまま復帰しない |
| P-5 | F4 が全フェーズでブロックされる | `UIManager.cpp:108-115` | ゲーム本来の F4 機能を全期間失う |
| P-6 | UI が D/R を直接書く | `UIManager.cpp:45,56` ← ゲームスレッド、`SyncCodec.cpp:161` ← 通信スレッド | 非 atomic の 2 スレッド 3 経路（B-2）。背圧と readPos の乖離（B-3）の入口 |
| P-7 | 例外を握りつぶす | `GameFrameOrchestrator.cpp:216-218` の `catch (...) {}` | UI が落ちても誰も知らない。ログ 1 行もない |
| P-8 | 失敗が UI にもログにも出ない | 下記 | **本章最大の問題**。P-1〜P-7 より優先度が高い |
| P-9 | 生きた UI のテストが 0 | `src/tests/test_overlay.cpp` は `NetplayOverlay` を対象 | UI テストが緑でも保証範囲がゼロ |

### 2026-08-13 に直した保存バグ3件 — なぜそうなっていたか

いずれも「設定したのに効かない」としか現れず、**画面にもログにも何も出なかった**。

| バグ | 何が起きていたか | なぜそうなったか | 修正 |
|---|---|---|---|
| **F4 で閉じるとバインドが捨てられる** | `OnClose()` が `ResetBindingState()` → `SaveDeviceAllocations()` の順。前者が `binds[]` を空にするだけで `SaveBinds()` を通らない。**デバイス割当だけは保存される**ので「デバイスは設定されているのに操作が効かない」という最も分かりにくい形になる | 「Finish and Save」行への到達を唯一の保存経路と想定していた。ウィザードの途中で閉じる経路が設計に無かった | `EndUiSession()`（保存）→ `ResetBindingState()` の順に変更。`SaveBinds()` は空バインドを書かない（空＝そのセッションで到達しなかった項目、既存値を消さない）。画面に `[F4] Save and Close` を明記 |
| **開いて閉じるだけで割当が消える** | `s_pXJoyId` は起動時 `-1` 固定で、`P1Device`/`P2Device` から復元する処理が無い。その状態で開閉すると `SaveDeviceAllocations()` が空文字で上書き | 「UI の状態＝真実」と置いたが、UI は毎回ゼロから始まり ini の側が真実だった | `BeginUiSession()` で ini から復元し、かつ未割当時は既存値を潰さない。**両方**入れた（片方だけでは別の穴が残る）。ただし「未割当なら書かない」だけだと P1→P2 の移動で古い `P1Device` が残るため、「反対スロットに入ったことが確認できるときだけ消す」規則にした |
| **設定ファイルが相対パス** | 書き手が `"cccaster\\xxx.ini"`、読み手は絶対パス。ゲームが `SetCurrentDirectory` を呼ぶと**書けているのに読めない** | 読み手だけ先に絶対パス化し、書き手が取り残された | 書き手も `paths::Resolve()` に通す |

3件に共通する構造は**「書き手（UI）と読み手（`DirectInputHook`）が別々に真実を持っていた」**こと。
同じ 2026-08-13 の別バグ（`ConfigManager` が DLL プロセスで一度も `Load()` されない、
`P1Device`/`P2Device` の意味が UI と読み手で食い違う）も同型である。

---

## 目指す設計

### 設計原則

1. **UI の存在は FastBoot に依存させない。** ImGui は D3D デバイスに依存するが、
   `WndProcHook` と `DirectInputHook` は依存しない。後者を先に、`RenderSkip` の判定より前に初期化する。
2. **重複実装を木に置かない。** 「参考のために残す」は、次の作業者が grep で先に踏む限りコストの方が大きい。
3. **失敗は必ず画面かログのどちらかに出す。** 無言で 0 を返す・無言で `catch(...)` する経路を作らない。
   毎フレーム出す必要はない。**状態が変わったときに1回**出す（2026-08-13 のデバイス解決ログと同じ方針）。
4. **UI は表示と入力の受付に徹し、共有状態を直接書かない。** D/R は所有者経由で変更する（07 章）。
5. **UI が読む値の出所を1つにする。** 現在 `StateUiLogic` は `GameFrameOrchestrator` からしか
   更新されない。この単一の供給点を維持する。

### 変更点

| # | 内容 | 理由 |
|---|---|---|
| U-1 | **`ControllerMapper.*` と `NetplayOverlay.*` を削除**（ヘッダ・cpp・CMake エントリ・`test_overlay` / `stub_controller_mapper` ごと） | 到達不能なうえ現行実装とほぼ同名。**残す理由がない**。履歴は git にある。参考価値は「先に見つかって間違って直される」損害を上回らない |
| U-2 | `test_overlay` を生きた `StateUiLogic` / `ControllerUiLogic` に向け直す。特に `SaveBinds()` の「空は書かない」「Alt 既定値の実体化」「反対スロット移動時のみ消す」を固定 | U-1 でテストが消える。かつ 2026-08-13 の3件は**すべてテスト可能な純ロジック**だった。同じ穴を再発させない |
| U-3 | `WndProcHook::Initialize` / `DirectInputHook::Initialize` を `OnEndScene` の ImGui ブロックから出し、`SceneRunner::Init` 近傍（`RenderSkip` 判定より前）へ移す | B-1 / 2-6。FastBoot 中でもホットキーとデバイス列挙が生きる |
| U-4 | ImGui 初期化のみ `OnEndScene` に残すが、`RenderSkip` の早期 return より**前**に置く | 描画はスキップしてよいが、コンテキストの生成をスキップする理由はない |
| U-5 | `GameFrameOrchestrator` の `GameMode → UiPhase` に Rematch を追加するか、`UiPhase::Rematch` と `Rematch_Ui_View` を削除する。**どちらかに倒す**（未確定事項参照） | 「分岐はあるが到達しない」を残さない |
| U-6 | F4 のブロックをキャラセレに限定。`return 1` を返すのは実際にトグルしたときだけ | ゲーム本来のキーを奪わない |
| U-7 | `s_imguiFrameReady` にフレーム番号を持たせ、`OnEndScene` 側でも「前フレームのものなら破棄して作り直す」 | P-4。`Present` が来なくても静止しない |
| U-8 | `catch (...)` にログを出す（1回のみ、抑止付き） | P-7。UI の例外を無言で捨てない |
| U-9 | **失敗表示の追加**（下表） | P-8 |

### 失敗を利用者に見せる

現状オーバーレイに出ているのは D / R / μs / fps / Δ / RTT / JTR の**すべて「正常時の数値」**である。
異常時に表示が変わる箇所は `Δ` が ±5ms を超えると赤くなる 1 箇所だけ（`State_Ui_View.cpp:103-105`）。

| 見せるもの | 出所 | 表示 | なぜ必要か |
|---|---|---|---|
| ローカル入力デバイス未解決 | `DirectInputHook::GetLocalPlayerInput` が `joyId<0` | キャラセレバーに赤字 `NO DEVICE — press F4` | 2026-08-13 のバグはここが見えていれば即断できた |
| 相手からのパケット断 | `_framesSinceLastRecv` / `s_lastPacketReceiveTimeMs` | 秒数付きで `PEER LOST 1.2s` | 現状は 3 秒で無言終了 |
| 背圧による停止 | `SceneRunner` の `s_stallFrames` / `s_starvedFrames` | `WAITING FOR PEER (N frames)` | 「相手が止まった」と「自分が待たされている」は利用者から区別できない |
| 入力枯渇 | 同上 | 同上 | |
| セッションエラー | `SharedState.lastErrorCode` | ゲーム内にも出す（現在はランチャー画面に戻らないと見えない） | ゲームが閉じてからでは原因が分からない |

### ロールバック実装時に必要な表示

`docs/design/roadmap.md:40` が **「デバッグ版において Rollup（高速再計算）にかかる処理時間を
画面に表示せよ」** と要求している。08 章の実装に合わせて `InGameUiView` を拡張する。

| 項目 | 単位 | 目的 |
|---|---|---|
| 直近の Rollup 発生フレーム数 | frames | 巻き戻し量。D の調整指標 |
| Rollup 所要時間 | μs（直近 / 最大） | **16666μs を超えたらフレーム落ちが確定する**。等倍維持の可否がここで分かる |
| 直近 1 秒の Rollup 回数 | 回 | 予測ミス率。回線品質の実感値 |
| 予測ミス率 | % | 同上 |

**表示は「デバッグ版のみ」ではなく常時にする。** 利用者が「重い」と言うとき、
Rollup が原因かネットワークが原因かを切り分ける手段が他にない。
ただし既定は簡略表示とし、詳細はホットキー（例: `Ctrl+Alt+D`）でトグルする。

### 移行手順

| # | 手順 | 前提 | 検証 |
|---|---|---|---|
| 1 | U-1 削除（`ControllerMapper` / `NetplayOverlay` / 対応テスト） | なし。今日できる | ビルドが通る。UI の挙動は変わらないはず（到達不能なので） |
| 2 | U-2 生きた UI のテストを新設 | 1 | `ctest` で 2026-08-13 の3件が回帰しないこと |
| 3 | U-3 / U-4 初期化順序の分離 | なし | FastBoot 中に F4 と `WM_DEVICECHANGE` が効くこと（実機） |
| 4 | U-6 F4 のブロック限定 | 3 | キャラセレ以外で F4 がゲームに届くこと（実機） |
| 5 | U-9 失敗表示。まず「デバイス未解決」と「PEER LOST」の2つ | 3 | ケーブルを抜いて表示が出ること |
| 6 | U-5 Rematch の去就決定 | 07 章のフェーズ定義の一本化 | — |
| 7 | U-7 / U-8 描画順序の堅牢化 | なし | Alt+Tab / 解像度変更で静止しないこと |
| 8 | Rollup 表示 | 08 章の実装 | harness で数値が出ること |

順序の理由: **1 と 2 を先にやる。** 以降の変更はすべて `Controller_Ui_*` / `State_Ui_*` に入るので、
重複実装が残っていると「どちらを直したか」が毎回争点になる。

---

## 未確定事項

| # | 事項 | 決めるのに必要なもの |
|---|---|---|
| 1 | **リマッチ画面の UI を出すのか**。`GamePhase::Rematch` は `MatchScene::OnRematch` が自動ナビまで実装しているが、UI 側は到達不能なプレースホルダ。自動ナビと手動選択 UI は競合する | 07 章のリマッチ同期の設計確定 |
| 2 | MBAA 本体の **F4 / Ctrl+数字 / Alt+数字 の本来の機能**。奪ってよいキーなのか。推測: F4 は何らかのウィンドウ操作に割り当てられている可能性があるが未確認 | 実機で DLL 未注入時の挙動を確認 |
| 3 | `ImGui_ImplWin32_WndProcHandler` が `WM_KEYDOWN` に対して非 0 を返すか。返すなら (2) 以降のホットキー処理が丸ごと到達不能になる。推測: 現行 ImGui は key 系で 0 を返すのでホットキーは動いている（実際に動作報告がある）が、ImGui のバージョン更新で壊れうる | 使用中の ImGui バージョンの `imgui_impl_win32.cpp` を確認。FetchContent なのでバージョン固定も併せて要検討 |
| 4 | オーバーレイの表示可否をゲーム内から切り替える手段。配信時に RTT を隠したい要求がありうる | 利用者要望 |
| 5 | Rollup 表示の常時化がフレーム時間に与える影響。ImGui の文字列生成は毎フレーム走る | 実測（等倍ゲート） |
| 6 | `StateUiLogic` の static 変数群は非 atomic。現在は全部ゲームスレッドから触るので問題ないが、通信スレッドから直接更新したくなった時点で壊れる | 08 章の Rollup 計測をどのスレッドで取るか |
