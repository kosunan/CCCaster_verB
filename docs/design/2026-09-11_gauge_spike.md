# ゲージ満タン化スパイクの再現・逆アセンブルと緩和

2026-09-11、MBAACC Ver.1.07 Rev.1.4.0、32bit、同一PC2窓。ユーザーの「満タンになる瞬間に見えたスパイク」を調べた。デバッガの有無を分け、通常更新・提示要求・描画API内経過を区別する。ゲーム画面の目視再確認は行っていない。

## 実験条件

`CCCASTER_GAUGE_STRESS=1` と自動入力の両方を明示した試験だけで有効。ゲームスレッド上で双方のゲージを0／30000（0%／300%）へ60通常F単位で交互設定し、初回満タンは世代内660F。固定0%の対照はモード2。入力は既存GameMem経路でニュートラルにし、体力・赤体力11400、ラウンドタイマー4752を更新前・提示前に設定する。WT／RT・入力時計・RNG・派生入力を直接変更しない。再計算でも同じフレーム番号から同じ値を決める。

内部更新がタイマーを1だけ減らす場合、採取値は4751となるが提示前に4752へ戻す。満タン直後はゲームがゲージを0へ消費し、HEAT825へ遷移するため、要求30000と採取ゲージ0を区別する。HEAT・演出状態を強制解除して負荷を消してはいない。

## 検出した遅れ

安定した0%区間の後、131732Fの初回満タンで再現した。

- `build_logs/gauge_alternate_20260911_probe`：ホストの更新＋描画3596.2µs、提示要求間隔19351µs。DrawIndexedPrimitiveUPの単一呼出し2677.5µs、音更新26.2µs。更新開始の間隔は約16669µsであり、同じ値を提示間隔とは扱わない。
- `..._transfer`：SetTexture前で明示PreLoadすると、約2562.8µsの待ちがPreLoad側へ移った。API名だけからGPUコピー時間と断定せず、次のデバッガ採取へ進んだ。
- `..._debug`：実採取間隔中央値564.3／564.5µs（設定200µsではない）。131732Fの2702.4／2690.0µsのPreLoad区間内で、それぞれ4件のNtWaitForAlertByThreadId命令を採取。

診断のGetRenderState・ログ・スレッド停止は処理時間を増やす。性能の採否はデバッガ／RenderProbe／明示PreLoadを外した別試験で判断する。

## 逆アセンブルによる帰属

`build_logs/gauge_alternate_20260911_debugstack/spike_debug_analysis/` に対象Fだけの実メモリ命令とJSONがある。32bit ntdllの実アドレス0x771EB9DCはRVA0x7B9DC、NtWaitForAlertByThreadId+0xC（システム呼出し復帰点）。EBP連鎖を採取したスタック範囲で辿り、逆アセンブルしたCALLと照合した。

下位からのD3D9呼出経路：

| D3D9 RVA | Microsoft公開PDBでの名前 |
|---|---|
| 0x4ABB5 | CMipMap::CMipMap+0x6D5 |
| 0x65E4F | DdCreateSurfaceFilter+0x3F |
| 0x3E864 | DdCreateSurfaceLH+0x24 |
| 0x3F541 | CreateSurfaceLH+0xC91 |
| 0x141933 | CBatchFilterI::LHBatchCreateResource2+0x13 |
| 0x140935 | CBatchFilterI::AcquireSynchronization+0x28 |

AcquireSynchronizationは `[ecx+0x20]` をEnterCriticalSectionへ渡す。LHBatchCreateResource2はその取得後にドライバの資源生成関数を呼ぶ。D3D9のバッチ用ロックが取れるまで、ゲームスレッドが止まっている。

使用したd3d9.pdbはDLLのCodeView識別子 `41BB33BF99B64091517B7E74C1B2ADC5`、age1と一致するMicrosoft公開シンボル。保存先 `build_logs/gauge_symbols/`。DLL timestamp1987618885、ImageBase/RVAでASLRを区別した。ゲーム側の0x4BF060の資源生成関数も逆アセンブルし、D3DXCreateTexture呼出し0x4BF0CF／戻り0x4BF0D4を確認。資料は `build_logs/gauge_texture_create.asm`。

`..._debugowner45/gauge_owner.json` では、同じロックのOwningThreadを読み、同じゲームPIDに属する所有者を停止・採取・再開した。ホストTID31136／クライアントTID35892から、nvd3dum.dll内部の複数の命令を採取。RVA0x109488Dの間接CALL、0x18F7B6のメモリ参照、0xB2FCEAのMOVUPS等で、単に所有者がOS待機APIにいたという結果ではない。全区間をCPU処理と証明したわけではなく、NVIDIA内部の処理名（シェーダーコンパイル等）は未確定。所有権を採取後にも再確認する診断を追加し、最終採取の結果は下段へ追記する。

## 試して採用しなかった変更

事前のダミー描画（実測FVF452、ブレンド、TSS、X8R8G8B8描画先、D16深度まで一致）、PreLoad、画像プール、UnlockRect直後の転送では遅れを解消できなかった。Scene対統合の無効化でも残存。公開フラグD3DCREATE_DISABLE_PSGP_THREADING、プロセス内だけのDisableThreadedDDI読取り差替えでも改善しなかった。ゲームは既にPresentationInterval=IMMEDIATEで、これも対策にはならない。

失敗した画像プール・レジストリ読取り差替え・転送フックは削除し、通常動作へ入れていない。Windowsレジストリ・ドライバ設定は変更していない。公開フラグの意味は[Microsoft D3DCREATE](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dcreate)を参照。

## 採用する緩和と限界

提示予算を1200→5000µsへ拡張する。入力・通常更新の絶対締切と60Hzを維持し、提示位相を固定して、更新＋描画の約4.7msを提示締切内で吸収する。処理後に16.667msを追加待機する方式ではない。

これは表示までの固定遅延を3.8ms増やす交換条件であり、NVIDIA内部の処理や資源生成コストを消す根治ではない。試験用 `CCCASTER_PRESENT_BUDGET_US=1200` で従来条件へ戻せる。許容値は0〜8000µs、無効値は既定へ戻る。別GPU・全キャラ全演出・長時間・実モニター上の表示完了時刻・全フレーム一定は未確認。

## 自動検出

`run_regression.py --profile real --case gauge --case gauge_control` で、32bitビルド・通常の高速試験・45秒ずつの交互切替／固定0%・同期比較・INI保全・WASAPI確認を一括実行する。GaugeStress解析は要求値と実ゲージ、体力・タイマー、通常／再計算、切替F前後4Fの提示間隔を保存する。通常600標本未満、上下各3回未満、提示標本不足、固定失敗、切替窓の提示周期誤差1ms超を不合格とする。

デバッグ調査は `--debug-spikes` と `CCCASTER_DEBUG_INTERVAL_US=200`、`CCCASTER_RENDER_PROBE=1`、`CCCASTER_TEXTURE_TRANSFER_PROBE=1` を明示。解析は `analyze_spike_debug.py <pair> --frame 131732 --kind textureTransfer --minimum-us 1000`。通常性能の試験と同じ計測負荷にはしない。

## 最終検証

一括入口の最終成功: `build_logs/regression_20260911_181030_136653/summary.json`（118.8秒、ゲージと対照だけを選択）。32bitビルド・22 CTest・49 Pythonテスト・4倍速同期harnessも同時に合格した。

| 条件 | 通常採取F（両側） | 確定状態一致F | 結果 |
|---|---:|---:|---|
| 5ms・0↔300% | 1339 | 1641 | 交互切替・固定・提示誤差ゲート合格 |
| 5ms・固定0% | 1302 | 1604 | 固定・同期合格。更新＋描画中央値209.4／206.2µs |
| 5ms・通常戦闘、15〜25ms／損失5% | ゲージ採取対象外 | 1604 | REC/FRAME/STATE/MEM差分・欠落0、巻戻し5／7回 |

交互切替の初回満タンの更新＋描画は3577.9／3576.2µs、提示要求間隔は両側16668µs。全切替窓の最大提示周期誤差は両側2.333µs。体力11400、タイマー4751〜4752の更新中採取を確認。全設定ファイル・WASAPI使用を一括入口で確認しており、実モニターのscanout時刻ではない。

先行5ms試験（`regression_20260911_180706_842474/gauge_0`）でも上下17切替ずつを採取し合格。ただし初回の処理は4972.7／4982.5µs、提示周期誤差292.3／186.3µsで、全試行が2.333µs以内だったとは扱わない。

先行一括入口は解析CLIのヘルプ文字列にある未エスケープの%により停止した（ゲームや同期の失敗ではない）。文字列を修正し、CLI起動テストを追加。既存のゲームログも再解析して合格したが、その最初のsummaryの失敗記録は書き換えていない。通常戦闘の根拠は同じ先行一括の`combat_0`。最終一括はゲージ・対照を選んだ新規実行で、両ケースまで完走した。

### 同じ最終バイナリでの対照

`build_logs/gauge_final_20260911/baseline1200` は提示予算だけ1200µsへ戻した実行。クライアント初回満タンの更新＋描画3559.8µs、提示要求間隔19220µs、周期誤差2553.3µsで自動ゲートが期待どおり失敗した。ホストはこの試行では886.1µsでスパイク非再現。双方で毎回出るとは主張しない。1627確定Fの状態は一致し、タイマー・体力固定も成立。

同じ最終バイナリの5ms試験ではクライアント更新＋描画3576.2µs、提示要求16668µs。処理時間の削減ではなく、提示前の待機余裕で吸収したことを確認した。先行試行も含め約0.3msまでの提示誤差は残っており、全提示が完全等間隔とはしていない。

### 所有者の再確認と実命令

最終証拠: `build_logs/gauge_final_20260911/debug_owner/spike_debug_analysis/README.md` と `report.json`。131732FのPreLoad区間内のゲームスレッド標本は5／3件。同じロックのOwningThreadを再読して、所有者を停止した後も所有権が維持されている5／3件を確認。PID28388→所有者TID36820、PID27836→所有者TID35316。

- NVIDIA nvd3dum.dll内の命令・EBP連鎖を双方で確認。ホストにはwin32u.dllのNtGdiDdDDILock2+0xC、NtGdiDdDDICreateAllocation+0xCも存在し、ドライバ側のCPU処理だけでなくOS側のロック／割当API呼出しも含む。
- クライアントにはD3D9側の命令もあり、ドライバ処理と呼出し側の遷移を記録できた。NVIDIA非公開内部の処理名や全時間配分を推測で埋めない。
- 実採取周期中央値569.8／569.9µs。所有者の採取停止上限32.6〜402.5µs／33.5〜46.8µs。デバッグ計測の影響を無視しない。
- デバッガは両側正常ENDあり。起動時の期待breakpointと、終了競合による最後のSuspendThread error=5を別記。所有者8件の採取APIエラーは0、1544確定Fの状態一致。無劣化なので巻戻し回数は合否条件から外し、確定状態比較は維持。

最終の3つのバイナリは成功した一括実行のSHA256と一致し、すべてPE Machine0x014C。55 INIの実験前後ハッシュ一致。根拠は `build_logs/gauge_final_20260911/final_integrity.json`。既存ゲームを停止せず、各試験が起動した対象だけを終了した。実ゲームデータ・コントローラ設定・レジストリの変更はない。
