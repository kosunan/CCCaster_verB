# Steam版の解析とRVAによる版別アドレス解決

状態: **移植は未完了**。版識別・実ロード先の取得・読取り専用観測まで実装。Steam版のネット対戦・入力フック・ロールバックはまだ有効にしていない。

## 対象と保全

- 追加対象: `I:/SteamLibrary/steamapps/common/MELTY BLOOD Actress Again Current Code/MBAA.exe`。
- 解析・起動用に `_TEST_MBAACC/Steam_Analysis` へ独立コピーを作成。元ゲームのEXE・設定・SteamのASLR設定は変更していない。
- 単独エージェントで実施。既存の未コミット変更を保持。作業中に起動短縮関係のソースが更新されているため、それらと今回の版識別・RVA変更を混同しない。
- ユーザー提供の旧調査表を参照。表の値は基準VAとして扱い、現在のEXE・実ロード位置・命令の読書き先に照合する。

| 項目 | カニファン版 | Steam版 |
|---|---|---|
| Machine | 0x014c | 0x014c |
| ファイルサイズ | 1,400,832 | 1,912,832 |
| TimeDateStamp | 0x4fe44444 | 0x586e1804（2017-01-05 UTC） |
| SizeOfImage | 0x3b4000 | 0xf3e000 |
| ディスク上のImageBase | 0x400000 | 0x400000 |
| Entry RVA | 0xe3d7e | 0x130982 |
| ASLR | 無効 | 有効（DllCharacteristics 0x8140） |
| SHA256 | `04b5bbd582fd795ea2fd27acb5beb2c4e958c6840b1054cda4cb0b70481d949d` | `11270cf2da851054aa6c1bff309d50b042429abd936dca5df833bcd1de5e6c46` |

`build_logs/steam_analysis/{legacy,steam}_identity.json` にセクション一覧を保存。

## 採用した方式

実アドレスは **ロード先＋RVA**。実行ファイルのASLRフラグを落としたり、任意の固定位置へ移したりしない。

例: ゲームモードの基準VA `0x5b391c` はRVA `0x1b391c`。今回の実ロード先 `0x320000` では `0x4d391c`。

- `GameImageAddress.hpp`: RVAとアクセス長の範囲検証、加算オーバーフロー検証。
- `RemoteGameImage.hpp`: 32bit CREATE_SUSPENDED直後の初期スレッドのEBXからPEBを読み、ImageBaseAddressを取得。PEヘッダーも検証し、ランチャーの入口固定 `0x400000` を解消。DLL内での将来のSteam解決には `GetModuleHandleW(nullptr)` を使用する。
- Windowsはロード後のOptionalHeader.ImageBaseも実配置へ変更していた。ディスクの優先位置とロード後の位置を版識別で混同しない。最初のSteam停止中検証の `verified=0` をこの差で修正し、再検証成功。
- ゲームが格納したポインターは既に実アドレス。入力元ポインター等へロード差分を二重に加算しない。
- 旧版ランタイム自体の固定アドレスをSteamへ自動変換する実装はまだない。RVA基盤の実装と、全アドレス／ABI／保存範囲の移植を区別する。

## 確認できた主な対応

以下のSteam列はすべて基準VA。実際の読取りはRVAに変換する。用途・書込み可否まで一括承認した表ではない。

| 用途 | 旧版 | Steam候補／確認先 | 根拠・範囲 |
|---|---|---|---|
| ゲームモード | 0x54eee8 | 0x5b391c | ユーザー表、0x48b33b/0x48b347の状態コピー。実機20→1を確認 |
| 次モード | 0x55d1d0 | 0x5ca9b4 | 同じ状態ディスパッチャー |
| ワールド時計 | 0x55d1d4 | 0x5c4418 | 0x48a78cのinc。5秒の21観測で18146→18446 |
| 入力元ポインター | 0x76e6ac | 0x5ca9b8 | 初期化0x5194cc、実機値0x6f8428（基準VAでは0x7d8428） |
| P1方向／ボタン | pointer+0x18/+0x24 | pointer+0x18/+0x24 | Steamの派生入力構築0x4762c8〜0x4762e3。表の0x7d844cは元ボタンバッファと一致 |
| プレイヤー配列 | 0x555130 | 0x5bc370 | 0x472f2b等、ストライド0xafc。戦闘中enabled=1、HP=11400 |
| P1 X/Y | 0x555238/0x55523c | 0x5bc478/0x5bc47c | ユーザー表・命令参照・実読取り |
| 乱数値／呼出数 | 0x563778/0x56377c | 0x5ca9bc/0x5ca9c0 | 0x479990〜0x4799da。実読取り |
| 乱数インデックス／状態 | 0x564068/0x564070 | 0x5cb2b8/0x5cb2c0 | 1〜55のインデックスで0x5cb2bc+index*4を使用。状態220Bを読取り |
| 実ラウンド時計 | 0x562a40 | 0x5c9c54 | 命令比較・実読取り |
| ラウンド残時間 | 0x562a3c | 0x5c9c58 | 命令比較・実読取り（隣接順は旧版と異なる） |
| トレーニング停止 | 0x562a64 | 0x5c9c7c | ユーザー表と命令比較が一致。停止操作による値の検証は未実施 |
| ダミー状態 | 0x74d7f8 | 0x7b4324 | ユーザー表。読めることと全操作の意味確認を区別 |
| 画面内状態 | 0x74d598 | 0x7b40a4 | 命令比較・戦闘中255を観測 |
| 効果音フラグ | 0x76e008 | 0x7d47b0 | 0x521956等、1500要素の走査 |
| 効果音オブジェクト配列 | 0x76c6f8 | 0x7d2d90 | 0x52195f。再計算時の抑止ABIは別途移植が必要 |

ユーザー表のP1_ADDRは `旧0x555140→Steam0x5bc378`。現行コードの構造体先頭は旧 `0x555130`、シーケンスは旧 `+0x10`。Steamの配列先頭は `0x5bc370`、`0x5bc380` への命令参照もあり、表の「開始位置」と内部フィールドの意味を同一視しない。8B差の用途は引き続き照合する。

表のDamage2旧値 `5.57E+12` は表計算ソフトの自動変換と思われるためアドレスとして採用しない。旧CAMERA1_Y列がX列と同値の箇所も、現行コードではY=`0x564b18`として別に扱う。派生入力（旧0x55541b/0x55541d）への直接書込みは追加しない。

## 旧版パッチを流用できない具体例

- 旧 `0x41f098` と `0x41f0a0` は個別のDWORDストア。Steamでは `0x476219: movdqu [esi],xmm0` にまとまっており、方向・補助値・ボタンを16B同時クリアする。単純な2B/3B NOP移植は不可。
- 効果音再生は旧版のEDI使用からSteamのECX使用へ変わっている（Steam `0x52195f`→`0x466c60`）。現行インラインASMの継続先とレジスター前提を流用しない。
- `GameSnapshotLayout.hpp` の61個のルート、派生するポインター構造と追加6領域は未移植。隣接変数の並びも変わるため、区間全体を一定オフセットで移すことはしない。
- Steam側にはSSEを使う演算・異なる呼出規約・Steam用の追加処理がある。両版混在ネット対戦の決定性は未検証。

## 実装した識別・拒否

`GameBuild.hpp` はPE32ヘッダーと `.text` 全VirtualSizeのFNV64を照合して既知版を識別する。これは偶発的な版違い／コード改変の検出で、暗号学的署名ではない。診断側は実ファイル全体のSHA256も記録する。

ランチャーは未知版・コード不一致・未移植Steam版をプロセス生成前に拒否。DLLは旧版の実ロード位置・PE情報・全コード（ランチャーによる入口2Bロックのみ正規化）を照合し、拒否時は多重起動APIパッチや初期化スレッドを開始しない。Steam対応を有効にするには新しいランタイムの検証が必要。

空白を含むパスはCreateProcessAのapplicationNameに明示し、変更可能な引用付きcommandLineを渡す。入口待機は30秒で失敗し、今回生成した子だけを終了する。

## 検証結果

- 32bit全ビルド成功。EXE/DLLのMachine=0x014c。`build_logs/steam_analysis/artifacts.json` に今回確認した成果物のSHA256。
- 22 CTest成功。実EXE2版を含む版識別・コード1B改変・切断・RVA境界検証574項目成功。`ctest.txt` / `identity_tests.txt`。
- `probe_suspended_image`: Steamのbase=`0x320000`、entry=`0x450982`、mode=`0x4d391c`、旧版のbase=`0x400000`、entry=`0x4e3d7e`、どちらもverified=1。停止中に読み取るだけで、子の命令実行・DLL注入・メモリ書込みなし。`suspended_steam.txt` / `suspended_legacy.txt`。
- Steam専用コピーで起動・キャラ選択・戦闘中の読取りを確認。`runtime_steam_3352.jsonl` は5秒・21回の観測。非同期の読取りなので整合したゲームスナップショットや瞬間フレーム精度の測定ではない。操作のあったSteam検証窓は保全。
- 旧版通常試験 `build_logs/bounded_real_20260911_121719` はゲーム起動前の接続交渉で停止し、今回の入口／DLLコードへ未到達。合格扱いにしない。
- 切分けの `CCCASTER_STARTUP_BASELINE=1`、40秒、同一PC2窓、遅延15〜25ms・損失5%、D2/R4: `build_logs/bounded_real_20260911_121821`。1095確定FでREC/FRAME/STATE/MEMの差分・欠落0。ロールバック199/221回、最大深度3/3。両側WASAPI active。`legacy_pair_comparison.json`。通常の新接続交渉の合格やSteam対戦の成功を意味しない。

## 再実行と次の作業

```powershell
python -X utf8 src/harness/inspect_mbaa_build.py '_TEST_MBAACC/Steam_Analysis/MBAA.exe'
python -X utf8 src/harness/inspect_mbaa_build.py '_TEST_MBAACC/Steam_Analysis/MBAA.exe' --pid <今回のPID> --seconds 5 --output <新規JSONL>
build/bin/probe_suspended_image.exe 'I:/work_space/CCCaster_v10/_TEST_MBAACC/Steam_Analysis/MBAA.exe'
```

命令比較は任意の解析用依存 `pefile` / `capstone` を使用する。今回のローカル導入先は無視対象の `build_logs/steam_analysis/deps`。

```powershell
$env:PYTHONPATH='I:/work_space/CCCaster_v10/build_logs/steam_analysis/deps'
python -X utf8 src/harness/compare_mbaa_code.py '_TEST_MBAACC/MBAACC_1/MBAA.exe' '_TEST_MBAACC/Steam_Analysis/MBAA.exe' --window 9 --relaxed-registers --output <新規JSON>
```

`port_candidates.json`: 33,698命令位置・4,956参照値の**候補**。正規化は誤対応も含み得る。曖昧な候補は保持し、実行パッチへ自動昇格させない。

残る順序: Steam入力ポーリング／元バッファの所有境界 → 正規モード遷移・再戦メニュー → 版別データ参照と保存範囲 → 効果音／描画のフックABI → Steam同士の遅延・損失つき2窓比較。旧版との同等動作、Steam同士のロールバック、両版混在はすべて未達。
