# コミュニティ版EXEに対応し起動時のSHA-256を検証する

2026-09-13。指定されたMBAA.exeを独立したテストコピーへ配置し、起動高速化を再有効化した。起動拒否は対応EXEの判定漏れであり、高速化によるゲーム本体ファイルの変更ではなかった。[報告と調査経緯](../issues/BUG_REPORT_TRAINING_LAUNCH.md)。

## 対応するファイル

対象は32bitのCarnival Phantasm Ver.1.07 Rev.1.4.0。コミュニティで主流という位置付けはユーザー申告。

| EXE | 全体SHA-256 | .text FNV64 | SizeOfImage |
|---|---|---|---|
| 今回指定されたコミュニティ版 | `6d1415ca9573100e86a779ac2f81e9bedd322664e3daeae0229a67d13720310a` | `8094b5405e07e85c` | `0x3b5000` |
| 従来の検証用 | `04b5bbd582fd795ea2fd27acb5beb2c4e958c6840b1054cda4cb0b70481d949d` | `67dfc81b81c56fe1` | `0x3b4000` |

元ファイルは `E:/06_game/04_メルブラ/MBAA.exe`。読取と独立コピーだけを行い、このファイルの内容は変更していない。Steam版はこのプロジェクトでは引き続き起動対象外。

## 差分確認

両EXEはImageBase `0x400000`、入口 `0x4e3d7e`、.textの位置と長さが同じ。コミュニティ版にはRVA `0x3b4000`、4096バイトの `.new00` がある。

- .textの481差分バイトを173命令の逆アセンブル比較ですべて照合。152命令は追加セクション内の英語文字列への参照、19命令は文字列付近の長さ指定の変更。
- 残り2命令は `0x41e185` の `0x54c94c` への書込みをNOP化し、`0x483217` の参照を `0x54c94c` から `0x54c950` へ変更している。周辺は文字描画の幅・高さ計算とみられるが、変数の完全な意味までは確定していない。
- .rdataは同一。.dataは59個のDWORDに差があり、54個は追加セクションへの文字列ポインタ。残り20バイトは入力処理 `0x4a02d6` が参照する `0x54d2c0` のキーコード配列。リソースにも差がある。
- 起動素材・初期化・メニュー・暗転・CS解放・Present・描画結合・音声準備・入口・入力クリア・ウィンドウ・負荷試験の主要14か所の命令列が一致。既存の固定アドレスと各フックの個別署名検査を維持した。

根拠は `build_logs/community_support_20260913/instruction_changes.json`、`data_changes.json`、`reference.asm`、`community.asm`。誰がいつEXEを変更したかは未特定。

## 実装

- CLI／GUI共通のゲーム起動経路で、Windows BCryptによるファイル全体のSHA-256を毎回計算する。実パス・実ハッシュ・検査結果を表示し、上記の版別ハッシュと一致しなければプロセス生成前に拒否する。ヘッダーや.textが同じでも、他の領域だけ異なるファイルを許可しない。
- 検査用ファイルハンドルは書込み・削除の共有を許可せず、起動処理完了まで保持する。ハッシュ計算失敗・読取失敗も起動を許可しない。
- `Carnival140Community` を独立した版識別として追加。DLLはフック導入前に.text全体を照合し、コミュニティ版は.new00全体のFNV64 `be3eff2892bc3d03` も照合する。ランチャーで照合する全体SHA-256とは別の、ロード後の検査。
- CS解放、描画結合、音声準備、Training計測、負荷試験に残っていた従来版だけのSizeOfImage条件を共通版判定へ統一した。Steam版に固定アドレスを流用しない。
- 起動高速化を既定ONへ戻した。DDS直接転送・初期化省略・先行準備・ゲーム時計加速・Sleep／中間描画省略・暗転短縮を再有効化。DLL準備同期とキャラ選択後の通常待機を維持。通信版10／拡張6に変更なし。

## 検証結果

32bit Releaseビルド成功、CLI／GUI／DLLのPE Machineはすべて `0x014c`。37 CTest成功、既知3 EXE・SHA標準ベクトル・改変・欠落を含む633検査は失敗0。隔離CLIで不在・排他ロック・破損・.text改変・.text外改変・.new00改変・Steam版の7ケースを実行し、すべてゲーム生成前に拒否した。

| 実ゲーム試験 | 条件 | 結果 |
|---|---|---|
| コミュニティ版Training | GUI worker、未作成の素材キャッシュを指定、1回 | 入力経路2.823秒、Present経路2.835秒、Trainingモード一致。その後2秒維持 |
| コミュニティ版同士 | 同一PC2窓、脚本入力、40秒、15〜25ms・5%損失、短縮接続コードで参加 | 1,295確定F、入力・フレーム・代表状態・保存領域の差分／欠落0。RB 1／11回 |
| 従来版＋コミュニティ版 | 同条件、各側のランチャー／DLLは今回ビルド | 1,234確定F、差分／欠落0。RB 8／9回 |

両対戦で全側のSHA-256成功、起動高速化ON、フック署名一致、WASAPI activeを確認。Trainingでは296枚のDDS直接転送、初期化省略、Trainingの指定モードと描画・入力経路への到達を確認した。2.835秒は単発の起動結果であり、過去の3回中央値との性能比較ではない。

元の指定EXE、通常テスト1・2、独立したCommunityRegression／CommunityMixedRegressionのEXE・INI計77件を前後照合して不変。試験で生成したプロセスだけを終了した。

根拠は `build_logs/community_support_20260913/` の以下のファイル。

- `build.log`、`ctest.log`、`game_build_test.log`、`launch_results.json`
- `training/result.json`、`training/game_1.log`、`training/launcher_1.err`
- `pair/rollback_comparison.json`、`mixed_pair/rollback_comparison.json` と各ゲーム・ランチャーログ
- `preservation_before.json`、`preservation_after.json`

## 反映と残件

通常テスト1・2と独立CommunityRegressionの1・2へCLI／GUI／DLLを反映し、ビルド元ハッシュと一致。以前のバイナリは `deploy_before/` に保管し、`deployed.json` に配置先を記録した。

差し替え用ZIPは `build_logs/community_support_20260913/cccaster_B_community_fastboot.zip`。3バイナリ、英語置換手順、SHA256一覧だけを収録して再読照合した。ZIPのSHA-256は `64147c6eebfa5dcf62ffefda2a29b49f19402c5909a6dbc0b7e03d3a295b9997`。ゲーム本体・設定・ログは含めない。外部公開はしていない。

指定EXEでの起動とローカル対戦疎通は確認済み。報告者PC、別PC／別回線、全キャラ、実画面目視と物理コントローラー操作は今回未確認。描画・入力APIの到達を物理表示・手入力応答の検証とはしない。保存領域の全状態への完全性と、瞬間フレーム精度の再計測も今回の範囲外。
