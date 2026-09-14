# システム情報収集を起動経路から外して秒単位で短縮する

後続作業で残る素材初期化を短縮した。[起動素材の変換結果を再利用する](2026-09-11_startup_assets.md)を参照。以下はDxDiag省略時点の記録。

2026-09-11。今回は人数指定がないため単独で解析・実装・実測した。対象はカニファン版MBAACC Ver.1.07 Rev.1.4.0、32bit。Steamは別プロジェクトであり、この変更の対象外。

## 原因の確定

最初のD3D factoryとCreateDeviceの間にあった約3.6秒を、実関数のCALL前後でQPC計測した。大半は設定画面のシステム情報収集で費やされていた。

| CALLサイト → 対象 | 変更前の診断回 | 意味 |
|---|---:|---|
| `0x4C819F → 0x41E470` | 13.530ms | pack準備 |
| `0x4A3DF9 → 0x414F10` | 0.562ms | 音声初期化 |
| `0x4C81C8 → 0x4A3E20` | 3416.246ms | 起動設定全体 |
| `0x4A1F3F → 0x4A2190` | 3231.601ms | 設定画面のシステム情報ページ |
| `0x4A21FA → 0x417020` | 3229.347ms | DirectX診断情報の収集 |

親子の区間は包含関係があるので、上表を合計しない。根拠は `build_logs/startup_seconds_20260911/profile_before.json` と `skip_probe/game_1.log`。この回は版基盤の分離に伴い省略が未適用（`skip=0`）で、情報収集を実行したログである。

`0x417020`はCoInitializeEx、CoCreateInstance、IDxDiagProviderのInitialize、GetRootContainerなどを呼ぶ。実行ファイルの`0x51B644`のCLSIDと`0x51B634`のIIDがローカルSDK `C:/msys64/mingw32/include/dxdiag.h`のDxDiagProvider/IDxDiagProviderと一致。`szOSExLongEnglish`、`szProcessorEnglish`、`szPhysicalMemoryEnglish`などを読み、ページへ表示し、`System/_Sysinfo.txt`を書き出す。

`0x4A2190`は最後に`0x417170`でCOMオブジェクトを解放し、関連ポインターとCoInitialize成功フラグを0へ戻す。直接参照を全.textで確認し、この情報収集状態はゲーム資源・モード初期化の入力ではない。起動時に自動で閉じる設定画面の診断表示のために、毎回秒単位の待ちが発生していた。

## 対策

`StartupSystemInfo.hpp`で、システム情報ページのWM_INITDIALOGが行うCALL `0x4A1F3F`の5byteだけをNOPにする。ページ全体や設定ダイアログを飛ばさず、映像設定・入力設定・設定ファイル読込みと保存を維持する。呼出側の引数回収と戻り値生成は残す。

DLLの既存版・コード照合に加え、対象ページの前後命令・元CALLを照合する。準備イベントによりゲーム入口が停止している間に設置し、最初のゲームフレームで元CALLへ復元する。後からそのページを開く経路は元に戻る。通常利用では情報収集を省略するが、既存の`_Sysinfo.txt`は削除・更新しない。

Training/Versusかつ対応ランチャーの起動イベントがある場合のみ有効。照合や書込み準備が成立しなければ従来動作に戻り、書込み後のキャッシュ・保護復元失敗は不完全な命令のまま進めない。比較用に`CCCASTER_STARTUP_SECONDS_BASELINE=1`でこの省略だけを無効化できる。

診断回では設定全体が3416.246→190.204msとなった。全体約1.5秒のゲーム資源準備は維持し、そのうち約0.8秒は`0x4AF4E0`で`grp/RoundCall_AA/Ko/ko_ef...`等を読み、`0x4BD2D0`で画像を準備する処理と分かった。これを省くとKO演出を失う可能性があるため、今回の対策には含めない。

## 比較結果

前回の起動高速化を両条件で有効にし、今回の情報収集省略だけを比較した。同一PC、Trainingは1窓、Versusは2窓。workerプロセス生成直前から、キャラ選択の正しいモード・正常入力経路・成功Presentの全条件が成立するまでを計測し、その後2秒維持する。GUIクリックやディスプレイ発光の時間ではない。

| 各3回の中央値 | 変更前 | 変更後 | 短縮 |
|---|---:|---:|---:|
| Training | 6.712秒 | 3.379秒 | **3.333秒、49.7%** |
| Versus、両側完了 | 8.908秒 | 5.098秒 | **3.810秒、42.8%** |

範囲: Trainingは6.590～8.490→3.363～4.268秒、Versus両側は8.818～9.588→4.796～5.954秒。各条件を交互に測定した。前回の6.807/8.874秒は別試行なので、今回の差分計算には使わない。

追加のVersus高速1回は、計測器が片側の子プロセスを捕捉できずtimeoutになった。両ゲームの正しいモード・正常入力・成功Presentは6.018秒で記録され、同期後の進行もログに残っている。この試行は `excluded_discovery_timeout` に原結果を保存して完走3回の集計から除外した。PID再利用を誤除外し得る判定を取り除き、既存対象パスの事前拒否＋自分が起動したworkerの子＋実行パス照合で対象を限定し直して再測定した。ゲームの起動失敗と、この計測器の捕捉不備を区別する。

診断フックは比較時に無効。比較後に診断専用の「旧方式で入口を先に解放する場合には設置しない」ガードとGetLastError保全を追加した。通常成功経路は同一。通常経路は後続の実対戦で確認した。

根拠: `build_logs/startup_seconds_20260911/final_*/result.json`、`final_summary.json`。各試行に使用EXE/DLL/ゲームのSHA256を記録。`bench_startup.py --comparison system-info` と `summarize_startup.py`で再現できる。関数内訳は `analyze_startup_calls.py`で集計する。

## 確認範囲

最初のゲームフレームでCALL復元、キャラ選択でモード・入力・表示の成立を確認。比較前後で両テストコピーの9個のINI設定のSHA256が一致（`config_check.json`）。実ゲームファイルへのパッチ書込みは行わない。

変更前の情報収集を行うホストと、省略するクライアントの40秒実戦は15～25ms遅延・5%欠落下で1357確定FのREC/FRAME/STATE/MEM比較が一致、different/missing/failures=0。307/330回のrollbackを含む。比較器が除外する絶対WT・メニューカウンタは従来の検証定義どおり。根拠: `build_logs/bounded_real_20260911_124219/rollback_comparison.json`。

双方省略の40秒実戦も同じ遅延・欠落条件で1513確定F一致、different/missing/failures=0、193/409回のrollbackを含む。根拠: `build_logs/bounded_real_20260911_124400/rollback_comparison.json`。全体32bitビルドと22 CTest成功。更新CLI/GUI/DLLはPE Machine 0x014c、SHA256を`final_binaries.json`へ保存。

更新ファイルを両テストコピーのcccasterフォルダーへ配置。稼働中の旧GUIを保全しており、起動には `_TEST_MBAACC/MBAACC_1/cccaster/CCCaster_v10_GUI_startup.exe` を使う。通常起動で診断フックは無効。ゲームEXEとINIの内容は保全する。

冷起動、別GPU/PC、WAN、全キャラ・全演出、物理コントローラの総合確認は未実施。残りの起動時間は実資源・デバイス・入力準備と正規画面遷移が中心。この変更はその準備を後の戦闘へ先送りしない。
