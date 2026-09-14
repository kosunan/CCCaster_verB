# ゲーム側の多重描画と省略候補

## 今回確認した範囲

対象MBAACC Ver.1.07 Rev.1.4.0、32bit。ゲームの業務処理を維持するため、今回追加したのは明示指定時だけ有効な描画計測であり、描画APIの省略は実装していない。
`CCCASTER_RENDER_PROBE=1`で18種のD3D9 APIと既存EndSceneを計測。すべて元の関数へ転送し、HRESULTを維持する。
実ゲームデバイスから関数を取得し、フック成否を記録する。HUD内の呼出しは除外。
カウンタはthread_local固定配列。各API内でログを出さず、Present入口で1F単位の結果を出す。
120の倍数の適用フレームではEndSceneごとの描画先・寸法・Draw回数を標本記録する。

各時間は実QPCによるAPI呼出しの経過時間（1tick=1/60µs）。GPU完了時間・GPU使用率・転送バイト数ではない。
Texture/Surface/VertexBuffer/IndexBufferのLock/Unlock、すべてのD3D9メソッドは網羅していない。
そのためDraw=0を「GPU書込みも0」とは断定できない。計測・ログ負荷もあるため以前のWORKや揺らぎと単純に速度比較しない。

## 実機根拠

本線 `build_logs/bounded_real_20260911_055324`。45秒、同一PC2窓、自動入力、遅延15〜25ms・損失5%、D2/R4、TimeScale1、更新準備枠3ms。
32bitビルド・16テスト成功。全18追加フックが両側で成功、対象区間の観測API失敗0。
1442確定FのREC/FRAME/STATE/MEM差分・欠落0。ロールバック351/365回、最大深度3/3。
同期比較は既存のabsolute_WT/menu_counter除外条件によるもので、全メモリ同一の証明ではない。

`python -X utf8 src/harness/analyze_render_probe.py build_logs/bounded_real_20260911_055324`

根拠ファイルは `render_analysis.json`、`rollback_comparison.json`、原本game_1/2.log。
通常戦闘はPaceのplay=1、初回ロールバック以降・同一世代に対応させる。
skip=1は表示省略フレームとして別集計し、再計算の同じ適用番号の複数回をseqで分離する。

## 1F当たりの実測

| 項目 | ホスト通常901F | クライアント通常903F | ホスト表示省略647F | クライアント表示省略701F |
|---|---:|---:|---:|---:|
| BeginScene/EndScene組数中央値 | 113 | 113 | 111 | 113 |
| 同最大 | 158 | 158 | 158 | 158 |
| DrawIndexedPrimitiveUP回数中央値 | 52 | 52 | 51 | 52 |
| SetRenderTarget回数中央値 | 15 | 15 | 15 | 15 |
| 観測API合計時間中央値µs | 126.0 | 124.5 | 108.1 | 109.3 |
| 観測API合計時間最大µs | 527.8 | 469.7 | 398.0 | 308.0 |

通常ホストの内訳中央値はDrawIndexedPrimitiveUP=57.5µs、BeginScene=16.1µs、EndScene=27.2µs。
別々の中央値を足した値を全体の中央値とは扱わない。
SetRenderState中央値71回、SetTexture56回、SetSamplerState40回。重複値かどうかは未計測。
UpdateSurface/UpdateTexture/GetRenderTargetData/StretchRect/ColorFillはこの通常戦闘区間の記録にない。ただしLock経由転送等まで否定する根拠ではない。

従来の「EndScene約8回/F」というコメントは通常戦闘全体には適合しない。
今回見えた最終バックバッファの7回程度と、1F全体の100回以上を区別する。
表示省略中も通常と同程度の描画命令を発行しており、Present省略だけではこの負荷はなくならない。

## 描画先と空に見える区間

通常フレーム131400のホストは122組のEndScene。そのうちDraw=0は67組。
描画先は順に1024×256のオフスクリーン、1024×512のオフスクリーン、640×480のバックバッファ。
バックバッファは最後の7組で、Draw合計2回、Draw=0が5組だった。
すべてを同じ完成画面への重複書込みとみなすことはできない。

通常・表示省略を含む抽出標本はホスト863組中469組、クライアント1075組中584組がDraw=0。
標本は120フレーム刻みであり、全フレームの割合を保証する値ではない。

## 省略候補と安全性

1. **再計算中の最終バックバッファへのDraw**。最初に検証する候補。見せない中間画像の最終合成なら省略余地がある。一方、オフスクリーン更新は次の描画やフレームで使う可能性があるため維持する。バックバッファの読戻し・コピー・Lockによる後続依存を調べ、状態比較だけでなく描画結果も比較する必要がある。
2. **Drawを伴わないBeginScene/EndScene組の削減**。回数は多い。空に見えてもClearやリソース更新、シーン外でのみ許される処理が入る可能性がある。単純にEndSceneだけを捨てず、Begin/Endの対応と間の全APIを捕捉してから遅延Beginや組の統合を検証する。
3. **CCCaster側の重複バックバッファ判定**。OnEndSceneはHUD準備済みでも先にGetRenderTarget/GetBackBuffer/Releaseを実行する。準備済みガードを前に置く候補で、ゲーム本体の描画省略より影響範囲が狭い。今回のAPI時間にはこのHUD準備コールバックの費用は含めていない。
4. **同じ値の状態設定**。71/56/40回という回数だけでは冗長と判断できない。値と状態ブロック・Resetの境界を記録してから判断する。

全Draw、オフスクリーン描画、テクスチャLock/Unlock、Begin/End、戦闘更新関数を一括で止める根拠は得ていない。
CC_SKIP_FRAMES_ADDRやゲーム入力・RNG・スナップショットの経路は変更していない。

DrawIndexedPrimitiveUPにもストリーム/インデックス設定を解除する副作用があり、単なる成功戻りへの置換は避ける。
EndSceneは非同期のコマンド投入で、複数組の存在は最適化の手掛かりだが、呼出回数だけでGPU負荷を断定できない。
API根拠：[EndScene](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-endscene)、[DrawIndexedPrimitiveUP](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitiveup)。

今回の結論は「省略対象の候補を実測で特定」。省略による高速化・絵の同一性・全キャラ全技での無干渉は未確認。
