---
name: cccaster-verify
description: CCCaster_v10の32bitビルド、変更に応じた短い実対戦と同期ログ比較、配布物作成を行う。CCCasterの検証・リリース依頼、または入力・時計・通信・フック変更の確認時に使う。
---

# CCCasterの検証・配布

## 作業場所と範囲

ユーザー指定のCCCasterリポジトリ／Worktreeを使う。指定がなければ `I:/work_space/CCCaster_v10`。対象のAGENTS.md、docs/CURRENT_STATE.md、docs/OPEN_ISSUES.mdを読み、`git status --short`で既存変更を確認する。人数指定がなければ単独で作業し、指定された場合はその人数のサブエージェントを使う。

書類だけならリンク・差分の確認で十分。通常コードは必要なビルドとテストまで。入力・同期・タイミング・フック変更では短い実対戦を加える。既に同じソースで成功した確認を理由なく繰り返さない。

## ビルド

PowerShellで対象リポジトリを作業ディレクトリにする。

```powershell
$env:PATH='C:/msys64/mingw32/bin;C:/msys64/usr/bin;'+$env:PATH
cmake --build build -j8
# 成否を確認してから次へ進む。
ctest --test-dir build --output-on-failure
```

未構成Worktreeはdocs/DEVELOPMENT.mdの新規構成手順を使う。元フォルダーのCMakeCacheや生成済みMakefileをコピーしない。配布前にEXE/DLLのPE Machineが0x014cであることを確認する。

## 短い実対戦

実ゲーム2窓を使う場合、対象はその作業場所の `_TEST_MBAACC/MBAACC_1` と `_TEST_MBAACC/MBAACC_2`。存在と既存プロセスを先に確認。Worktreeにゲームがなければ元のテストデータを共有リンクで接続せず、独立したローカルコピーを用意する。既存セッションを停止したり、元フォルダーへ実験DLLを勝手に上書きしたりしない。

```powershell
./src/harness/run_bounded_real_pair.ps1 -Seconds 40 -Port 17860 -Network '15,25,5'
python -X utf8 src/harness/compare_rollback_pair.py <今回出力されたログフォルダー>
```

ポートは空きを確認して選ぶ。脚本は自動入力を使うため手操作試験の代用にはならない。遅延なし測定ではNetworkを省くが、比較器はロールバック発生も要求するので無劣化での比較器不合格を即デシンクと扱わない。failed、different、missingの実値を読む。

時計調査時だけCCCASTER_FRAME_TIMING_TRACE=1を付ける。WASAPI activeとQPC切替を区別し、1F（表示要求）、STEP（実更新開始）、WORK（更新＋描画経過）、再計算を混同しない。実画面確認はComputer Useを使用する。

## 配布と引き継ぎ

ユーザーが配布物を求めた場合、成功したビルドのEXE/DLL、置換手順、通信版、既知の制約、SHA256をZIPへまとめ、内容を再読して検証する。ゲーム本体、ユーザー設定、ログ、秘密情報を含めない。公開や外部送信をこの手順から推測しない。

CURRENT_STATEとOPEN_ISSUESは結果に応じて更新し、changelogを追記する。最終報告は変更点・測定条件・確認できた範囲・未達を明記する。動作保存用コミットを安定版認定と呼ばない。
