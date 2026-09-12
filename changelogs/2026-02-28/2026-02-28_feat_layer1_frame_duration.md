# feat: レイヤー1(RTTベース基礎時間延長)の組み込み

## 変更概要
ロールバック処理の過負荷やネットワーク遅延を考慮したレイヤー1（RTTベース基礎時間延長）の関数呼び出しをゲーム内の各画面遷移パスに組み込みました。

## 修正内容
- 以下3ファイルにおいて、`GC::UpdateLayer1FrameDuration(ctx.delay, ctx.maxRollback)` の呼び出しを追加
  - `SceneCharaSelect.cpp`
  - `SceneInGame.cpp`
  - `SceneLoading.cpp`
