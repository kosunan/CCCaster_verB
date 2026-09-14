# クラス設計の棚卸し（2026-09-10）

`src/` 配下の全ヘッダ・実装を読み、クラス単位で責務・依存・生死を確認した記録。
根拠はすべて 2026-09-10 時点のコード。`docs/` の他ドキュメントは参照していない。

対象: 実装 76ファイル / クラス・構造体 58個 / うち DLL 本体 (`core_dll/`) が 41個。

---

## 0. 結論の要約

| # | 所見 | 影響 |
|---|---|---|
| 1 | `network/` と `sync/` が相互 include で**循環している** | どちらを先に読んでも意味が取れない。分離してテストできない |
| 2 | 通信層 (`SyncCodec` / `NetplaySession`) が**表示層 `StateUiLogic` を直接叩く** | 通信の単体テストに UI がついてくる |
| 3 | DLL が**別バイナリのクラス `main_app::ConfigManager` を include** している | プロセスをまたいで同じクラスが別々のグローバル状態を持つ |
| 4 | `dllmain.cpp` が `mbaa_mem/` にあるが実体は**全レイヤの合成点** | 「ゲームメモリ層」を読むと全レイヤが降ってくる |
| 5 | クラス58個中 **22個が全 static**、さらに**シングルトンが5個** | 状態がグローバルに散り、初期化順序が暗黙 |
| 6 | `MatchScene`（10メソッド）と `NetplayOverlay`+`ControllerMapper` が**完全な死体** | 約1,700行 |
| 7 | `MatchInputBuffer` の公開APIの**約半分がテストからしか呼ばれない** | 旧ロールバック設計をテストだけが固定している |
| 8 | `SpeedFlags` の2フラグのうち **`TickBypass` は読み手がゼロ** | 「2フラグ構成」というドキュメントが実態と違う |

---

## 1. レイヤ構造の実態

`core_dll/` はフォルダ名でレイヤを表現している。想定の下→上はこう読める。

```
common → mbaa_mem → hook → timing → network → sync → rollback → engine → ui
```

実際の include を数えると、**下位が上位を include する「逆流」が 28本**ある。
そのうち設計上の問題として実害があるのは以下。

### 1.1 `network/` ↔ `sync/` の循環

```
network/SyncCodec.hpp   → sync/NetplayClock.hpp      （ヘッダ同士）
sync/NetplaySession.hpp → network/SyncCodec.hpp      （ヘッダ同士）
network/SyncCodec.cpp   → sync/NetplaySession.hpp
network/SyncCodec.cpp   → sync/MatchInputBuffer.hpp
network/PacketRouter.cpp→ sync/NetplaySession.hpp
```

原因は `NetplayClock` の**置き場所が間違っている**こと。
このクラスの namespace は `cccaster::core::timer` で、中身は「θ推定とα算出の純粋関数群、スレッドもI/Oも持たない」（`NetplayClock.hpp:6-9`）。
`timing/` に属するべきものが `sync/` にある。ここを移すだけで `network → sync` のヘッダ依存が1本消える。

残る `SyncCodec.cpp → NetplaySession.hpp` / `MatchInputBuffer.hpp` は、
「パケット解析器が受信結果を書き込む先を自分で知っている」という設計そのものの向き。
本来は `SyncCodec` がデコード結果を返し、`NetplaySession` が書き込む向きになる。

### 1.2 通信層が UI を直接呼ぶ

```
network/SyncCodec.cpp   → ui/State_Ui_Logic.hpp
sync/NetplaySession.cpp → ui/State_Ui_Logic.hpp
engine/GameFrameOrchestrator.cpp:206-209 → StateUiLogic::SetFps/SetFrameTimeUs/SetTimeOffsetMs/UpdateNetworkMetrics
```

同じ「メトリクスを UI に流す」処理が **通信層とengine層の両方**にある。
`UIManager::UpdateNetworkMetrics()` という委譲メソッドも用意されているが、こちらは**呼び出し元ゼロ**。
つまり供給経路が3系統に分裂し、うち1本は死んでいる。

### 1.3 DLL がランチャーのクラスを使う

```
core_dll/hook/DirectInputHook.cpp → cli_launcher/ConfigManager.hpp
core_dll/ui/Controller_Ui_Logic.cpp → cli_launcher/ConfigManager.hpp
core_dll/mbaa_mem/dllmain.cpp     → cli_launcher/ConfigManager.hpp
core_dll/CMakeLists.txt:71        → ../cli_launcher/ConfigManager.cpp を DLL にコンパイル
```

`ConfigManager` は全 static のグローバル設定ストア。
DLL とランチャーは**別プロセス**なので、同じクラスが2つの独立したグローバル状態を持つ。
実際の受け渡しは ini ファイル（`cccaster_v10.ini`）と共有メモリ（`IpcData.hpp`）で行われている。
つまり `ConfigManager` はプロセス間契約ではなく「ini パーサ」でしかない。
`shared_contracts/` に ini アクセスを置くか、DLL 側専用の薄い設定読み取りを分けるのが素直。

### 1.4 `dllmain.cpp` の置き場所

`core_dll/mbaa_mem/dllmain.cpp` (17KB) は以下をすべて include する。

```
cli_launcher/ConfigManager, engine/GameFrameOrchestrator, engine/MatchContext,
engine/SceneRunner, hook/DxHook, hook/TimeHooks, network/NetplayManager,
shared_contracts/IpcData, common/DataPaths, common/LogSink, ...
```

やっていることは composition root（IPC読み取り → MatchContext構築 → 各層の初期化順序決定）。
`mbaa_mem/`（ゲームメモリ層）にあるべきものではない。

### 1.5 フック層が UI を直接呼ぶ

`DxHook` は関数ポインタでコールバックを受ける形になっており、ドメイン層への依存を持たない（`DxHook.hpp:32-34`）。設計としては正しい。
一方 `WndProcHook.cpp:43` は `UIManager::HandleWndProcMessage()` を**直接呼んでいる**。同じ hook/ の中で方針が2つある。

---

## 2. クラス別の所見

### 2.1 `common/` — OS・基盤

| クラス / モジュール | 評価 |
|---|---|
| `platform` (関数群) | **良い。** `windows.h` をヘッダに漏らさない方針が明記され守られている（`Platform.hpp:19-21`）。`SleepMs` と `RealSleepMs` の区別も文書化済み |
| `core::log::LogSink` (関数群) | **良い。** 目的（同期I/Oがフレームタイミングを汚染する）と方針が書かれ、Win32非依存も守られている |
| `core::paths::DataPaths` | **良い。** ただし**利用が徹底されていない**（後述 2.7） |
| `DebugLog.hpp` | `HookLog` を extern 宣言してラップするだけ。namespace が `cccaster::domain::session` になっており、common に置くには不適切 |
| `ScriptedInput.hpp` / `TimeScale.hpp` | namespace `cccaster::testing`。テスト専用のものが本番ツリーの common にある。`ScriptedInput.hpp` は `mbaa_mem/` を include しており common の階層を破っている |

**問題**: `common/` の namespace が5種類（`core::log` / `core::paths` / `domain::session` / `platform` / `testing`）に分裂している。「共通の土台」という括りが名前空間として存在しない。

### 2.2 `mbaa_mem/` — ゲームメモリ

| クラス | 評価 |
|---|---|
| `IGameMemory` | **このプロジェクトで最も設計の良い部分。** 抽象化の理由・実装が2つだけであること・仮想呼び出しコストの見積り・seam に含めないものまでヘッダに書かれている（`IGameMemory.hpp:5-22`）。ゲーム無しでテストできる根拠がここにある |
| `RealGameMemory` | `IGameMemory` の実装。`final` 指定あり。ただし `rollback/GameSnapshotLayout.hpp` を include しており、ゲームメモリ知識が rollback/ 側に置かれている（配置の逆転） |
| `PhaseMonitor` | **名前が3つある。** ファイル `GamePhaseDetector.hpp` / クラス `PhaseMonitor` / 実装 `PhaseMonitor.cpp`。grep が取りこぼす。さらにヘッダの doc コメント（`GamePhaseDetector.hpp:71-73`）が `IntroState` の意味を正しく書いている一方、AGENTS.md は「ヘッダが誤り」と記録している — **現時点ではヘッダ側が修正済みで、AGENTS.md の記述が古い** |
| `GameInput` | struct。`Pack`/`Unpack` のみ。素直 |
| `MemoryPatcher` | 143行のテンプレート群。うち `ReadMemory` / `WriteMemory` は**完全未使用**（NetplayManager.hpp:23 に「削除した」と書かれているのに残っている） |
| `MbaaPatcher` | 起動時パッチ1メソッドのみ。責務が明確 |
| `MbaaMemTrace` | 観測専用。「seam の外に置く」理由がヘッダに明記されており良い |
| `dllmain.cpp` | 前述のとおり配置が誤り |

### 2.3 `hook/` — API フック

| クラス | 評価 |
|---|---|
| `DxHook` | **良い。** vtable hook の手順がヘッダに書かれ、コールバック型でドメイン依存を切っている |
| `TimeHooks` | Windows 専用を `#ifdef _WIN32` でファイルごと囲む方針。`RealQPC` 等のフック前APIも提供しており設計意図は明確。ただし `SetSleepBypass(true)` が `dllmain.cpp:221-222` で恒久設定され、**自DLLの `Sleep` も潰す**副作用が残っている |
| `DirectInputHook` | 全 static・16メソッド。デバイス列挙・エッジ検出・マッピングウィザード・テストモード注入が1クラスに同居。`GetLocalPlayerInput()` のコメントが「`GetPlayerNInput()` を直接呼ぶな」と警告しているのは、APIの粒度が誤っているサイン |
| `WndProcHook` | 2メソッドのみ。ただし UI を直接呼ぶ（1.5） |

### 2.4 `timing/` — 時間

| クラス | 評価 |
|---|---|
| `WasapiClock` | シングルトン。QPC フォールバック方針が明記されており良い。COM オブジェクトを `void*` で持つのはヘッダ汚染を避けるためで妥当 |
| `Metronome` | 責務が「精密待機の提供のみ、フレーム番号は持たない」と明記され、実際そうなっている。**良い** |
| `SpeedFlags` | **`TickBypass` の読み手がゼロ。** `SetHighSpeed()`/`SetNormalSpeed()` が書き込むだけ。ヘッダの「3. TickBypass=true → DLLスレッドが通信スレッドのポーリングをスキップ」は現在の実装に存在しない。実質1フラグ構造体 |

### 2.5 `network/` — 通信

| クラス | 評価 |
|---|---|
| `UdpSocket` | pImpl で asio を隠蔽。**良い。** ただしヘッダのコメントが「後ほど cpp ファイル側で定義」という執筆中の口調のまま残っている |
| `NetplayManager` | シングルトン。実質「UdpSocket のライフサイクル + 送信ラムダ」だけ。公開 getter 5個のうち `GetUdpSocket` / `IsHost` / `GetTargetIp` / `GetTargetPort` は**すべて未使用**。ヘッダのコメントは「NetplaySession 向けソケット直接アクセス」と書いているが、NetplaySession は使っていない |
| `PacketRouter` | 1メソッドのみ。ヘッダ通り「検証して NetplaySession に転送」。素直 |
| `SyncCodec` | **責務が広すぎる。** ヘッダ自身が6項目を列挙している（解析・θ計算・α1・α2・バッファ書込み・パケット組立て・D/R dirty管理）。実際にはさらに UI 更新と `MatchScene` include も抱える。名前は「Codec」なのに符号化以外が大半 |
| `NetworkSimulator` | シングルトン。設計は素直。ただし**対戦パケットには効いていない**（DLL 側で有効化されていない、AGENTS.md 記載どおり）。ログ用 getter 3個は未使用 |

### 2.6 `sync/` — 同期

| クラス | 評価 |
|---|---|
| `FrameSequence` | 34行。世代 (`epoch`) × STRIDE でフレーム番号空間を切る。単一責務、状態は4変数、副作用なし。**このプロジェクトで最もきれいなクラス** |
| `BoundedWait` (関数テンプレート) | 時計・待機・疎通を全部注入する形。実時間に依存せず検証できる。**良い設計** |
| `MatchInputBuffer` | 280行のシングルトン。レーン分離と64bitスナップショットで競合を構造的に排除している設計自体は良い。**ただし公開APIの約半分が死んでいる**（2.8 参照） |
| `NetplayClock` | 純粋計算エンジンとして責務は明確。**置き場所が誤り**（1.1）。`GetDriftRate()` は `return 0.0;` のスタブが残っている |
| `NetplaySession` | シングルトン。`SharedSyncState` が **28個のフィールドを持つ巨大な共有構造体**。うち `peerPhaseBaseFrame` / `phaseBaseFrame` / `localPhaseKind` / `peerPhaseKind` / `localRetryMenuIndex` は書かれるだけか読まれるだけの片道フィールド。スレッド間契約が構造体1つに全部詰まっており、どのフィールドをどのスレッドが書くかはコメントでしか表現されていない |

### 2.7 `engine/` — フレーム進行

| クラス | 評価 |
|---|---|
| `SceneRunner` | 公開APIは4つだけで、ヘッダは薄く保たれている。**実装側は匿名 namespace にファイルスコープ変数14個**、`Step()` は210行の単一関数。2026-09-10 の bounded lockstep + rollback で全面書き換えされた直後 |
| `GameFrameOrchestrator` | **責務が6つ**（コールバック登録 / ImGuiライフサイクル / WndProc・DirectInput初期化 / バックバッファ判定 / GameMode→UiPhase変換 / メトリクス供給）。ヘッダは3つと書いている |
| `FrameControl` | 「制御層 / プリミティブ層の2層」と書かれているが、実装は全部 inline の薄い委譲4本のみ。プリミティブ層は `IGameMemory` に移管済みで、**このクラスは既に役目を終えかけている**。`SetRenderSkipByGap()` は完全未使用 |
| `MatchContext` | POD。`charaSelectSyncDone` / `roundStartSynced` / `fastBoot` / `rollbackReady` の4フラグは**どこからも書かれていない** |
| `SceneFastBoot` | 4メソッド。責務明確 |
| `SceneInputFilter` | **良い。** 「なぜ送信前に適用するのか」（決定性が壊れる理由）がヘッダに論理として書かれている。単体テスト16ケースあり |
| `MatchScene` | **完全な死体。** 10メソッドすべて外部呼び出しゼロ |

### 2.8 `rollback/` — ロールバック

| クラス | 評価 |
|---|---|
| `PredictionHistory` | 44行。リングバッファ32、予測・照合・リプレイ解決。副作用なしでテストしやすい。**良い** |
| `RollbackStates` | 24行。スロット12。`std::fenv_t` まで保存/復元しているのは正しい判断 |
| `ReplayEffects` | 8行のヘッダ |
| `RollbackEngine` / `StateBuffer` / `StateRingBuffer` / `MemDumper` / `DumpEntryList` | **ビルド対象外**（`CMakeLists.txt:51-55` に理由が明記）。16bit入力前提の旧設計 |
| `GameSnapshotLayout.hpp` (82KB) | アドレステーブル。`rollback/` にあるが内容は完全にゲームメモリ知識 → `mbaa_mem/` が正しい置き場所 |

**二重実装**: ロールバックの「予測が外れたフレームの検出」が2箇所にある。

- `MatchInputBuffer::RecordMismatch` / `HasMismatch` / `ConsumeMismatch` / `predictedRemote` / `rollbackable` — 旧設計。**本番から一度も呼ばれない**（テストのみ）
- `PredictionHistory::Reconcile` — 現行。`SceneRunner.cpp:138,290` が使う

テストだけが旧設計を生かしている状態。

### 2.9 `ui/` — 表示

現行の生きた経路:

```
GameFrameOrchestrator::OnEndScene
  → UIManager::Render(UiPhase)
      ├ CharaSelectUiView::Draw()   → StateUiView::Draw*Bar/Popup / ControllerUiView::Draw()
      ├ InGameUiView::Draw()        → StateUiView::DrawInGameBar()
      └ RematchUiView::Draw()       ← 到達不能
```

| クラス | 評価 |
|---|---|
| `UIManager` | Render は素直なディスパッチ。`HandleWndProcMessage()` が同居しているのは責務違い（フックの戻り値規約 `>0/0/-1` を UI が持っている）。`UpdateNetworkMetrics()` は未使用 |
| `StateUiLogic` | 全 static・20メソッド。D/R値・ポップアップタイマー・ping/jitter履歴・fps・θ・マッピング開閉フラグを1クラスで保持。「UI状態の一元管理」としては筋が通っている |
| `StateUiView` | 描画4本のみ。Logic/View 分離が守られている。**良い** |
| `ControllerUiLogic` / `ControllerUiView` | Logic/View 分離が守られている。`OnClose()` のコメントに「EndUiSession → ResetBindingState の順序が重要」と過去バグの理由が書かれているのは良い。`GetP1CachedEdge` / `GetP2CachedEdge` は宣言されているが View が使っていない |
| `OverlayRenderer` | 4メソッドの描画基盤。責務明確。ヘッダのコメントが削除予定のクラス名（`NetplayOverlay`, `ControllerMapper`）を参照している |
| `CharaSelectUiView` / `InGameUiView` | 数行のディスパッチ。適切 |
| `RematchUiView` | **到達不能。** `UiPhase::Rematch` を生成する場所が無い（`GameFrameOrchestrator.cpp:174-183`） |
| `NetplayOverlay` / `ControllerMapper` | **完全な死体。** 互いにしか参照しない閉じた島。`ControllerMapper.cpp` の542行中350行が `Controller_Ui_Logic.cpp` + `Controller_Ui_View.cpp` と literal で重複 |

**設定パスの不徹底**: `DataPaths` は「相対パスはゲームのカレントディレクトリ基準になり危険」という理由で作られた（`DataPaths.hpp:6-15`）。`Controller_Ui_Logic` / `DirectInputHook` / `dllmain` は `paths::Resolve()` を通しているが、`ControllerMapper.cpp:107,120,199,217` は生の `"cccaster\\cccaster_v10.ini"` を使い続けている（死んだファイルなので実害はないが、削除の根拠になる）。

### 2.10 `cli_launcher/` — ランチャー

| クラス | 評価 |
|---|---|
| `MainController` | 状態機械（`AppState` 5状態）。`MenuOption` に `std::function` を持たせる形。18KB。CLI・ネゴシエーション・ゲーム起動監視を1クラスで抱えている |
| `ConnectionHash` | 16KB。Base32 / XOR / セッショントークン生成。暗号処理を自前実装しているのはリスクだが、用途（接続情報の短縮共有）を考えれば妥当 |
| `SessionNegotiator` | 19KB。STUN 相当の外部IP取得 + ホール開け |
| `ConsoleRenderer` | 全 static・描画のみ。適切 |
| `Config` / `ConfigManager` | `Config` はインスタンス、`ConfigManager` はその static ラッパ。二重になっている。`SetInt` は両方とも未使用 |

### 2.11 `shared_contracts/` — プロセス間契約

`IpcData.hpp` (214行)。`SharedState` 構造体と `IpcManager`。
**プロセス間の唯一の正しい契約**であり、ここに置かれているのは正しい。
`IpcManager::UpdateOrReadState()` が関数ポインタ API になっているのは、DLL とランチャーで別々にコンパイルされる前提を考えると妥当な判断。

---

## 3. 横断的な所見

### 3.1 全 static クラスが22個

```
ui/         : UIManager, StateUiLogic, StateUiView, ControllerUiLogic, ControllerUiView,
              OverlayRenderer, NetplayOverlay†, ControllerMapper†
engine/     : SceneRunner, GameFrameOrchestrator, SceneFastBoot, SceneInputFilter, MatchScene†
hook/       : DxHook, TimeHooks, DirectInputHook, WndProcHook
mbaa_mem/   : MbaaMemTrace, (PhaseMonitor, MbaaPatcher)
cli_launcher: ConfigManager, ConsoleRenderer, ConnectionHash
shared      : IpcManager
                                                          († = 削除候補)
```

DLL 内で1インスタンスしか存在しないものが大半なので方針自体は一貫している。
問題は**初期化順序と生存期間が型で表現されない**こと。実際、`SceneRunner::IsReady()` / `SceneFastBoot::IsComplete()` / `s_imguiInitialized` / `s_uiSessionActive` のような「初期化済みフラグ」が各所に手書きされている。

### 3.2 シングルトンが5個

`NetplaySession` / `MatchInputBuffer` / `NetplayManager` / `NetworkSimulator` / `WasapiClock`。
うち `MatchInputBuffer` と `NetplaySession` は**2スレッドから触られる可変状態**を持つ。
`Reset()` はあるが、テストでセッションを2つ作ることはできない。harness が1プロセス1セッションに縛られているのはこれが理由。

### 3.3 ドキュメントとコードの乖離

ヘッダの設計コメントは総じて質が高い（「なぜそうしたか」が書かれている）が、実装が動いた後に更新されていない箇所がある。

| 場所 | 記述 | 実際 |
|---|---|---|
| `SpeedFlags.hpp:9` | TickBypass で通信ポーリングをスキップ | 読み手ゼロ |
| `FrameControl.hpp:11-16` | 2層構成（制御層 + プリミティブ層） | プリミティブ層は `IGameMemory` に移管済み |
| `GameFrameOrchestrator.hpp:8-21` | 責務は3つ | 実際は6つ |
| `NetplayManager.hpp:14` | NetplaySession 向けソケット直接アクセス | 未使用 |
| `ControllerMapper.hpp:33` | NetplayOverlay::Render() から呼ばれる | 両方とも死んでいる |
| `Controller_Ui_View.hpp:8` | 既存 ControllerMapper をラップ | 移植完了済みでラップしていない |
| `OverlayRenderer.hpp:11` | NetplayOverlay, ControllerMapper から利用可能 | 両方削除候補 |
| `UdpSocket.hpp:23` | 「後ほど cpp ファイル側で定義」 | 定義済み |
| `AGENTS.md`（`IntroBarrier`/`phaseBaseFrame`） | MatchScene.cpp:100-130 が原因 | そのコードは実行されない |
| `AGENTS.md`（`(F2)`/`(G)` ラベル重複） | SceneRunner.cpp に重複ラベル | 現行ファイルにラベル自体が無い |

### 3.4 本番から呼ばれない public API（テスト・コールバック登録を除く）

```
FrameControl::SetRenderSkipByGap        完全未使用
SpeedFlags::TickBypass                  読み手ゼロ
NetplayClock::GetDriftRate              return 0.0 のスタブ
MemoryPatcher::ReadMemory / WriteMemory 完全未使用
NetplayManager::GetUdpSocket / IsHost / GetTargetIp / GetTargetPort
NetworkSimulator::GetMinDelayMs / GetMaxDelayMs / GetLossPercent
UIManager::UpdateNetworkMetrics
ControllerUiLogic::GetP1CachedEdge / GetP2CachedEdge
Config::SetInt / ConfigManager::SetInt
IpcManager::MappingName
MatchInputBuffer::TryReadForGame / HasLocal / HasRemote / GetReadPos /
                  GetEffectiveHead / HasMismatch / ConsumeMismatch /
                  ConfirmConflicts / HasConfirmedRemote /
                  GetConfirmedRemoteFrame / SetWriteHead / IsRollbackable
MatchScene::（10メソッド全部）
NetplayOverlay::（Draw系3本 + Render）
```

`MatchContext` の未書き込みフィールド: `charaSelectSyncDone` / `roundStartSynced` / `fastBoot` / `rollbackReady`。

---

## 4. 是正候補（依存関係の順）

**A. 削除（挙動変化ゼロ・約1,700行）**
`MatchScene` / `NetplayOverlay` / `ControllerMapper` と、それ専用のテスト
(`test_overlay.cpp` は `add_test()` に登録されておらず ctest では走っていない) 、
`stub_controller_mapper.cpp` / `stub_hooklog.cpp`。あわせて上記の未使用 public API と
`MatchContext` の死にフィールド。

**B. 配置の是正（include だけの変更）**
`NetplayClock` → `timing/`（`network/`↔`sync/` の循環が1本消える）。
`GameSnapshotLayout.hpp` → `mbaa_mem/`。
`dllmain.cpp` → 独立した composition root へ。
`ScriptedInput.hpp` / `TimeScale.hpp` → テスト側へ。

**C. 責務の分離**
`GameFrameOrchestrator` から ImGui ライフサイクル・UiPhase変換・メトリクス供給を剥がす
（結果として `engine → ui` と `engine → mbaa_mem` の逆流が消える）。
メトリクス供給を `StateUiLogic` 側の1経路に集約する
（`SyncCodec` / `NetplaySession` から `ui/` への include が消える）。

**D. ドキュメント同期**
3.3 の表の各箇所と AGENTS.md。

**E. 保留を推奨**
`SceneRunner` / `SyncCodec` / `NetplaySession` / `MatchInputBuffer`。
2026-09-10 に bounded lockstep + rollback で書き換えられた直後で、
今リファクタリングすると変更差分が読めなくなる。
`MatchInputBuffer` の旧ミスマッチ機構の削除は、それを固定しているテストの
書き換えとセットになるため、ロールバックの実装が落ち着いてから。
