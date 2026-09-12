# MBAACCメモリ知識ベース

更新: 2026-09-11。対象は本プロジェクトのMBAACC Ver.1.07 Rev.1.4.0・32bit。
この資料は外部ソースと現行コードを照合した調査記録。追加情報の実ゲーム確認・書込み実装は行っていない。

## 参照元と確度

[MBAACC-Extended-Training-Mode](https://github.com/fangdreth/MBAACC-Extended-Training-Mode)（以下ETM）のコミット `038887d7d8e6e70963ce9eb5780a25ac35cf1cac` を固定して調査した。
[README](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/README.md)はCommunity Editionを対象としSteam非対応と記載している。ただし、これだけで手元EXEと完全一致することは証明できない。Steamへの固定アドレス流用はしない。

- **現行定義と一致**: アドレス・サイズが本プロジェクトにも存在する。今回の実機確認を意味しない。
- **参照ソースで確認**: ETMの宣言・使用箇所で確認。手元ゲームでは未確認。
- **解釈保留**: コメントと宣言の不一致、用途・有効条件・保存の必要性が未確定。

[フィールド索引](etm_fields.md)は9構造体262フィールドを収録する。[JSON](etm_layout.json)には各フィールドの型・配列長・サイズ・相対offset・例示VA・固定コミットのソース行URL、および参照6ファイルのSHA256を保存した。未命名の空白もサイズ計算に含め、9構造体の期待サイズと19件のCHECKOFFSETに一致することを検査した。これはソース上の算術検査であり、C++ビルドや実機検証とは区別する。

## アドレスの読み方

ETMの `Common/Common.h` はモジュール相対値、構造体相対値、絶対VAを混在させている。名前の接頭辞だけで分類しない。

| 種類 | 例 | 計算・注意 |
|---|---|---|
| モジュール相対 | dwP1Health=0x1551EC | 基底0x400000の場合VA=0x5551EC |
| プレイヤー本体相対 | adDirInputBuffer=0x3E8 | P1本体0x555130に加える |
| ActorData相対 | adHealth=0xB8 | P1 subObj=0x555134に加える。本体基点との差は4B |
| 既に絶対VA | dwCameraX=0x0055DEC4 | 0x400000を二重加算しない |
| ETM専用通信領域 | adShareBase=0x381000 | ETMが用意する共有用途。素のゲーム状態に分類しない |

基底0x400000は参照ソースの前提。将来の読取り実装では本プロジェクトの版識別とアドレス解決を通す。ポインタは32bit、掲載構造体はpack(1)。64bitホストのポインタ幅で再解釈しない。NULL・ページの可読性・ロード世代を確認してから動的な参照先を読む。

根拠: [Common.hのアドレス定義](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Common/Common.h#L126)、[現行アドレス定義](../../src/core_dll/mbaa_mem/MbaaAddresses.hpp)。

## 主な構造と配置

| 構造 | 基点・大きさ | 内容・照合結果 |
|---|---|---|
| PlayerData ×4 | 0x555130 / 0x555C2C / 0x556728 / 0x557224、各0xAFC | 本体・パートナー用4枠。現行サイズ・基点と一致 |
| EffectData | exists DWORD + ActorData、0x33C | プレイヤー本体の共通先頭部分でもある。existsはETM宣言4B、現行enabledフラグ読取りは1B |
| ActorData | 本体+4、0x338 | 体力、動作、入力派生状態、攻撃者、所有者、各データへのポインタ |
| EffectData ×1000 | 0x67BDE8、合計828000B | 現行固定保存表のサイズと一致。飛び道具等を含むため単なる描画効果とみなさない |
| PlayerAuxData | 0x557DB8から0x20C刻み | 操作キャラ、コンボ表示、行動不能時間。ETM FullSaveは4枠保存するがFrameBarのP3/P4はP1/P2側の補助データを共有して参照 |
| CommandData | 動的、0x2C | コマンドID・入力表記・遷移先pattern・消費ゲージ・フラグ |
| CommandFileData | PlayerData+0x33Cのポインタ先、0x68 | 空中ジャンプ数、根性値、シールド反撃、投げpattern・間合い等 |

補助構造体を機械的に4プレイヤーへ割り当てない。ETMの定数式で第3枠は0x5581D0だが、対応する古いコメントには別値がある。実使用は[FrameBar.cpp:36](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/FrameBar.cpp#L36)を参照。

## よく使う戦闘状態

以下はP1のVA。P2/P3/P4本体には0xAFCずつ加算する。ActorDataとの詳細な対応は[索引](etm_fields.md)にある。

| 情報 | VA | ETMの型・意味／注意 |
|---|---|---|
| キャラ・ムーン | 0x555135 / 0x55513C | BYTE / WORD。動作中本体のIDであり、キャラセレ設定とは別 |
| pattern / state | 0x555140 / 0x555144 | DWORD。動作番号とその内部状態。現行sequence/seq_stateと一致 |
| 体力・赤体力 | 0x5551EC / 0x5551F0 | DWORD。現行と一致 |
| ガード量・品質 | 0x5551F4 / 0x555208 | float。整数ビット列を数値のゲージ量として扱わない |
| マジックサーキット | 0x555210 | DWORD。ETMコメント例12345→表示123.4%。丸め表示と内部量を区別 |
| 現在X/Y | 0x555238 / 0x55523C | int。現行と新ActorDataが一致 |
| 前回X/Y | 0x555244 / 0x555248 | int。旧dwP1Yは後者をYと呼んでいる |
| 速度X/Y・加速度X/Y | 0x55524C / 0x555250 / 0x555254 / 0x555256 | int / int / short / short。現行と一致 |
| hitstop / receivedHitstop | 0x5552A2 / 0x5552D4 | BYTE。異なるフィールド。旧dwP1HitstopRemainingはreceived側 |
| noInput / tag / remainingHits | 0x5552A7 / 0x5552A8 / 0x5552AA | BYTE。tagの値の意味・所有関係はキャラに依存 |
| ガード硬直 / ガード予定 | 0x5552AB / 0x5552AC | BYTE、inBlockstun / willBlock |
| 打撃無敵 / 投げ無敵 | 0x5552B5 / 0x5552B6 | BYTE。無敵判定は他のデータ条件も必要 |
| 受身不能合計 / 経過 | 0x5552BE / 0x5552C0 | WORD。空中硬直表示はこの差も参照する |
| アーマー時間 | 0x5552C8 | WORD。ETM旧表は0なし・正値ありと記載 |
| 硬直経過 / 硬直残量 | 0x5552D8 / 0x5552DC | DWORD / int。残量は空中で-2という注記があり、符号なしで読まない |
| リバースビート補正 / 減衰時間 | 0x55531A / 0x55531C | WORD |
| 攻撃成立側のコンボ数 | 0x55531E | WORD。補助構造体のコンボ表示カウンタとは別 |
| 向き | 0x555444 | BYTE facingLeft。現行コメントは0左・1右で、極性が食い違うため保留 |
| 動作内経過フレーム | 0x555464 | DWORD framesIntoCurrentPattern。停止中の進行フラグも存在 |

行動可能性は単一カウンタだけで決めない。[FrameBar](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/FrameBar.cpp#L309)ではinactionableFrames、ガード硬直、空中受身不能時間、attackDataPtr、停止や着地を組み合わせる。表示値には残量-1などの補正がある。これをゲームの生カウンタと混同しない。

## キャラクター固有の状態

根拠は[Common.h:152以降](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Common/Common.h#L152)。値域・用途は参照側の記載であり手元未確認。同じ領域は別キャラで異なる意味を持ち得る。

| P1のVA | 意味 | 値の解釈 |
|---|---|---|
| 0x5552F6 | シオンの弾消費数 | 0=13発残、4=9発残。残弾そのものではない |
| 0x555300 / 0x555302 | ロアの非表示／表示チャージ | 旧表は各0〜9 |
| 0x5552E8 | 両儀式のナイフ状態 | 0=所持、1=なし |
| 0x5552FC | Fメイド・翡翠先頭のハート | 0=満タン、5=空 |
| 0x5568F6 | Fメイド・琥珀先頭のハート | 先頭プレイヤーではなくパートナー側。0=満タン、5=空 |

ActorDataではspecialVariables BYTE[10]とextraVariables WORD[10]等の汎用領域として見える。旧アドレス定数のDWORD型は「アドレス値の型」であり、参照先の読取り幅を保証しない。

## 入力履歴と動的な攻撃データ

入力変化ごとの履歴と継続F表示はMBAACC標準機能（ユーザー確認）。下記は内部配列の調査範囲で、標準表示機能の未実装を意味しない。有利不利表示のkosunan版／ETM比較と停止条件は[専用の参照記録](../design/2026-09-11_mbaacc_frame_advantage_reference.md)を参照。

ActorDataの補正方向は+0x2E6、raw方向は+0x2E7。P1では0x55541A / 0x55541B。buttonInputs DWORDは+0x2E8（P1=0x55541C）、buttonReleased DWORDは+0x2EC。ETM注記では押下と保持を同一DWORDの異なる部分に格納する。旧表dwP1ButtonHeldの値説明と新宣言のビット説明はそのまま同一視しない。

PlayerDataの入力履歴はWORD[0x81]ずつで、方向+0x3E8、A+0x4EA、B+0x5EC、C+0x6EE、D+0x7F0、E+0x8F2、F+0x9F4。要素の時間表現・更新順は未調査。これらはゲームの派生状態であり、ネット入力の注入先ではない。変更時も既存GameMem入力経路を使用する。

| 辿る対象 | オフセット（本体基点） | 続き |
|---|---|---|
| patternDataPtr | +0x31C | PatternData 0x58B。+0x34→animationDataContainer |
| animationDataPtr | +0x320 | AnimationData 0x54B。+0x38→StateData、+0x3C→AttackData |
| attackDataPtr | +0x324 | 現在のAttackData。NULLの状態がある |
| charFileDataPtr | +0x330 | CharFileData→HA6Data→patternContainer。全patternの探索候補 |

ActorData基点では上表から4を引く。AnimationData+0x42/+0x43は非攻撃判定／攻撃判定のindex、+0x4C/+0x50は各box表へのポインタ。RawBoxDataはshortのx1,y1,x2,y2（8B）。参照側自身がbox配列の解釈を未確認と注記しており、最大indexを要素数と断定しない。座標の画面変換には向き・位置・カメラ等が必要。

AttackDataは0x50B。+0x00のguardFlags、+0x2C customHitstop（WORD）、+0x2E untechTime（WORD）、+0x38 extraGravity（float）、+0x3C hitFlags、+0x44 damage、+0x46 meterGain、+0x48 vsDamage、+0x4A guardDamage、+0x4C circuitBreakTime（末尾5項目はWORD）を持つ。生damageは補正後の実ダメージとは別。StateDataは0x1Cでstance +0xC、invincibility +0xD、cancelNormal +0xE、cancelSpecial +0xF、flagset2 +0x18。参照側に構造の意味が不確実との注記がある。

根拠: [AttackData・StateData](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L14)、[AnimationData以降](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/DebugInfo.h#L265)。assertのメッセージに旧サイズが残る箇所があるため、文字列ではなく比較式と実宣言を読む。

## 時計・カメラ・RNG・トレーニング設定

| VA | 意味・区別 |
|---|---|
| 0x54EEE8 | 現行game mode。ETMのgame stateと名称が逆。値1は戦闘側でありTrainingの一意判定にしない |
| 0x562A74 / 0x77BF2C | ETMのTraining判定16／versus判定1等。0x54EEE8と別の情報 |
| 0x55D1CC | ETMのFrameCount。FrameBarではスロー中に遅く進む側 |
| 0x55D1D4 | 現行CC_WORLD_TIMER。上と別アドレス |
| 0x562A3C / 0x562A40 | 現行ラウンド残量／開始後経過。後者をETMはTrueFrameCountと呼ぶ |
| 0x562A48 | GlobalFreeze。単一キャラのhitstopとは別 |
| 0x55D208 | SlowMo。ETM FullSaveの保存先メンバーはDWORDでも実読取りはWORD |
| 0x54EB70 / 0x54EB74 | カメラの現在／目標zoom |
| 0x555124 / 0x555128 | カメラ目標X/Y |
| 0x55DEC4 / 0x55DEC8 | ETMの現在カメラX/Y |
| 0x564B14 / 0x564B18 | カメラのコピー／次位置。現行CC_CAMERA_X/Yはこちら |
| 0x563778 / 0x56377C / 0x564068 / 0x564070 | 現行RNG交換: 3状態値と220B、計232B |
| 0x564068から0xE4B | ETM FullSaveのRNG保存範囲。現行RNG交換と一致しない |
| 0x77C1E8 / 0x74D7F8 | ETMのダミー状態設定／コピー |
| 0x77C1F0 / 0x77C1F4 | 防御設定／防御タイプ |
| 0x77C1F8 / 0x77C1FC / 0x77C200 | 体力回復／ゲージ／ガード設定 |

ゲーム内カウンタを実時間時計として用いない。現行のWASAPI/QPC時計・入力採取フレーム番号とは別。RNGもETMの保存範囲で現行の交換・世代合流を置換しない。

## ロールバック保存表との比較

[ETM FullSave::save](https://github.com/fangdreth/MBAACC-Extended-Training-Mode/blob/038887d7d8e6e70963ce9eb5780a25ac35cf1cac/Extended-Training-Mode-DLL/SaveState.cpp#L246)の実コピー範囲と、[現行GameSnapshotLayout](../../src/core_dll/rollback/GameSnapshotLayout.hpp)の固定ノードを比較した。[比較JSON](snapshot_comparison.json)に16領域の結果と現行表のSHA256を保存している。

| 固定保存表外にあるETM保存範囲（終端含まず） | bytes | 次の確認 |
|---|---|---|
| [0x55512C,0x555130) | 4 | カメラ目標3番目の成分の用途・戦闘への影響 |
| [0x5581D0,0x5585E8) | 1048 | 補助構造体の後半2枠。実使用の有無とFrameBarの共有参照との関係 |
| [0x558600,0x558608) | 8 | StopSituation先頭の用途と更新条件 |
| [0x55D1CC,0x55D1D0) | 4 | 表示／停止用カウンタか、ゲーム計算にも影響するか |
| [0x56406C,0x564070) | 4 | ETMのRNGArray先頭、現行表との差の理由 |

**この差は保存漏れやデシンクの証明ではない。** 動的ノード、別の保存経路、ゲームによる再生成、表示専用状態は判定していない。対象版で値の変化・参照命令・復元往復を確認してから保存表への追加要否を判断する。

逆方向にも差がある。ETMは各PlayerDataの先頭0x3E8のみ保存し入力履歴を含めない。現行は0xAFC×4=11248B全体を保存する。ETMの軽量SaveStateはeffectの有効範囲を推定するがFullSaveは1000枠を保存するため、両者を混同しない。現行表にはeffectのanimationポインタ+0x320からStateData+0x38へ辿る子ノードもあり、ポインタ先の追跡を単なる本体コピーへ削減しない。

## 未確認事項と次の利用

1. Y座標の旧命名、向きの極性、notInComboの名前と旧コメントの反転、hitstopとreceivedHitstopを対象版で区別する。
2. 読取り診断を作る際はキャラ・ムーン・フェーズ・フレーム番号・通常更新／再計算を同時記録し、基点・型幅・ポインタ世代を検証する。メモリの採取はゲームスレッドの一貫した時点で行う。
3. 上記5候補のうち実際に変化する状態を絞り、保存・復元後の同一入力再計算で影響を確認する。現時点で保存表は変更しない。
4. 全キャラの固有領域、box座標変換、コマンド履歴の時間表現、動的データの所有・寿命は継続調査。

ゲームコード・ビルド成果物・ゲーム設定の変更なし。今回の確認は資料生成と静的照合のみで、実ゲーム試験は実施していない。

## 再生成

参照ソースの取得先は `I:/work_space/CCCaster_v10_experiments/reference-etm-20260911`。取得したソースやEXEは実行していない。別環境では同リポジトリを取得し上記コミットへcheckoutする。

```powershell
python -X utf8 docs/memory/extract_etm_layout.py I:/work_space/CCCaster_v10_experiments/reference-etm-20260911
```

生成器は指定コミットの変更なしcheckoutだけを受け付ける。ETMの全C++構文を扱う汎用パーサではなく、指定9構造体の宣言を対象にする。未知の宣言やサイズ・offset不一致では停止する。`etm_fields.md`・`etm_layout.json`・`snapshot_comparison.json`は生成物。上流更新時はコミット・構造・期待サイズを再調査する。参照元の権利表記は[ETM_NOTICE.txt](ETM_NOTICE.txt)を参照。
