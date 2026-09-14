# 実機確認シート（2026-08-13）

Phase 1（フレーム空間の一本化）に入る前に、**実機でしか確認できないこと**をまとめて1回で取る。

分けた理由: Phase 1 は `netFrame` の採番をゲーム内時計に移す変更で、その設計は
「実機で WorldTimer がどう動くか」に依存する。ここを推測で決めると、作り直した後に
前提が違ったことが分かり、丸ごとやり直しになる。**設計を確定させる前に測る。**

所要は 30〜40 分程度。A が回帰確認、B が Phase 1 の設計判断、C が今後の作業用。
**時間が無い場合は A と B-1 だけでもよい**（B-1 が Phase 1 の設計を左右する唯一の項目）。

---

## 0. 準備

```powershell
$env:PATH = "C:\msys64\mingw32\bin;C:\msys64\usr\bin;" + $env:PATH
cd I:\work_space\CCCaster_v10
git log --oneline -4     # 76a794b / d1699d3 / 7951ee7 / d3aeb24 の4つが乗っていること
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j12
ctest --test-dir build --output-on-failure
```

> **`cmake -B build` を省略しないこと。** 今回ルートの CMakeLists を変更している
> （32bit ガード / `_WIN32_WINNT`）ため、再構成しないと反映されない。

### 期待する結果と、外れたときの意味

| 事象 | 意味 |
|---|---|
| configure が `32bit ツールチェインが必要です` で落ちる | **PATH が mingw64 を先に拾っている。** これが今まで無言で通っていた事故そのもの。PATH を直して再実行 |
| `inet_pton` 等でコンパイルエラー | `_WIN32_WINNT=0x0601` が MSYS2 の新しいヘッダと衝突している。**エラー全文を報告してほしい**（Linux 側クロスでは通っているので MSYS2 固有） |
| `ctest` が 5/5 PASS しない | 落ちたテスト名と出力を報告。Phase 0 は挙動を変えていないので、赤くなったら今回の変更が原因 |

### デプロイ（**ここを飛ばすと古い DLL を測ることになる**）

```powershell
.\build.bat
# または build\bin\libcccaster_hook.dll を _TEST_MBAACC\MBAACC_1 / _TEST_MBAACC\MBAACC_2 へ配置
Get-FileHash build\bin\libcccaster_hook.dll, `
             _TEST_MBAACC\MBAACC_1\cccaster\libcccaster_hook.dll, `
             _TEST_MBAACC\MBAACC_2\cccaster\libcccaster_hook.dll | Format-Table Hash, Path
```

3つのハッシュが一致していることを目視で確認する。2026-07-27 に「キャラセレでボタンが反応しない」
という報告が出た原因は、配備されていた DLL が古かったことだった。

---

## A. 回帰確認 — Phase 0 で壊していないか

Phase 0 は「挙動を変えない」変更のはずなので、**ここで何か変わっていたら想定外**。

```powershell
.\_TEST_MBAACC\dual_test.bat
```

| # | 見るもの | 期待 | 外れたら |
|---|---|---|---|
| A-1 | ゲームが起動し、キャラセレまで到達するか | 従来どおり | `Platform` 導入で時刻取得が変わった疑い。`RealMonotonicUs` を疑う |
| A-2 | `MBAACC_N\cccaster\cccaster_hook_log.txt` が生成され、**行が混線していないか** | 各行が完結している | LogSink のバッファ化が想定外。ログ末尾20行を報告 |
| A-3 | ログの**最終行**（正常終了・切断のどちらでも） | `Peer Disconnected` / `Aborted` が出る場合、その行がログに**残っている** | 同期フラッシュ経路が効いていない。`IsUrgent` の判定漏れ |
| A-4 | タスクマネージャの CPU 使用率 | 従来どおり（2コア高負荷のまま。**今回は直していない**） | 逆に下がっていたら `SleepMs`/`RealSleepMs` を取り違えている。**要報告** |
| A-5 | キャラセレでボタンが反応するか | 反応する | デプロイ漏れをまず疑う（ハッシュ再確認） |

> A-4 で CPU が下がっていたら、それは「良くなった」ではなく**意図しない挙動変更**。
> CPU を返す修正は Phase 2-1 で別途行う予定なので、ここで下がるのは取り違えを意味する。

---

## B. Phase 1 の設計判断に必要な測定

`CCCASTER_MEM_TRACE=1` を設定して両プロセスを起動する。毎フレーム `[MEM]` 行が出る。

```
[MEM] netFrame mode intro state WT RT roundTimer menuCtr rng0 rng1 p1seq p2seq p1hp p2hp roundCnt p1win p2win
```

### B-1. ロード中に WorldTimer が進むか（**最重要**）

Phase 1 は「`netFrame = WT - baseWT`」にする案なので、**ロード中に WT が止まるなら
採番も止まり、ロード時間差が自動的に吸収される**。進み続けるなら別の手当てが要る。
harness の `FakeGame` は「常時 ++」として模擬しているが、これは未検証の仮定。

確認方法: ログから `mode` が `8`(LOADING) の区間を抜き、その間の `WT` 列を見る。

```powershell
Select-String -Path .\_TEST_MBAACC\MBAACC_1\cccaster\cccaster_hook_log.txt -Pattern '^\[MEM\]' |
    ForEach-Object { $_.Line.Split(' ') } | Where-Object { $_[2] -eq '8' }
```

| 観測 | Phase 1 の設計 |
|---|---|
| ロード中 WT が**止まる** | WT 基準が素直に効く。案そのままで進む |
| ロード中も WT が**進む** | ロード区間を採番から除外する仕組みが追加で要る。`FakeGame` の校正も直す |

**報告してほしいもの**: LOADING 区間の最初と最後の `netFrame` と `WT`。

### B-2. Present とゲームロジック 1 tick が 1:1 か

`netFrame` は「Present 回数」でしかない。もし Present が 1 ゲームフレームに複数回、
あるいは逆なら、破綻は監査で書いたものよりさらに悪い。

確認方法: 対戦中（`intro=0`）の任意の連続100行で `Δnetframe / ΔWT` を出す。

**報告してほしいもの**: 対戦中の連続する `[MEM]` 行を10行そのまま貼る（両プロセス分）。

### B-3. フレーム原点のズレの実測値

```powershell
Select-String -Path .\_TEST_MBAACC\MBAACC_*\cccaster\cccaster_hook_log.txt `
              -Pattern 'Mode -> Counting|CharaSelect reached'
```

**報告してほしいもの**: 両プロセスの `startFrame=` の値と、その差。
これが「起動タイミングだけで生じている恒久的なズレ」の実測値になる。

### B-4. Metronome 起動前のフリーラン幅

`[FastBoot] ★ CharaSelect reached! (frame=N)` と `Mode -> Counting ... startFrame=M` の
時刻差、およびその間に `netFrame` がいくつ進んだか。監査 A-2 の実害の大きさが分かる。

---

## C. 逆アセンブルで確認したいもの（後回し可）

Phase 2 以降で触る箇所。今回は**情報を取るだけ**で、変更はしない。

### C-1. `MbaaPatcher` のパッチ 4-1 / 4-2 の意図

`MbaaPatcher.cpp:49-53` が `0x4A1D42` に `EB 0E`、`0x4A1D4A` に `EB` を書いている。
**両方ともコメントが一切ない。** しかも 0x4A1D4A は 4-1 のジャンプ範囲
`[0x4A1D44, 0x4A1D52)` の内側にあり、4-1 が有効なら 4-2 には制御が届かない。

デバッガか逆アセンブラで `MBAA.exe` の `0x4A1D30`〜`0x4A1D60` を**パッチ適用前**の状態で
ダンプして貼ってほしい。片方が不要と判明すれば消せる。

### C-2. `+0x1A..0x1B` が未使用パディングか

`RealGameMemory.cpp:66` は方向入力を `+0x18` に **4バイト**で書くが、
`GameInput::direction` は `uint16_t`。つまり `+0x1A..0x1B` に必ず `0x0000` が書かれている。
ここに別のフィールドがあると、毎フレーム静かに破壊していることになる。

確認方法: 対戦中に `*(char**)0x76E6AC` を辿り、`+0x10`〜`+0x40` を数フレーム分ダンプして、
`+0x1A..0x1B` と `+0x2E..0x2F` が常に 0 か、他の値を取ることがあるかを見る。

---

## 報告のしかた

A は「表のとおりだった / 違った箇所」だけで十分。
B は**ログの生データをそのまま**貼ってほしい（要約されると読み取れる情報が落ちる）。
`cccaster_hook_log.txt` が大きい場合は `[MEM]` 行だけ抜いたものでよい。
