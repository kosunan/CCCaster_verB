# 出力機器別のDirectSound処理時間

ユーザーの指示により波形・音質・録音の確認を中止し、処理時間だけを測定した。録音と既定出力先の変更は実施していない。

## 方法

`src/harness/bench_sound_devices.cpp` をmingw32で32bitビルド。
DirectSoundEnumerateで利用可能な機器を列挙し、GUIDをDirectSoundCreate8へ明示して指定する。
生成した無音PCMバッファにゲームと同じGetStatus→Stop→SetCurrentPosition(0)→SetVolume→SetPan→Playを実行。
準備あり側では本線のSoundPrewarm.hppを直接使用して事前準備する。

- 22050Hz/mono/250ms、44100Hz/mono/1000ms、48000Hz/stereo/250msの3条件。
- 各条件64回×準備あり／なし。各回新規バッファを生成し、両条件の順を交互にする。
- 2回目は先頭を準備ありに逆転。4出力先×3条件×64回×2条件×2順序＝3072バッファ試行。
- 各APIのQPC前後だけを計測。CSV出力は機器の測定終了後。既定出力、システム音量、ゲーム設定は変更しない。
- 音声内容はゼロPCM。実ゲームの全アセット・試合状態の再現ではなく、機器別のAPI処理時間ベンチマーク。
- このベンチマークにはゲームのMMCSS・CPU配置制約を適用していない。個々の外れ値を実対戦へそのまま換算しない。

根拠ログ: `build_logs/sound_devices_20260911_073017` と `build_logs/sound_devices_reverse_20260911_073105`。
列挙結果とGUIDは各 `devices.txt`、生値は `device_0.csv`～`device_3.csv`、集計は `analysis.json`。

## 順序逆転後の中央値

単位µs、各セルは準備なし→あり。各条件・各側64回。

| 出力先 | 22050Hz mono 250ms | 44100Hz mono 1000ms | 48000Hz stereo 250ms |
|---|---:|---:|---:|
| Creative SB X-Fi スピーカー | 87.0→7.1 | 81.6→6.4 | 278.2→7.0 |
| NVIDIA Q27G4Z | 457.6→6.8 | 288.8→7.2 | 87.8→7.4 |
| NVIDIA ZOWIE XL LCD | 447.6→7.3 | 291.6→7.4 | 81.5→7.4 |
| Creative SB X-Fi SPDIF | 441.6→6.3 | 286.2→6.4 | 75.0→7.0 |

先行試行でも準備後中央値6.2～7.5µs。1536回の準備は全てPrepared、復元失敗0、計測対象API失敗0。
4つの物理独立音源ではなく、Creative/NVIDIAという2系統の4出力先。列挙されなかったRealtek等は未試験。
形式と長さ・チャンネル数を同時に変えているため、未準備費用の差をサンプルレート変換だけの原因と断定しない。

## 機器を開いた直後の費用

準備なしを先頭にした最初のPlayは、上記順で11.0128/27.1918/20.4832/14.0148ms。
準備ありを先頭にした試行では、10.7744/25.8783/22.0614/15.2907msがPrepareの所要時間になり、直後のPlayは4.8/7.8/7.7/7.0µs。
機器を開いた直後の初動費用も準備側へ移せた。ただしこの費用は機器の初期稼働を含み得て、すべてを個別バッファの資源準備に帰属できない。
このミリ秒値を既に音が出ている実対戦の初回SFX費用と混同しない。

## 外れ値と残件

先行試行のQ27G4Z/48000Hzの準備後Playに169.0µsが1件。順序逆転後の同条件最大は25.5µs。
逆転後のZOWIE/44100Hzにも69.5µsがある。ETW区間はこの試行では取得していないので、OS割込み・待機・API内部のいずれかへの帰属は未実施。
中央値の改善は再現したが、全スパイク除去は未達。後発生成／消失復元バッファ、別機器での実対戦、長時間処理時間を今後の対象とする。
波形・音質は依頼により対象外。

## 再実行

```powershell
C:/msys64/mingw32/bin/g++.exe -std=c++20 -O2 -static -Isrc src/harness/bench_sound_devices.cpp -o build_logs/bench_sound_devices.exe -ldsound -lole32 -ldxguid
./build_logs/bench_sound_devices.exe
# 列挙された番号を指定。最後の1で最初を準備ありへ反転する。出力は未作成のパス。
./build_logs/bench_sound_devices.exe 0 build_logs/device_new.csv 1
python -X utf8 src/harness/analyze_sound_devices.py <device_*.csvを格納したフォルダー>
```
