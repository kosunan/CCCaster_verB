# 描画の区切りをまとめ、HUDの重複判定を省く

## ゲーム側で確認した処理

`bounded_real_20260911_055752`でBegin/Endのゲーム内戻り先を記録し、対象MBAA.exeをobjdumpで照合。
本線 `build_logs/mbaa_render_disassembly_20260911.txt` に逆アセンブルを保存した。

描画バッチの処理に次の区切りがある（VAは当該版のimage base=0x400000）。

1. 0x4BE358のEndScene → 0x4BE364のBeginScene。間は同じデバイスのvtable読取りと引数pushのみ。
2. DrawIndexedPrimitiveUPと、場合によりエフェクトのBeginPass/EndPass等を実行。
3. 0x4BE4A1のEndScene → CPU側の描画バッファ整理 → 0x4BE51CのBeginScene。間の全135バイトに他のAPI呼出しはない。

通常の最終EndScene（例0x4BF141）や描画先切替は別経路であり、そのまま実行する。
以前の「Draw=0の区間を全部削る」という推測より範囲を狭め、上記2か所の対だけを扱う。

## 変更

- `ScenePairMerge.hpp`：実行中イメージの32bit、PE timestamp=0x4FE44444、SizeOfImage=0x3B4000と対象命令列29/135バイトを照合。RVAから求めた戻り先以外には適用しない。
- 成功したBeginのある同じデバイス・同じスレッドでだけEndを保留し、対応する直後のBeginと相殺する。想定外のBeginなら保留Endを実行して通常経路へ戻す。実EndとResetで追跡状態を破棄する。
- GPU API呼出し・描画先・描画順・CPUバッファ整理はそのまま。戦闘更新・入力・RNG・スナップショット・Presentの処理順には触れない。
- HUDは準備済みガードをGetRenderTarget/GetBackBufferより前へ移した。以降のCOM照会・参照カウント操作を省く。Reset時には準備済みフラグを解除する。
- `CCCASTER_DISABLE_SCENE_MERGE`を指定すると従来の区切りに戻る。通常は照合が成功した箇所を有効にする。
- `CCCASTER_RENDER_PROBE=1`時のSceneMergeFrameに相殺した組数を記録する。RenderApiのBegin/Endは実際に元APIへ渡した回数。

DrawIndexedPrimitiveUPは戻る前に入力頂点データへのアクセスを完了するため、後続のCPU描画バッファ整理を残してEnd/Beginの位置だけを変える根拠になる。
GPUの描画完了とは異なる。EndSceneは非同期であり、投入の区切りを減らしたからGPU負荷が同じ割合で減るとは主張しない。
API根拠：[DrawIndexedPrimitiveUP](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-drawindexedprimitiveup)、[EndScene](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-endscene)。

## 段階的な検証

- HUDガード＋呼出元の計測：`bounded_real_20260911_055752`、1135確定FのREC/FRAME/STATE/MEM差分・欠落0。
- 第1の対のみ：`bounded_real_20260911_060116`、50秒、同一PC2窓、D2/R4、遅延15〜25ms・損失5%。ホストを従来、クライアントを最適化ありにして1751確定F一致。
- 第1の対のみの通常更新1210/1209FではBegin/End中央値113→58組、DrawIndexedPrimitiveUP中央値52→52回。API合計経過時間中央値131.5→115.7µs。別プロセス・異なる予測経過・計測費用を含むため、厳密な速度改善率とはしない。
- Computer Useで第1の対の通常戦闘を確認。両キャラ・背景・攻撃エフェクト・HUDを表示。ピクセル単位の同一性は未検証。
- 2か所とも統合：`bounded_real_20260911_060329`。同じ50秒・遅延/損失条件で従来ホストと最適化クライアントを対戦し、1742確定FのREC/FRAME/STATE/MEM差分・欠落0。
- 2か所版の通常1201/1204FでBegin/End中央値113→3組（最大158→6組）、DrawIndexedPrimitiveUP中央値52→52回。観測API失敗0。API合計経過時間中央値136.2→68.1µs、最大830.7→293.5µs。
- 同じ試験の表示省略1045/585FもBegin/End中央値113→3組、Draw中央値52→52回。API時間中央値120.3→59.9µs。再計算回数・深度が異なるため、ロールアップ全体の中央値1440→634µsをそのまま同条件の速度比とはしない。
- 32bit Releaseビルド成功。17テスト成功。新規テストは未開始・異なるデバイス/呼出元・2種の対の取り違え・Reset後の保留状態破棄を確認する。
- 描画詳細計測を外した両側最適化版：`bounded_real_20260911_060446`、45秒、1468確定F一致。両側WASAPI active、2か所の命令列照合成功。通常928/928Fの更新間隔絶対誤差p99=7.667/15.333µs、最大22.333/18.333µs。従来のタイミングトレースは有効であり、無計測の値ではない。
- 最終表示確認：`bounded_real_20260911_060602`、45秒、描画詳細/PACEトレースOFF、両側最適化。1459確定F一致。Computer Useで最終版の通常戦闘を確認し、両キャラ・背景・HUDの表示を確認した。全描画のピクセル比較ではない。検証で起動したプロセスだけを終了し、既存GUI2プロセスを維持。

比較起動では `CCCASTER_TEST_BASELINE_HOST_SCENE_PAIRS=1` をrun_bounded_real_pair.ps1へ渡す。ホストだけ無効化し、スクリプト終了時に元の環境変数へ戻す。

## 残す課題

再計算中のバックバッファDraw省略やオフスクリーン転送省略は実装しない。Lock/Unlockや後続描画への依存の調査が必要。
Resetの追跡解除は実装し、単体検証するが、実機のデバイス喪失・復旧、全キャラ全技、別GPUは未確認。
通常フレームのスパイク解消、GPU完了時間、全描画のピクセル一致は今回の成功条件と混同しない。
