# ゲージ満タンスパイクの原因 ── コードからの追加分析

`docs/design/2026-09-11_driver_lock_probe.md` と `..._gauge_spike.md` の観測を、
`src/` の実装と突き合わせて読み直した記録。実機試験は行っていない。
結論は**コードから読み取れる事実**と**そこからの推定**を分けて書く。

---

## 1. 結論

**D3DPOOL_MANAGED テクスチャの「VRAM への遅延昇格」が、満タン演出の初回描画フレームに集中している。**

既存の緩和（提示予算 1.2→5ms）は正しい判断だが、
失敗した対策群が失敗した理由は「効果がなかった」ではなく**軸が違った**からで、
唯一効いた PreLoad は**呼ぶ時刻だけが間違っていた**。

---

## 2. 根拠

### 2.1 ゲームは MANAGED プールでテクスチャを作っている（コードで確定）

`src/core_dll/mbaa_mem/StartupAssets.cpp:131`

```cpp
const bool eligible = device && data && size>=4 && size<=Limit && !width && !height &&
    levels==1 && !usage && format==D3DFMT_UNKNOWN &&
    pool==D3DPOOL_MANAGED &&                    // ← ここ
    filter==D3DX_FILTER_NONE && ...
```

DDS キャッシュのフックは、ゲームが `0x4BD3F2` から呼ぶ
`D3DXCreateTextureFromFileInMemoryEx` の実引数が `D3DPOOL_MANAGED` であることを
**前提として成立している**。つまりゲームのテクスチャは MANAGED。

D3DPOOL_MANAGED の定義上、`CreateTexture` の時点で作られるのは**システムメモリ側のコピーだけ**で、
ビデオメモリ側のサーフェスは**最初に実際に使われたとき**に D3D9 が作る。

### 2.2 観測された呼出し経路は、まさにその「遅延昇格」の経路

`gauge_spike.md` の EBP 連鎖（下位から）：

```
CMipMap::CMipMap+0x6D5
DdCreateSurfaceFilter+0x3F
DdCreateSurfaceLH+0x24
CreateSurfaceLH+0xC91
CBatchFilterI::LHBatchCreateResource2+0x13
CBatchFilterI::AcquireSynchronization+0x28   → EnterCriticalSection([ecx+0x20])
```

これは **d3d9.dll の内部がサーフェスを生成している**経路であり、
呼出し元はゲームコードでも D3DX でもない。
`driver_lock_probe.md` の表でも呼出し元は `D3D9 0x140935`（= AcquireSynchronization）と記録されている。

### 2.3 決め手 ── PreLoad が 2562.8µs を吸収した事実

`gauge_spike.md`：

> `..._transfer`：SetTexture前で明示PreLoadすると、約2562.8µsの待ちがPreLoad側へ移った。

`IDirect3DBaseTexture9::PreLoad()` は**既に生成済みの MANAGED リソースを VRAM へ昇格させる**
ためだけの API。まだ生成されていないテクスチャに対しては呼びようがない。

したがって：

- テクスチャ**オブジェクトは既に存在していた**（＝起動時かキャラロード時に作られている）
- 欠けていたのは**ビデオメモリ側のサーフェスだけ**
- 2677.5µs の描画待ちのうち 2562.8µs（96%）がその昇格コストだった

この1点で「満タンの瞬間にゲームがテクスチャを新規生成している」という読みは否定される。
生成は前、昇格が後。**時間差そのものが原因**。

### 2.4 待ち時間の中身

昇格の中で D3D9 はバッチ用 CS を取りに行き、そのロックを NVIDIA UMD（nvd3dum）の
ワーカースレッドが保持している。所有者側で採取された命令には
`NtGdiDdDDICreateAllocation+0xC` / `NtGdiDdDDILock2+0xC`（win32u）が含まれる
（`gauge_final_20260911/debug_owner`）。
つまり待ちの実体は**カーネル側のビデオメモリ割当**であって、
シェーダーコンパイルのようなドライバ固有処理とは限らない。

---

## 3. 失敗した対策が失敗した理由

| 試した対策 | なぜ効かなかったか |
|---|---|
| **事前のダミー描画**（FVF452・ブレンド・TSS・X8R8G8B8・D16 まで一致） | 昇格コストは**テクスチャオブジェクト単位**。描画ステートをいくら一致させても、別のテクスチャを描いたのでは対象のサーフェスは作られない。合わせる軸が「レンダーステート」ではなく「そのテクスチャそのもの」だった |
| **PreLoad** | 機構は正しい。ただし呼び出し位置が `RenderProbe.hpp:47-56` の **SetTexture フック内**、つまり**同じフレームのバインド直前**。コストがフレーム内で移動しただけで、フレーム外へは出ていない |
| **画像プール / UnlockRect 直後の転送 / 転送フック** | DYNAMIC・DEFAULT プールのアップロード経路の話。MANAGED の遅延昇格とは別の仕組み |
| **D3DCREATE_DISABLE_PSGP_THREADING** | PSGP = Processor Specific Graphics Pipeline（ソフトウェア頂点処理）のスレッド化フラグ。リソース割当とは無関係 |
| **DisableThreadedDDI の読取り差替え** | 仮に効いても「どのスレッドが割当をやるか」が変わるだけで、割当そのものは消えない |
| **PresentationInterval=IMMEDIATE** | 提示同期の話で、割当とは無関係 |
| **ハードウェア頂点処理 / 明示ピクセルシェーダー / 48枚転送**（driver_lock_probe.md の3比較） | いずれも描画方法の変更。初回昇格のコストは描画方法に依存しない |

**PreLoad だけが唯一「効いた」対策**であり、実測でその証拠が残っている。

---

## 4. まだ確かめていないこと（2つ）

### 4.1 起動時テクスチャ昇格の前倒し ── 未試行

`StartupAssets::Create` は**ゲームが作った `IDirect3DTexture9*` を全部握っている**
（`StartupAssets.cpp:153,157` の `*texture`）。
ここで生成されたテクスチャを記録しておき、**戦闘開始前に一括で `PreLoad()` する**経路は試されていない。

置き場所の候補は既にある。`SceneRunner.cpp:738` の世代境界：

```cpp
if (phase == GamePhase::InGame && !mem.PrepareBattleAudio()) {
    Fail(Error::SyncTimeout, "sound preparation restoration failed");
    return;
}
```

`SoundPrewarm` が「開始演出の合流前に停止済み SFX を無音で準備する」のと
**まったく同じ形**を、テクスチャに対してやることになる。
この位置はフレーム critical ではなく、30秒の待機予算の中にある。

**ただし1つ問題がある。** `startup_assets::Restore()` は
`SceneFastBoot::ProcessFrame()` の先頭（`SceneFastBoot.cpp:93-94`）で呼ばれ、
**タイトル画面の最初のフレームでフックが外れる**。
そのため StartupAssets が見られるのは起動時テクスチャだけで、
キャラロード時に作られる**キャラ固有の演出テクスチャは記録されない**。
満タン演出のテクスチャがキャラ固有なら、この経路では捕まらない。

→ 登録を取るなら D3D9 デバイス vtable の `CreateTexture`（index 20）側で行う必要がある。
RenderProbe は既に vtable を index 指定でフックする仕組みを持っている
（`RenderProbe.hpp:144` が index 65 = SetTexture）ので、追加は小さい。

### 4.2 2窓による割当競合 ── 分離されていない

すべてのゲージ試験が**同一PC2窓**で行われている。
`NtGdiDdDDICreateAllocation` は dxgkrnl のカーネル側で、
**プロセスをまたいで**ビデオメモリマネージャを取り合う。

実測もその形をしている：

- `gauge_final_20260911/baseline1200`：クライアント 3559.8µs に対し**ホストは 886.1µs でスパイク非再現**
- 文書自身が「双方で毎回出るとは主張しない」と書いている

片側だけ、毎回ではない、という出方は「その処理が本質的に重い」よりも
「2つのプロセスが同時に割当てて競合した」に整合する。

現在の緩和（提示予算 +3.8ms、**全ユーザーの表示遅延が固定で増える**）は
この2窓の数字から決められている。1プロセスでの対照を取っていないので、
実際の対戦（1台1プロセス）で本当に 5ms 必要かが確かめられていない。

---

## 5. 提案する確認手順（コストの低い順）

### 手順1 ── 既存ログだけで昇格説を確定させる（実機試験1回、追加実装なし）

`RenderProbe.hpp:53` の `[TextureTransfer]` ログは既に `texture=%p` を出している。
`StartupAssets::Create` が返した `*texture` のポインタも DebugLog に出すようにすれば、
131732F で 2.5ms 掛かったテクスチャが**起動時に生成済みだったか**が直接わかる。

- 一致する → 「生成は起動時、昇格が満タン時」で確定。4.1 の対策が有効
- 一致しない → キャラロード時生成。登録を CreateTexture 側へ移す必要がある（対策の形は同じ）

### 手順2 ── 2窓の寄与を分離する（実機試験1回、追加実装なし）

ゲージ試験を**1プロセスだけ**で回し、初回満タンの更新＋描画を比較する。

- 1窓で大幅に短い → 5ms 予算は2窓試験の数字に引きずられている。実対戦向けの予算を見直せる
- 1窓でも同じ → 割当コストそのもの。4.1 の前倒しが本命になる

### 手順3 ── 前倒し PreLoad の試作

`PrepareBattleAudio()` の隣に `PrepareBattleTextures()` を置き、
登録済みテクスチャへ `PreLoad()` を掛ける。
`analyze_gauge_stress.py --maximum-work-us 1200` が既にコスト判定として使えるので、
合否はそのまま自動判定できる。

---

## 6. 限界

- MANAGED の VRAM 常駐は**保証されない**。VRAM 逼迫時に D3D9 が追い出し、次の使用でまた昇格する。
  `PreLoad()` はヒントであり、`SetPriority()` で追い出されにくくはできるが確約ではない。
  2窓で VRAM を取り合う状況では再発しうる
- `GaugeStress` は 0→30000 を 60F ごとに**瞬間切替**する（`GaugeStress.hpp:33`）。
  実プレイではゲージは連続的に増えるので、100%/200%/300% の演出テクスチャは別々のタイミングで昇格する。
  試験の 3.5ms は複数枚の初回昇格が1フレームに束ねられた**上限値**の可能性がある。
  ユーザーが実際に見たスパイクの大きさとは別に扱う
- 昇格経路と待ち時間の内訳は、NVIDIA 非公開部分を含む。
  `NtGdiDdDDICreateAllocation` が含まれることは採取で確認済みだが、全時間の配分は未確定
- `0x4BD37F → 0x4BF060` にもう1つのテクスチャ生成経路がある（`StartupProfile.hpp:75`）。
  `gauge_spike.md` によればここが `0x4BF0CF` で `D3DXCreateTexture` を呼ぶ。
  この経路の pool 引数は未確認。MANAGED でなければ話が変わるので、
  `0x4BF0CF` の引数を読んで確認したほうがよい
