# トレーニングのラウンドリセットでBGMと保存状態が消える

現行操作は[FN1保存長押し停止・FN2リセット後の自動ロード](../design/2026-09-14_training_fn.md)へ変更済み。以下は独立Save/Loadボタンを使用していた時点の修正記録。

2026-09-14 修正。ゲーム内のFN2でラウンドをリセットするとBGMが再開始し、保存したステートを読み込めなくなるとの報告。

## 続報：ヒットストップ中の保存（2026-09-14）

前項のBGM・ラウンドリセット後の保存保持は、利用者から修正確認の報告を受領した。新たに双方のヒットストップ中に保存できないとの報告を受け、以下を修正。

- 原因: `TrainingState::Step` が `sample.stopped` を一律に遮断。これは一時停止だけでなく、各プレイヤーのヒットストップや技の全体停止も含む。
- 修正: 遮断条件を `sample.paused` に限定。F4・メニューの一時停止・無効な画面では従来通り押下を抑止し、戦闘のヒットストップと全体停止は保存・読込可能にした。停止解除まで保存を遅延させず、押下時の状態を保存する。
- 保存／読込イベントへ停止状態を追記。`players=1/1` で保存されたこと、読込直後の `restoredStopped=1` を確認できる。全フレームの追加ログではなく、コマンド実行時だけ記録する。
- 32bitビルド、38 CTest成功。停止中の保存・読込、押しっぱなし抑止、全体停止、メニュー一時停止とF4の遮断を単体試験。`test/logs/training_hitstop_build.log`、`training_hitstop_ctest.log`。
- 実ゲームは独立コピー＋仮想DS4の通常設定経由。`run_training_reset_probe.py --hitstop` により接近して打撃を当て、双方のヒットストップカウンタが6の時点で保存ボタンを押した。
- 修正前 `test/logs/training_reset_20260914_202551/`: ボタン押下後に保存イベントが発生しないことを再現（期待した不合格）。比較DLLは前回のラウンドリセット修正版 `cf29abb2...`。
- 修正後 `test/logs/training_reset_20260914_202633/`: `event=1 saved=1 stopped=1 paused=0 players=1/1` で保存成功。同じ状態を3回読み込み、毎回 `restoredStopped=1`。通常の保存→FN2→読込も3回成功。元コピーのINI不変。実ゲーム確認はこの打撃に限定し、全キャラの特殊技・全体停止・物理パッドは未確認。
- テスト用MBAACC_1・2へ反映し、DLLハッシュ一致・PE Machine 0x014c・25 INI不変を確認。`test/logs/training_hitstop_deploy/result.json`。DLL SHA256: `9d9aeb7b28d65f9a789d9bc284fd01961cbd82bce271a416770e1cbd7612267c`。
- 今回はTrainingのコマンド受理とイベントログだけの変更で、ネット対戦の再試験は行っていない。以下の対戦試験は前回の結果。

## 原因と変更

- BGM: 旧版 `I:/work_space/CCCaster/OLD/targets/DllAsmHacks.hpp` の `disableTrainingMusicReset` が現行版に抜けていた。対応EXEの逆アセンブルで、0x472C6Dの `75 05` の直後がBGM停止処理0x4DDF40へのCALLであることを確認。旧版と同じ `EB 05` へ変更し、CALLだけを飛ばす。対象7バイトの一致を確認し、Training起動時だけ適用する。
- 保存状態: `ReadTrainingFrame` はイントロ中に無効サンプルを返す。`TrainingState::Step` が無効サンプルで保存スロットまで消去していた。戦闘画面内の無効サンプルでは保存状態を保持し、押しっぱなしの保存・読込は解除待ちにする。キャラセレなど戦闘外、モード変更、ラウンド番号／操作キャラの変更による破棄は維持。実機のFN2ではラウンド番号は0のままである。
- スナップショット領域、通信版10、入力時計、通常ラウンドリセットのゲーム処理は変更していない。

## 確認

- 32bit Releaseビルド、38 CTest成功。無効サンプル→有効サンプルの保存維持、押しっぱなしの読込抑止、解除後の読込を単体試験へ追加。ログ: `test/logs/training_reset_build.log`、`training_reset_ctest.log`。
- 独立したゲームコピーとViGEm DS4を使用。試験用の入力バイパスを使わず、通常のWireless Controller設定経路で保存→FN2→読込を行った。
- 修正前: `test/logs/training_reset_20260914_200948/`。保存成功後、3回とも `event=3 saved=0`（保存なし）。保存位置へ戻らないことを再現。
- 修正後: `test/logs/training_reset_20260914_201027/`。同じ保存スロットで3回とも `event=2 saved=1`、保存時のX座標へ復元成功。元ゲームコピーのINI不変。BGMパッチ適用ログと全採取サンプルの実メモリ `0x05EB` を確認。BGMの聴感／再生カーソル連続性の測定は未実施。参考のBGM内部値だけから音の連続性を判断していない。
- 初回の `training_reset_20260914_200912/` は仮想パッドのFN2押下ボタンが違い、位置リセットに到達せず試験不成立。その後、B9に対応するOPTIONSへ修正して上記A/B試験を行った。
- 40秒・同一PC2窓、15〜25ms／5%損失: 1,328確定Fの入力・代表状態・記録MEM項目に差分／欠落0。ホスト5回／クライアント7回ロールバック。`test/logs/bounded_real_20260914_201106/rollback_comparison.json`。
- `test/runtime/MBAACC_1` と `MBAACC_2` へ反映し、ビルド元とDLLのSHA256一致、PE Machine 0x014cを確認。`test/logs/training_reset_deployed.json`。DLL SHA256: `cf29abb2b87e5948fd6e777824515b5cd81fc422dd093b0f1dfecffbb04e2957`。

再実行は既存のViGEm環境で `src/src/harness/run_training_reset_probe.py` を使用する。`--baseline-dll` に保全した修正前DLLを渡すと比較できる。各回は `test/runtime/TrainingReset_*` に独立コピーを作り、今回起動したランチャーの子ゲームだけを終了する。物理パッドの手操作、全キャラ、報告者側の復旧は未確認。
