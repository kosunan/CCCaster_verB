# 現行コードの構造（2026-09-11 時点）

`src/` を読み直して、いま何がどう動いているかを整理した文書。
性能の実測値は `docs/CURRENT_STATE.md`、未解決は `docs/OPEN_ISSUES.md` にある。
ここは「コードがどう組まれているか」だけを書く。

---

## 1. 成果物

| 成果物 | 中身 | 置き場所 |
|---|---|---|
| `cccaster_hook.dll` | ゲームプロセスに注入される本体。対戦ロジックの全部 | `src/core_dll/` |
| `CCCaster_v10.exe` | CLI ランチャー。募集・参加・ゲーム起動・注入 | `src/cli_launcher/` |
| `CCCaster_v10_GUI.exe` | GUI ランチャー（新規）。英語既定、日英切替 | `src/gui_launcher/` |
| `cccaster_transport` (静的lib) | `UdpSocket` + `NetworkSimulator`。DLL とランチャーが共有 | ルート `CMakeLists.txt:110` |
| `harness` | ゲーム無しで同期ロジックを走らせる実行ファイル | `src/harness/` |
| CTest 23本 | 純ロジックの単体テスト | `src/tests/` |

プロセス間の契約は `src/shared_contracts/` に集約されている。
昨日は `IpcData.hpp` だけだったが、いまは 8ファイル：
`IpcData` / `GameBuild` / `GameImageAddress` / `NetplaySettings` / `SessionClosePacket` / `SpikeDebugGate` / `StartupNegotiation` / `StartupTrace`。
D/R の既定値と妥当性判定（`NetplaySettings::IsValid`）がここに移り、
DLL・CLI・GUI・テストが同じ定義を見るようになった。

---

## 2. スレッド構成

対戦中、DLL は 5本のスレッドを使う。

```
① ゲームスレッド     … MBAA 本体。D3D9 Present から SceneRunner::Step() が呼ばれる
② 入力時計スレッド    … NetplaySession::InputThreadMain → InputTimeline::Pump()
③ 通信スレッド        … NetplaySession::ThreadMain（送受信のみ）
④ WASAPI worker      … 無音供給と時計の相関採取（WasapiClock::Worker）
⑤ ログ書き出し        … LogSink のバックグラウンド writer
```

**いちばん大きな設計変更は ② の独立**。
以前は生入力の採取もゲームスレッド（`SceneRunner::Step()` の中）でやっていた。
いまは専用スレッドが WASAPI 由来の絶対締切で採取し、`MatchInputBuffer` に番号付きで置く。
ゲームスレッドは「保存済みの枠を順番に消費する」だけになった。
ゲームの処理が遅れても入力採取の間隔は乱れない。

②③ は `ThreadSignal`（Windows では高分解能 waitable timer + Event、それ以外は condition_variable）で寝起きする。
締切の 1ms 手前で起きて、最後だけ `CpuRelax()` でスピンする。

`TimingThread`（`common/Platform.hpp`）が ①②③ を MMCSS の Games/HIGH に登録し、
CPU アフィニティを維持する。

---

## 3. 時計の階層

いちばん下から積み上がっている。単位は基本 **1/60µs tick**（60Hz の 1F がちょうど 1,000,000 ticks）。

```
WasapiClock            IAudioClock の再生位置（オーディオDACの水晶）
  └ ClockContinuity    QPCで短区間を補間し、音声位相へ ppm 速度で調律
  └ ClockProjection    2スロット世代公開。他スレッドはロック無しで読む
      └ FrameCadence   1F = 1,000,000 ticks を整数で正確に加算（丸め誤差を溜めない）
      └ PhaseFollower  ホスト固定・クライアント追従。1F 合計 10µs 上限
      └ PeerClockModel 8秒窓の低RTT標本から θ とドリフト ppm を推定
```

- `ClockContinuity` は音声時刻へ**出力を直接合わせない**。誤差を ppm の速度差に変えて約1秒かけて吸収する。
  ±1% の速度異常、または 5ms 超の残差が1秒続いた場合だけ QPC へ固定切替し、起動中は復帰しない（`ClockContinuity::FallBack`）。
- `PhaseFollower` は「同方向の観測が2回」「誤差1ms以上」で追従を始め、0.25ms で止める。
  反対側へ越えた観測が来たら即座に逆補正せず、追従をリセットする（ハンチング防止）。
- `Metronome` は α1/α2 補正付きの間隔を提供するだけで、フレーム番号は持たない。

---

## 4. 1フレームの流れ

`DxHook` は起動 gate 付きランチャーなら `Direct3DCreate9` の IAT を捕まえ、
`CreateDevice` が返す**実デバイス**の vtable をフックする（旧来のダミーデバイス方式は失敗時のみ）。

```
Hooked_EndScene → OnEndScene
    ImGui 遅延初期化（初回）→ バックバッファ判定 → UIManager::Render()
    ここでは描画データを作るだけ（MBAA は 1F に ~113回 EndScene を呼ぶ）

Hooked_Present  → OnPresent
    ImGui を最上位レイヤーとして合成
    presentDueTicks（絶対締切）まで待つ ── 残り 1ms からスピン
    ↓
  【元の Present】完成画像が画面に出る
    ↓
                → OnAfterPresent          ← ★ここが新しい
    DirectInputHook::PollUi()
    SceneRunner::Step()                   ← 1フレーム分のロジック
```

**順序が変わったのが要点**。以前は Present の前に `SceneRunner::Step()` を呼んでいたので、
相手入力の待ちがそのまま表示の遅れになっていた。
いまは「完成画像を先に出してから、次フレームの入力待機と状態保存をやる」。

`OnPresentSkip` が true を返すと元 Present 自体を飛ばす（ロールアップ中の中間画像）。

---

## 5. `SceneRunner::Step()`

731行の単一関数（`SceneRunner.cpp:257-988`）。状態は匿名 namespace の `SceneRuntime` 構造体1つに集約されている。

経路が4本に分かれる。

```
(a) FastBoot            phase < CharaSelect かつ未完了
(b) キャラセレ独立選択   phase==CharaSelect かつ mem.HasIndependentSelect()
(c) 再戦メニュー独立     phase==Rematch    かつ mem.HasIndependentRetry()
(d) 対戦                上記以外（InGame 本体 + 旧経路）
```

(b)(c)(d) は同じ形をしている。

```
1. timeline.HasCaptured(frame) を待つ      … 入力時計が枠を埋めるまで
2. 相手の状態を読む                          … SelectionState / RetrySelection / MatchInputBuffer
3. 絶対締切まで待つ                          … CapturedDeadlineTicks(frame) + 3ms、最後の3msはスピン
4. FrameControl::WriteInput() でゲームへ書く
5. sequence.Commit() / timeline.SetConsumed()
```

3 の「採取予定時刻＋3ms」を更新開始の基準にするのが肝で、
**相手待ちや状態保存に掛かった時間を次の周期に足さない**。
処理が終わってから 16.667ms 待つ方式だと遅れが累積するが、その形を取っていない。

`Wait()` ヘルパー（`SceneRunner.cpp:127-228`）が待機を一手に引き受ける。
`BoundedWait` に「中断要求」「疎通」「期限」「時計」「待機方法」を注入する形で、
締切の残りが 3ms 以下ならスピン、それ以外は `PreciseWaitUs(500)` に切り替える。
タイムアウトは通常3秒、起動・合流は30秒。

---

## 6. 入力の経路

```
InputTimeline::Pump()  ［入力時計スレッド］
  DirectInputHook::Poll() → GetLocalPlayerInput(host, soloLocal)
  → LocalInputGate        F4設定中は遮断。閉じた後は全ボタン解放を待つ
  → SceneInputFilter      画面ごとの制約（送信前に適用＝両者が同じ値を受け取る）
  → SettingsCommands      キャラセレでは上位8bitに D/R 操作を載せる
  → MatchInputBuffer::WriteLocal(frame, value)
        │
        ├─［通信スレッド］writeHead の変化を見て SYNC_TICK 送信（最新10件を再送）
        │
        └─［ゲームスレッド］TryGetLocalInput / TryGetRemoteInput
              → SettingsCommands::Apply() で設定ビットを取り出す
              → & GameMask (0x00ffffff) で設定ビットを落とす
              → FrameControl::WriteInput() → GameMem().WriteInput()
```

`MatchInputBuffer` は 600スロットのリングで、local レーンと remote レーンを別スレッドが書く。
番号と値を 64bit atomic の1スナップショットとして公開するので、周回中でも番号と値が分離しない。

設定変更（Ctrl+0-8 で D、Alt+0-8 で R）は `SettingsCommands` が入力列の上位8bitに載せて運ぶ。
ホストが要求順を決め、確定値を次の世代入口で適用する。自分のキャラ確定後の新規変更は取り消される。

---

## 7. フレーム空間と世代

`FrameSequence` が番号空間を持つ。

```
epoch を1つ進める → base = epoch × 65536
次に消費するフレーム = base + 1 から
採取するフレーム     = Next() + Lookahead   （Lookahead = D）
```

世代（epoch）は画面が変わるたび、ラウンドが変わるたびに切り替わる。
番号空間ごと替えるので、前世代の遅れて届いたパケットが新世代を壊さない。

世代境界でやること（`SceneRunner.cpp:692-766`）：

1. 相手が旧世代の末尾を消費するまで待つ（末尾パケットを落とした相手を取り残さない）
   - ただし相手が**次の画面へ到達済み**なら打ち切る（`PeerEnteredNextPhase`）。再戦入口のデッドロック対策
2. `SettingsCommands::Boundary()` → 新しい D/R を確定
3. `sequence.Begin(delay)` で新しい番号空間へ
4. ホストが RNG 232B を採取、クライアントが待って書き戻す
5. `localPhaseToken`（base<<32 | phase）を公開し、相手の token が一致するまで待つ（世代バリア）
6. **InGame のみ** `EpochStartGate` で戦闘開始の絶対時刻を合意

`EpochStartGate`（`sync/EpochStart.hpp`）は5段階のハンドシェイク：

```
Ready → Offer(ホストが未来時刻を提案) → Accepted → Commit → Committed
```

提案する未来量は `RTT×6 + 200ms` を 0.5〜2.5秒に丸めた値。
合意した時刻から両者の入力時計を同時に開始するので、
ロード時間の差があってもフレーム0の物理時刻が揃う。

---

## 8. ロールバック

```
PredictionHistory  32エントリ。Record / Reconcile / Prediction / ResolveReplay
RollbackStates     12スロット。SaveSnapshot/LoadSnapshot + std::fenv_t も保存
```

流れ：

- 相手入力が未着で予測余裕があれば `history.Prediction()`（前回値）で進める
- 確定値が届いたら `Reconcile()` が最古の不一致フレームを返す
- 不一致があれば `mem.BeginReplay()` → `snapshots.Load()` で状態復元 →
  高速スキップモードで対象フレームまで1Fずつ再計算 → `mem.EndReplay()`
- 再計算中は `history.NeedsReplaySnapshot()` が **確定済みフレームの保存を省略**する
  （未確定フレームだけ保存。保存中央値 232→115µs）

`std::fenv_t` まで保存しているのは、浮動小数点の丸めモードが変わると再計算がずれるため。

上限は R フレーム、D+R ≤ 8。既定 D2/R4。

---

## 9. 通信

```
UdpSocket (asio, cccaster_transport)
  → PacketRouter        CC10統一ヘッダ(20B)を検証して NetplaySession へ
  → NetplaySession      送受信専念。WaitReady → WaitStart → Counting
      └ SyncCodec       解析・θ/RTT計算・α2・バッファ書込み・パケット組立て
      └ Metronome       フレーム間隔の提供
```

パケットは **SYNC_TICK 1種類だけ**。フェーズの違いは flags とフィールドで表す。通信版10、拡張3。

スレッド間の共有は `SharedSyncState`（`NetplaySession.hpp`）。
昨日から構造が変わり、**用途ごとに mutex で区切った領域**が増えた。

| 領域 | mutex | 中身 |
|---|---|---|
| 再戦 | `retryMutex` | `RetrySelection` local/peer |
| 戦闘開始合意 | `epochStartMutex` | `EpochStart` local/peer |
| キャラセレ | `selectionMutex` | `SelectionState` local/peer |
| 入力スケジュール | `scheduleMutex` | `InputSchedule`（枠番号と締切を**組で**公開） |
| RNG | `seedMutex` | `RngState` local/peer |
| その他 | atomic | token / consumedFrame / appliedFrame / protocolError など |

`InputSchedule` を組で公開するのが新しい点で、クライアントはここから相手の締切を読み、
`PhaseFollower` で自分の採取位相をゆっくり寄せる。

---

## 10. ゲームメモリ seam の拡張 ── 方針転換

`IGameMemory` が 14 → 26メソッドに増えた。増分はほぼ全部**ゲーム本来の画面を外から駆動する API**。

```
ConfigureNetplayMenu()                  ネット対戦メニューのフック設置
SetRetryTarget(int)                     VS RESULT MENU の選択先を固定
HasIndependentRetry() / BeginIndependentRetry() / ReadRetryChoice()
HasIndependentSelect() / BeginIndependentSelect(bool)
ReadLocalSelection(bool, SelectionState&)
DriveRemoteSelection(bool, const SelectionState&, uint32_t)
SetSelectionRelease(bool, uint32_t)
PrepareBattleAudio()                    開始演出前のSFX事前準備
```

これが今回いちばん大きな設計変更。

**以前**：キャラセレも再戦も、フレーム入力を1Fずつ同期して両者のゲームを同じに進めていた。
自前の ImGui パネルで再戦メニューを描いていた。

**いま**：キャラセレと再戦は**ローカルで自由に操作する**。
相手とは確定項目（キャラ / ムーン / カラー / ステージ / 再戦の選択）と ACK だけを交換し、
相手側の表示は `DriveRemoteSelection()` が生成する疑似入力でゲームに再現させる。
再戦はゲーム本来の VS RESULT MENU をそのまま使う。

効果として、相手が7秒無操作でも自分のキャラ選択は止まらない。
代わりに `SelectionState`（60B）と `RetrySelection`（12B）という**状態の再送**が必要になり、
`Accept()` に「順序逆転や古い通知で状態を戻さない」規則が入っている。

ランダムステージも、以前は「ランダム指定0を共有して各端末が別々に抽選」していたのを、
ホストが元ゲームの抽選を1回だけ行って**実ステージ番号を交換**する形に変わった。

---

## 11. 起動短縮とゲーム側の最適化

DLL に「MBAA 本体を速くする」層が増えた。

| ファイル | 役割 |
|---|---|
| `mbaa_mem/GameBuildGuard.hpp` | 対象版（Ver.1.07 Rev.1.4.0）以外を拒否 |
| `mbaa_mem/StartupPatch.hpp` | 起動時パッチ（暗転短縮・DxDiag省略など） |
| `mbaa_mem/StartupProfile.hpp` | 起動段階ごとの所要時間を採取 |
| `mbaa_mem/StartupAssets.cpp` | 素材変換結果をローカル DDS キャッシュへ。初回フレームで通常経路へ復元 |
| `mbaa_mem/SoundPrewarm.hpp` | 開始演出の合流前に停止済み SFX を最小音量で準備 |
| `hook/ScenePairMerge.hpp` | 命令列を照合してから、バッチ前後の End/Begin 対だけ統合（113→3組） |
| `timing/GameCpuGuard.hpp` | ゲームスレッドを CPU0 と同じ物理コアから外す（GPU割込み回避） |
| `timing/GameCoreLease.hpp` | 同一ログオンの CCCaster 同士で物理コア選択を調整（名前付き Mutex） |

`ScenePairMerge` と `StartupPatch` は**命令バイト列を照合してから**書き換える形になっていて、
版が違えば黙って何もしない。

---

## 12. 診断層

計測用のコードが独立した層として整理されている。共通する形は
**「固定長バッファへ数値だけ記録し、整形は締切の外でやる」**。

```
timing/SpinProbe.hpp             監視空白と締切後処理を分けて記録
hook/DriverLockProbe.hpp         CS/SRW 取得APIのロック待ちを検出
hook/RenderProbe.hpp             EndScene の呼び出し回数と省略候補
mbaa_mem/GaugeStress.hpp         ゲージ満タン化のスパイク再現
mbaa_mem/CombatStress.cpp        近距離連続ヒットの負荷再現
mbaa_mem/MbaaMemTrace.cpp        実機メモリを毎フレーム記録
common/DeferredNumericLog.hpp    締切内では数値だけ積み、後で整形
launcher/SpikeDebugger.cpp       EBP連鎖・PDB・所有者スレッドでスパイクを帰属
```

有効化はすべて環境変数か CLI 引数（`CCCASTER_SPIN_PROBE`、`--debug-spikes` など）で、
既定では動かない。`FrameTiming::PresentBudgetUs()` のように環境変数で上書きできる定数もある。

---

## 13. 昨日の監査からの差分

### 直ったもの

- **`MatchScene` 削除**。10メソッド全部が死んでいたクラスがツリーから消えた
- **`MatchContext` の死にフラグ4つ削除**（`charaSelectSyncDone` / `roundStartSynced` / `fastBoot` / `rollbackReady`）
- **`SceneRunner::SendFunc` 削除**。「レガシー、現在未使用」と書かれていた typedef
- **`SceneRunner` の散らばった static 変数 → `SceneRuntime` 構造体へ集約**。Init で `*this = {}` 相当に初期化できるようになった
- **D/R の定義が `shared_contracts/NetplaySettings.hpp` へ集約**。DLL・CLI・GUI・テストが同じ判定を使う

### 残っているもの

- `ui/NetplayOverlay.cpp` と `ui/ControllerMapper.cpp` は**まだビルド対象**（`core_dll/CMakeLists.txt:58,63`）。
  本番の呼び出し元はゼロのまま。`tests/test_overlay.cpp` と `stub_controller_mapper.cpp` も残っている
- **`network/` ↔ `sync/` の循環は解消されていない**。
  `network/SyncCodec.hpp` が `core_dll/sync/NetplayClock.hpp` を include し、
  `sync/NetplaySession.hpp` が `network/SyncCodec.hpp` を include する形が続いている。
  `NetplayClock` の namespace は今も `cccaster::core::timer`
- `cli_launcher/ConfigManager.cpp` が**まだ DLL にコンパイルされている**（`core_dll/CMakeLists.txt:66`）
- `dllmain.cpp` が `mbaa_mem/` のまま。中身は全レイヤの composition root
- `FrameControl::SetRenderSkipByGap()` と `SpeedFlags::TickBypass()` は未使用のまま。
  `FrameControl.hpp` の「2層構成」「RenderSkip + TickBypass」というコメントも実態と合っていない

### 新しく増えたもの

- **`SceneRunner::Step()` が 210行 → 731行**。経路が4本に増えたぶん、単一関数の中で分岐している。
  状態が `SceneRuntime` に集約されたので読めなくはないが、いちばん長い関数であることは変わらない
- `SharedSyncState` が mutex 5本 + atomic 20個規模に。
  用途ごとに区切られたので昨日より読みやすくなっているが、
  「どのフィールドをどのスレッドが書くか」は今もコメントでしか表現されていない
