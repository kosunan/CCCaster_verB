> **旧監査資料（2026-08-13）**。現行仕様は [CURRENT_STATE](../../CURRENT_STATE.md) を参照。本文の「現行」「未実装」は当時の状態。

# 04. フレーム空間と時間 — netFrame をゲーム進行に紐付ける

## 責務

この章の対象は「**あるフレーム番号が、両機で同じゲームの瞬間を指すこと**」を保証する仕組みである。

ロールバック netplay の全機能 — 入力の対応付け、予測とその検証、ステートの巻き戻し先、
デシンク検出 — はすべて「番号 → ゲームの瞬間」の写像が両機で一致していることを前提にする。
この前提が崩れると、下流はすべて正しく動きながら間違った答えを出す。現在の CCCaster_v10 は
まさにその状態にある。

具体的な担当範囲:

- ネットプレイフレーム番号（`netFrame` = `MatchInputBuffer::_writeHead`）の採番規則
- ゲーム内時計（WorldTimer）との対応付けと、その基準点（BaseWT）の確定
- フレームレート制御（Metronome）と背圧の相互作用
- 実時間クロック（WasapiClock）と TimeSync（θ / RTT / α）が何を補正するか

**担当しないもの**: 入力の中身（05章）、パケットの形式（06章）、フェーズ遷移の状態機械そのもの（07章）。

---

## 現状の構造

### 主要コンポーネント

| ファイル | 役割 | 実際に効いているか |
|---|---|---|
| `sync/MatchInputBuffer.hpp` | `_writeHead` を保持。フレーム番号の唯一の実体 | ✅ 効いている |
| `engine/SceneRunner.cpp` (E) `:164-212` | 背圧判定 → 通過したら `WriteLocal(head+1, ...)` | ✅ 唯一の採番地点 |
| `timing/Metronome.*` | 60fps 周期の精密待機。フレームレート制御はここだけ | ✅ ただし Counting 到達まで停止 |
| `timing/WasapiClock.*` | 実時間 [μs]。WASAPI IAudioClock、無ければ QPC | ✅ |
| `common/Platform.*` | OS 依存の時刻・待機の唯一の窓口。`SleepMs` / `RealSleepMs` を分離 | ✅ ただし呼び出し側は `SleepMs` のまま |
| `common/TimeScale.hpp` | `CCCASTER_TIME_SCALE` によるフレーム周期の圧縮（検証用） | ✅ |
| `sync/NetplayClock.*` | NTP T1-T4 による θ / RTT 推定、α 算出 | 計算は正しい。用途が薄い（後述） |
| `network/SyncCodec.cpp` | パケット組立/解析。`ConfirmRemote` の呼び出し元 | ✅ |
| `engine/MatchScene.cpp` `:100-105` | `phaseBaseWorldTimer`（＝ BaseWT）を採取 | ❌ 代入とログのみ。採番に未使用 |

### 時間の種類と所有者

| # | 時間 | 実体 | 進める者 | 読む者 | 停止条件 |
|---|---|---|---|---|---|
| 1 | 実時間 | `WasapiClock::GetTimeUs()` [μs] | OS / オーディオ DAC | Metronome, NetplayClock, `SleepUntil` | 止まらない |
| 2 | 実時間（フック前 QPC） | `platform::RealMonotonicUs()` | OS | WasapiClock のフォールバック, `GetCurrentTimeMs` | 止まらない |
| 3 | メトロノーム位相 | `Metronome::_nextTickUs` | Metronome 自身（`WaitForNextTick`） | ゲームスレッドのみ | **Counting 到達まで未起動** |
| 4 | **WorldTimer (WT)** | `*0x55D1D4` (uint32) | **MBAA 本体**。Present 1回につき +1 | `SceneRunner` の 60F 周期ログ / `MbaaMemTrace` / `MatchScene:100` | **止まらない（ロード中も +1／実測確定）** |
| 5 | RealTimer (RT) | `*0x562A40` | MBAA。ラウンド開始後に +1 | ログと `MbaaMemTrace` のみ | ラウンド外で停止 |
| 6 | RoundTimer | `*0x562A3C` | MBAA。4752 から減算 | `MbaaMemTrace` のみ（`IGameMemory` にすら露出していない） | 演出中は停止 |
| 7 | **netFrame** | `MatchInputBuffer::_writeHead` | `SceneRunner::Step()` (E) が背圧通過時のみ +1 | 背圧, `GetReadPos`, `SyncCodec`, `NetplaySession` の送信判定 | 背圧・FastBoot で停止 |
| 8 | framesInPhase | `MatchContext::framesInPhase` | `SceneRunner::Step()` が毎回 +1（FastBoot 中も） | 60F 周期ログのみ | フェーズ遷移で 0 に戻る |
| 9 | θ / RTT | `NetplayClock::_thetaUs` / `_bestRttUs` | 通信スレッド（受信ごと） | α1 / α2, `SetPeerStartTime` | — |

**重要な非対称**: 4 と 8 は Present に 1:1 で従うが、7 だけが従わない。
7 を進めるのは (E) の背圧判定であり、そこがゲームの進行とは無関係な条件で番号を止める。

### 処理の流れ

```
DxHook::Hooked_Present                   ← ゲームが毎フレーム呼ぶ（02章）
  └─ SceneRunner::Step()
       (A) PhaseMonitor::GetCurrentPhase()
       (B) フェーズ遷移 → framesInPhase = 0
       (C) FastBoot 中（phase < CharaSelect）→ framesInPhase++ して早期 return
           ★ (D) 以降に到達しない = メトロノーム待機なし = 無制限 FPS
       (D) Metronome.IsRunning() なら WaitForNextTick(false)
           ★ Counting 到達まで IsRunning() は false
       (E) lead = writeHead - confirmedRemote
           lead <= delay + maxRollback なら WriteLocal(writeHead + 1, 入力)
           そうでなければ ++starvedFrames          ★ 番号だけが止まる
       (F) MatchScene::On*()（IntroBarrier はここ。戻り値は捨てられる）
       (F2) readPos = writeHead - (delay + maxRollback) の入力をゲームメモリへ書込み
       (G) 60F ごとの状態ログ / 疎通チェック
```

通信スレッド（`NetplaySession::ThreadMain`）は独立に 16.6ms 周期で回り、
`_writeHead` の変化を監視して送信する。ゲームスレッドが止まっても回り続ける。

### 他領域との境界

| 相手 | 境界 |
|---|---|
| **01 プロセス** | `dllmain` の `SetTimeMultiplier(1000)` / `SetSleepBypass(true)` が恒久設定。**自 DLL の `Sleep(1)` も 0 化する**ため、この章の待機はすべてビジースピンになっている。`Platform::RealSleepMs()` が逃げ道 |
| **02 フック** | フレームの唯一の駆動源は `Hooked_Present`。`Step()` はここからしか呼ばれない。ここでブロックすると描画が止まる |
| **03 ゲームメモリ** | WT / RT / intro / gameMode の読み取りは `IGameMemory` 経由。`IsAvailable()` が偽の窓が存在しうる |
| **05 入力パイプライン** | 本章はフレーム番号だけを決める。番号に何を載せるか（フィルタ・予測・配信）は 05章 |
| **06 ネットワーク** | `SyncPayload.baseFrame` が本章の番号をそのまま運ぶ。冗長入力 `inputs[10]` は「番号が連続で欠番なし」を前提にしている |
| **07 セッション状態機械** | `SyncMode::Counting` 到達が Metronome の起動条件。IntroBarrier は 07章の機構だが、その副産物である BaseWT は本章が使う |
| **08 ロールバック** | 巻き戻し先フレームは本章の番号で指定される。番号がゲームの瞬間と対応していない限り、ロールバックは接続しても意味を持たない |
| **09 UI** | D / R の実行時変更が背圧と `readPos` の両方に影響する（現状は別変数を見ている） |
| **10 検証基盤** | harness の `FakeGame` は WT を「常時 ++」で模擬しており、実機と一致することが実測で確認済み |

---

## 現状の問題

### P-1. netFrame は「背圧を通過した Present の回数 + 200」でしかない

`NetplaySession.cpp:57` が `Initialize(200, ...)` で原点を 200 に固定し、
`SceneRunner.cpp:207` が `WriteLocal(head + 1, ...)` で 1 ずつ進める。
**ゲーム内時計と一度も突き合わされていない。**

AUDIT §1 の因果連鎖を要約すると:

```
FastBoot 中は (D)(E) に到達しない
  → Metronome も未起動（Counting 到達まで）＋ TimeHooks がリミッタを殺している
  → 無制限 FPS でフリーラン（AUDIT の想定した破綻経路）
Counting 突入時（NetplaySession.cpp:227）に GetWriteHead() を読むが、ログに渡すだけ
  → 唯一の合流点で再基準化していない
背圧が両者の「番号」だけを D+R 以内に強制的に揃える
  → ゲーム状態の食い違いが「番号は合っている」という形で隠蔽され、恒久化する
```

ただし **実測（REALHW B-3）では Counting 突入時に両機とも `startFrame=200 WT=0` で揃っていた**。
FastBoot の早期 return が CharaSelect 到達まで (E) を止めており、Counting はそれより前に成立するため。
→ **「Counting 時の原点合意」は不要**。破綻は原点ではなく、その後の採番規則にある。

### P-2. starve がそのまま「番号とゲームの対応ずれ量」になる（実測で確定）

背圧が発動した Present では、ゲームは 1 フレーム進む（WT +1）が netFrame は進まない。
この差は二度と埋まらず、累積する。

実測（REALHW C-1）:

| 機体 | 時点 | WT | writeHead | WT − wh | starve |
|---|---|---|---|---|---|
| host | phase=2 fip=0 | 45 | 201 | −156 | 0 |
| host | phase=2 fip=1740 | 1785 | 1630 | 155 | 311 |
| client | phase=2 fip=1740 | 1785 | 1634 | 151 | 307 |

`(WT − wh) − (−156) == starve` が**1フレームの誤差もなく一致**する。
つまり `WT − netFrame = 定数 + starve` が恒等式として成立している。

**背圧は同期機構ではなく、対応ずれの生成器である。** 番号を揃えるために、
番号とゲームの対応関係を犠牲にしている。

### P-3. 本当に大きいずれはメニュー滞在時間の差から来る（実測・新発見）

`[IntroBarrier] Both peers reached intro=2!` 時点の BaseWT:

| | ラウンド1 | ラウンド2（リマッチ後） |
|---|---|---|
| host BaseWT | 1869 | 5412 |
| client BaseWT | 1870 | 5560 |
| 差 | 1 | **148** |

starve の差（4〜5）では説明できない。Rematch フェーズの滞在時間が host 479 行 /
client 1056 行と倍以上違うことが原因。**セッション通しの単一フレーム空間である限り、
メニューでの滞在差がそのまま対戦に持ち込まれる。**

### P-4. Metronome の穴

- **Counting 到達まで起動しない**（`NetplaySession.cpp:229`）。その間 TimeHooks がゲーム内蔵
  リミッタを殺しているため、フレームレート制御が誰もいない窓が最低 0.5 秒存在する。
- **catch-up 上限が無い**。`WaitForNextTick` は `_nextTickUs += interval` してから待つだけで、
  `_nextTickUs` が現在時刻より大きく過去に落ちた場合、その債務ぶんは**無待機で連続実行**される。
  1 秒遅れれば 60 フレームがフリーランする。
- `SleepUntil` の `SleepMs(1)` は実機では 0ms 化される（01章の `SetSleepBypass`）。
  結果、待機は全部ビジースピンで、CPU が 1 コア張り付く。

### P-5. TimeSync が補正しているもの / していないもの

| 対象 | 誰が補正するか | 実効 |
|---|---|---|
| 両機の**絶対時刻**の差 | θ（NTP T1-T4、最小 RTT フィルタ） | ✅ 機能している。`SetPeerStartTime` で開始時刻合意に使われる |
| 合意後の**クロックドリフト** | α2 = `GetTickUs() - BASE_TICK_US`（Δθ の二乗カーブ、±2666μs 飽和） | ✅ 計算は妥当。ただし補正対象は自分のティック周期のみ |
| **パケット遅延が D+R で吸収しきれない分** | α1 = `max(0, RTT/2 − (D+R)×16666)` | ❌ D=2,R=4 なら RTT 200ms 未満で常に 0。かつ**両者が同じ値を出すので相対補正にならない** |
| **相手のフレーム進行そのもの** | — | ❌ **補正ループに入っていない**。`_latestPeerFrame` は `SyncCodec.cpp:167` で更新され、`NetplaySession.cpp:279` のログに出るだけ |

**θ が揃えているのは「時計」であって「ゲームの進み具合」ではない。**
WT の進み方は Present の頻度で決まり、Present の頻度は Metronome とゲーム側の処理負荷で決まる。
θ はそのどちらにも触れていない。相手が 148 フレーム先に進んでいても、θ は 0 のままでありうる。

### P-6. 相手のフレーム停止が切断として検出されない（実測・最も実害が大きい）

実機ログ末尾: ホストの `Present` が来なくなり `writeHead` が 5452 で凍結。
クライアントは `lead = 5459 − 5452 = 7 > maxLead 6` で恒久的に背圧停止。
**両者が無言で固まり、`Peer Disconnected` はどちらにも出ない。**

原因は、キープアライブを送るのが**ゲームスレッドから独立した通信スレッド**だから。

> キープアライブは「相手のプロセスが生きている」ことしか示さず、
> 「相手のゲームが進んでいる」ことを一切保証していない。

---

## 目指す設計

### 設計原則

1. **フレーム番号はゲームが決める。ネットワークは決めない。**
   採番の権威を「背圧を通過した Present の回数」から「WorldTimer」へ移す。
   WT は MBAA 本体が進めるので、通信の都合で止まることがない。

2. **基準点は交換しない。各自が同じ論理的瞬間で自分の WT を採ればよい。**
   BaseWT が両機で違う値（5412 と 5560）であることこそが正しい。
   各自の絶対 WT の差を吸収するのが基準点の役目だからである。
   → **原点合意プロトコルは不要**（REALHW D）。

3. **背圧は番号を止めてはならない。ゲームを止める。**
   番号を止めると P-2 の対応ずれが生まれる。止めるべきは Present の間隔であり、
   そのレバーは Metronome しかない。

4. **同値・欠番は原理的に起きないはずのものとして扱い、起きたら異常として計測する。**
   `Present : WT = 1:1` は実測で確定している（98サンプル×2機、例外なし）。
   これを「暗黙の前提」ではなく「検証される不変条件」にする。

5. **ラウンドごとに基準を取り直す。** P-3 のメニュー滞在差を対戦に持ち込まない。

### 新しいフレーム番号の定義

```
relFrame  = WT − BaseWT                 ← ラウンド内相対フレーム（採番の権威）
netFrame  = epochId × EPOCH_STRIDE + relFrame   ← セッション通し番号（既存 API の値）
```

| 記号 | 定義 | 決める者 | 更新契機 |
|---|---|---|---|
| `WT` | `GameMem().WorldTimer()` | MBAA | 毎 Present |
| `BaseWT` | **このラウンドで初めて `IntroState()==2` を観測したフレームの WT** | ゲームスレッド | ラウンドごとに1回 |
| `epochId` | ラウンド通番。IntroBarrier を通過した回数 | ゲームスレッド | ラウンドごとに +1 |
| `EPOCH_STRIDE` | 定数。1 ラウンドが絶対に超えない長さ（例 `1 << 16` = 65536 ≒ 18分） | コンパイル時定数 | — |

#### なぜ BaseWT を「バリア解放時」ではなく「intro=2 ラッチ時」に採るか

現在のコード（`MatchScene.cpp:100`）は**バリア解放時**の WT を採っている。
バリア解放は「自分が intro=2 に到達し、かつ相手の到達通知が届いた」瞬間なので、
**片道遅延ぶんの誤差が原点に混入する**。

- host が t=0、client が t=10 に到達し、片道遅延 owd の場合
- host の解放は t = 10 + owd、client の解放は t = max(10, owd)
- → 原点が owd ぶんずれる（RTT 50ms なら約 1.5 フレーム）

一方 `IntroState()` が 2 になる瞬間は**ゲーム状態で定義された事象**であり、
ネットワークに依存しない。`Present : WT = 1:1` なので DLL はすべての WT を観測でき、
取りこぼしはない。よってラッチ時に採るほうが厳密に良い。

**バリアは「待つ」ためだけに残す。原点を「決める」役目からは外す。**

> 推測: ラウンド1 の BaseWT 差が 1 フレームだったのは、この片道遅延誤差である可能性が高い。
> ラッチ時に採れば 0 になるはず。ただしラッチ時の WT は現行ログに記録されていないので未検証。

#### 同値・欠番のポリシー

`Present : WT = 1:1` が成立する限り `ΔWT == 1` だが、成立しない場合を規定しておく。
毎フレーム `prevWT` と比較し、以下に分類する。

| 観測 | 意味 | 規定する扱い | 計測 |
|---|---|---|---|
| `ΔWT == 1` | 正常 | relFrame を +1。通常処理 | — |
| `IsAvailable() == false` | ゲームメモリが読めない | **relFrame を進めない。(E)(F2) を丸ごとスキップ**。keepalive のみ維持。`++wtUnavailable`。60F 続いたら `SessionErrorType` を立てる | `wtUnavailable` |
| `ΔWT == 0` | 前提違反（同値） | **relFrame を進めない。(E)(F2) をスキップ**。`++wtStall`。0 でない時点で異常なのでログ | `wtStall` |
| `ΔWT > 1` | 前提違反（欠番） | **欠番を作らない。飛ばされた relFrame に「直前のローカル入力を保持」で埋める**。`wtGap += ΔWT-1` | `wtGap` |
| `ΔWT < 0` | WT の巻き戻し | エポック境界以外では致命。ログして `wtRewind`、セッション継続は不可 | `wtRewind` |
| `gameMode` 遷移 | Loading↔InGame↔Rematch | **何もしない。WT は連続なので relFrame も連続**（実測 B-1） | — |

**欠番を許さない理由**: `MatchInputBuffer` のリングは `frame % 600` でスロットを決め、
冗長入力は `baseFrame - i (i=0..9)` の連番を送る。`readPos` も `writeHead - (D+R)` の
単純減算である。番号が疎になると、これらすべてが「存在しないフレーム」を参照する。
**密であること（consecutive）は下流全体の前提**なので、上流のここで保証する。

**同値を許さない理由**: 同じ番号に 2 回入力を書くと、`ConfirmRemote` の先勝ちルール
（`MatchInputBuffer.hpp:76-83`）により、後から来た本物の入力が永久に捨てられる。

### 変更点

#### C-1. 採番地点の置き換え（`SceneRunner.cpp` (E)）

```
現行:  lead <= maxLead なら WriteLocal(writeHead + 1, ...)
       そうでなければ ++starvedFrames          ← 番号が止まる

新規:  relFrame = WT - BaseWT
       netFrame = epochId * EPOCH_STRIDE + relFrame
       ΔWT 分類（上表）で異常を処理
       lead > maxLead なら (D) のメトロノーム待機を延長して Present を遅らせる
       → 番号は必ず WT に追従する。starve という概念自体を廃止する
```

#### C-2. 背圧の作用点を「番号」から「時間」へ移す

これが最も影響の大きい変更である。

```
(D) メトロノーム待機
    lead = netFrame - confirmedRemote
    while (lead > maxLead && 経過 < BACKPRESSURE_MAX_MS) {
        Metronome::WaitForNextTick(false);   // 1F ぶん余分に待つ
        lead を再評価
    }
    if (経過 >= BACKPRESSURE_MAX_MS) → 相手停止として扱う（C-5）
```

| 論点 | 判断 |
|---|---|
| ゲームスレッドをブロックしてよいのか | **条件付きで可**。AGENTS の禁止事項は「相手入力を待って無限ループする」こと。ここは上限付きで、かつ keepalive は通信スレッドが独立に送るので途絶えない（`needKeepalive` はレベルフラグで、`Step()` が止まっても true のまま） |
| 描画が止まるのでは | 止まる。ただし数フレームぶんであり、ロールバック netcode が普遍的に行う「タイミングストール」そのもの |
| 上限に達したら | 相手のゲームが進んでいない。C-5 の停止検出に合流させる |

**なぜこれが必要か**: 番号が WT に固定された以上、番号を止める手段がもう無い。
lead を抑える唯一の方法は WT の進行、すなわち Present の間隔を伸ばすことである。

#### C-3. `_writeHead` の単調性 — 二層構造を選ぶ

`_writeHead` は単調増加を前提にした箇所が複数ある
（`NetplaySession.cpp:254` の `newHead > _lastSentFrame`、`SyncCodec.cpp:167` の
`gtp.baseFrame > _latestPeerFrame`、`MatchInputBuffer.hpp:95` の `frame > prev`）。
ラウンドごとに `relFrame` を 0 に戻すと、これらすべてが壊れる。

| 案 | 内容 | 利点 | 欠点 | 採否 |
|---|---|---|---|---|
| **A. 二層（採用）** | `netFrame = epochId × EPOCH_STRIDE + relFrame` | 単調性が保たれ、上記3箇所を触らない。`epochId` は「バリア通過回数」なので両機で自明に一致し、交換不要。番号から epoch と rel の両方が復元できるのでパケットにフィールドを増やさなくてよい | 番号が疎（ラウンド間に大きな空隙）。`uint32_t` の枯渇は `65536 × 65536` なので実用上問題なし | ✅ |
| B. `Reset()` を挟む | ラウンド境界で `MatchInputBuffer::Reset()` して 0 から | 番号が小さく読みやすい | ①両機の Reset 時刻がずれ、その間に届く前ラウンドの高い番号のパケットが新しい空間を汚染する ②`_lastSentFrame` / `_latestPeerFrame` の単調比較が壊れ、リセット後しばらく送信が止まる ③リング全消去のコスト | ❌ |
| C. 相対番号をそのまま送る | パケットに `epochId` と `relFrame` を別フィールドで | 意味が明快 | パケット拡張が必要。単調比較は結局 (epoch, rel) の辞書式に書き換えることになり、B と同じ範囲を触る | ❌ |

**A を選ぶ理由**: 触る箇所が最小で、かつ「エポックをまたいだ古いパケット」が
自動的に無害になる。前ラウンドの番号は現ラウンドより必ず小さいので、
`ConfirmRemote` が書き込むスロットは `frame % 600` で衝突しても
**フレーム番号の完全一致検証で弾かれる**（`MatchInputBuffer.hpp:120,127`）。

> 注意: `EPOCH_STRIDE` を `RING_SIZE (600)` の倍数にしても安全性は変わらない
> （スロット検証は完全一致なので）。ただしデバッグ時に別ラウンドの同一スロットが
> 紛らわしいので、**600 と互いに素でない値を避ける**ことを推奨する。65536 は 600 の倍数でない。

#### C-4. `readPos` / 冗長入力 / D・R の整理

| 項目 | 現状 | 変更後 |
|---|---|---|
| `readPos` | `_writeHead - (_delay + _maxRollback)` | 定義は同じ。ただし `_writeHead` が WT 由来になるので、**readPos が指す瞬間も両機で一致する**（現状は starve ぶんずれる） |
| D/R の所有 | 背圧は `ctx.delay`、readPos は `_delay` を見ており乖離する（AUDIT B-3） | `MatchInputBuffer` の atomic を単一の所有者にし、背圧もそこを読む |
| 冗長入力 | `baseFrame - i` の連番。未書込みフレームが入力 0 として確定される（AUDIT B-5） | 番号が密であることは C-1 で保証。加えて**有効ビットマスク**を付け、「書いていない」と「0 を書いた」を区別する（詳細は 06章） |
| エポック境界 | — | `relFrame < 0`（BaseWT 確定前）のフレームは送らない。`baseFrame == 0` を「未開始」として既存のガードがそのまま使える |

#### C-5. 相手フレーム停止の検出

`_latestPeerFrame` は既にパケットから取れている（`SyncCodec.cpp:167`）。判定を足すだけでよい。

```
通信スレッドの Counting で毎ティック:
  if (_latestPeerFrame == _lastObservedPeerFrame) ++peerStallTicks;
  else { _lastObservedPeerFrame = _latestPeerFrame; peerStallTicks = 0; }
  if (peerStallTicks > 180)   // 3秒相当
      → isPeerAlive = false → SceneRunner (G) の既存経路で PeerDisconnected
```

**新しい番号体系で初めて意味を持つ**: 番号がゲーム進行に紐付いているので、
「相手の番号が止まった」＝「相手のゲームが止まった」と言い切れる。
現行の番号では背圧で止まっているだけの可能性があり、区別できなかった。

#### C-6. Metronome の是正

| 問題 | 対処 |
|---|---|
| Counting 到達まで未起動 | `SceneRunner::Init` の直後に `Start()` する。同期前でも 60fps に律速されているほうが安全（無制限 FPS の窓を消す） |
| catch-up 上限なし | `WaitForNextTick` で `_nextTickUs` が `now - CATCHUP_LIMIT_US`（例 3F ぶん）より古ければ `_nextTickUs = now` に引き直す。溜まった債務は切り捨てる |
| FastBoot 中に (D) に来ない | FastBoot の早期 return を (D) の**後ろ**へ移す |
| `SleepMs` が 0ms 化 | `platform::RealSleepMs()` へ差し替え（01章と共同。CPU 2 コアが返る） |

#### C-7. フレーム空間の相互検証（これまで存在しなかった）

パケットに 1 バイトの状態タグを載せる: `stateTag = (gameMode << 4) | introState`。
受信側は `baseFrame` に対応する自分の `stateTag` と比較し、食い違えばデシンクとして記録する。

**なぜ必要か**: AGENTS が明記するとおり「同じ `netFrame` なら同じ WorldTimer / mode / intro」を
検証するテストが1つも無い。デシンクがテストをすり抜けるのはこのためである。
1 バイトで、実行中に常時この不変条件を監視できる。

---

### 移行手順

各段階は単独でビルド・検証でき、途中で止めても回帰しないように区切る。

| 段階 | 内容 | harness で検証できること |
|---|---|---|
| **S0 観測** | `relFrame` を「影の番号」として毎フレーム計算・記録するだけ。採番は `_writeHead` のまま。ΔWT 分類のカウンタ（`wtStall` / `wtGap` / `wtUnavailable`）を追加 | `ΔWT == 1` が本当に例外なく成立するか（60F 粒度では見えなかった同値・欠番の有無）。`WT − wh == 定数 + starve` の恒等式が harness でも再現するか |
| **S1 基準点の配線** | BaseWT を intro=2 ラッチ時に採取。`epochId` を導入。死んでいる `phaseBaseFrame` を `epochId` に転用。パケットに `stateTag` を追加。**採番はまだ変えない** | 「同じ影の relFrame なら同じ (mode, intro)」が成立するか。ロード時間差を `--host-loading 60 --client-loading 180` で作っても成立するか。**これが本設計の可否を分ける判定** |
| **S2 採番の切替** | `netFrame = epochId × STRIDE + relFrame` を採用。背圧を「番号を止める」から「メトロノームを延ばす」へ。`starvedFrames` を廃止し `backpressureFrames`（時間で計測）に置換 | `run_pair.sh` の状態突合せで `WT` 列が全共通フレームで一致すること。`starve` 起因のずれが原理的に消えたこと。リマッチ 2 ラウンドで epoch がまたいでも入力列が一致すること |
| **S3 周辺の追随** | 相手フレーム停止検出、冗長入力の有効マスク、D/R の単一所有者化、Metronome の catch-up 上限と早期 Start | 片側を `kill -STOP` して 3 秒以内に切断が報告されること。D/R を実行中に変更しても恒久 stall に落ちないこと |
| **S4 実機ゲート** | `CCCASTER_MEM_TRACE=1` を有効にして等倍で 2 窓テスト。ログ間引き（REALHW F-2）を先に済ませること | 実機での `ΔWT == 1` の完全性、リマッチ後の BaseWT 差が消えること、`stateTag` 不一致が 0 件であること |
| **S5 ロールバック接続** | 08章へ引き渡す。本章の番号が安定していることが前提条件 | — |

**S1 で「成立しない」と出た場合の分岐**: `stateTag` が同じ relFrame で食い違うなら、
intro=2 の観測タイミングが両機で同じ論理的瞬間になっていない。その場合は原点を
`IntroState()` ではなく `RealTimer() == 0` の初回観測（ラウンド開始）に移す案が次点。
RealTimer はラウンド開始でリセットされるため、より明確なゲーム事象である。

---

## 未確定事項

1. **intro=2 ラッチ時の BaseWT が本当に同一論理瞬間か。**
   現行ログはバリア解放時の値しか記録していない。S0 でラッチ時の WT を記録して比較する必要がある。

2. **実機で `ΔWT != 1` が発生するか。** 実測は 60 フレーム粒度の定期ログからの推定であり、
   「60 フレームで 60 進んだ」ことしか言えない。途中で +2 と 0 が打ち消し合っていた可能性は
   排除できていない。S0 の毎フレーム記録で確定させる。

3. **`IsAvailable()` が偽になる窓が実際に存在するか。** `RealGameMemory::IsAvailable()` は
   `IsBadReadPtr(CC_GAME_MODE_ADDR)` だけを見ており、WT のアドレスは検査していない。
   プロセス終了間際やモード遷移中の挙動は未観測。

4. **CharaSelect / Loading / Rematch の採番方針。** 本設計は InGame エポックの厳密性のみを
   保証する。メニュー中は BaseWT が無いため、対応関係が緩い。
   キャラセレ同期が未実装（AUDIT: `CC_P1/P2_CHARA_SELECTOR_ADDR` は全て未使用）である
   現状では実害が無いが、実装時にはメニュー用のエポックが要る。

5. **`BACKPRESSURE_MAX_MS` と `CATCHUP_LIMIT_US` の値。** 短すぎると正常な回線でも
   切断扱いになり、長すぎると P-6 の無言フリーズが残る。実測が要る。

6. **背圧のブロッキングがゲーム側の内部タイマーに与える影響。** Present の間隔を伸ばした際、
   MBAA が自前で経過時間を見てフレームスキップしないという保証は無い。
   `SetTimeMultiplier(1000)` が既に入っている状態での挙動は未検証。

7. **ホストのゲームスレッドが停止した原因（REALHW E）。** `[FATAL]` も `Aborted` も残っておらず、
   利用者がウィンドウを閉じた可能性も否定できない。再現条件が要る。

8. **α1 の扱い。** 現状は実質無効かつ相対補正になっていない。C-2 で背圧が時間側に効くように
   なると α1 の役割はさらに薄くなる。**削除するか、相手の relFrame との差を入力とする
   本物の相対補正に作り直すか**を決める必要がある。
   後者なら「相手のフレーム進行を補正ループに入れる」ことになり、本章の未解決点が閉じる。
